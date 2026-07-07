# Phase 1a.3 — DSP V5 (channel strips + matrix mixer 8×16)

**Status** : proposition initiale soumise pour investigation 6 workers.
**Date** : 2026-04-27
**Branche** : `feature/audio-platform-v2`
**Précédent** : V4.2 production figée (master chain stereo seulement, pas d'effets per-voie, pas de matrix). V3.2.2 NPU tap end-to-end validé.

---

## 1. Objectif

Compléter le DSP HiFi4 i.MX8MP avec :

1. **Channel strips per-voie** (×8) : EQ paramétrique 4 bandes + compresseur + volume + pan + mute
2. **Matrix mixer 8 sorties × 16 entrées** : 16 sources possibles (8 mics post-effets entrée + 8 ASIO USB retours), 8 destinations (HP/monitor + future USB sends)
3. **Master chain V4.2 conservée** intacte (multiband_drc + pga + drc + NPU tap V3.2.2)
4. **Pré-requis SDRAM2 128 MB DSP-only @ 0x80000000** ajoutée pour faire entrer tout ça

C'est l'étape **décisive** du projet : V5 = la console mastering live elle-même, vraie (pas juste passthrough). Le user attend des effets indépendants par voie + matrice de routage avant de pouvoir piloter le NPU.

## 2. Pré-requis : extension mémoire SDRAM2

### 2.1 Pourquoi : V4.2 sature SDRAM1 à 99.66 %

Le firmware V3.2.2.1 utilise déjà `sdram1: 8144 KB / 8172 KB (99.66%)`. Ajouter 8× eq_iir + 8× drc + matrix dépasse la capacité.

### 2.2 Découverte clé : 0x80000000-0x9FFFFFFF = 512 MB cacheable WT

Cacheattr Xtensa (`imx8m.x.in:166` `_memmap_cacheattr_imx8_wt_allvalid = 0x22212222`) donne digit 4 = `0x1` = write-through pour la plage `0x80000000-0x9FFFFFFF`. C'est la **seule** plage cacheable pour le DSP HiFi4. Hors de cette plage = bypass non-cacheable (utilisable mais lent).

Kernel rproc translation table (`drivers/remoteproc/imx_dsp_rproc.c:170`) :
```c
{ 0x40000000, 0x40000000, 0x80000000, 0 }, /* DDR : 2 GB visible par DSP */
```
Le DSP peut adresser DDR de 0x40000000 à 0xBFFFFFFF (2 GB). Cacheable WT seulement dans 0x80000000-0x9FFFFFFF.

### 2.3 Layout V5 final

```
0x40000000  ┌──────────────────────────────┐  DDR start (Linux)
            │  Linux DDR usage             │
0x50000000  ├──────────────────────────────┤
            │  M7 zone (32 MB, optionnel)  │  réservé futur M7
0x58000000  ├──────────────────────────────┤
            │  Linux DDR usage             │
0x80000000  ╞══════════════════════════════╡ ◄── ENTRÉE region 4 = WT
            │ ◆ SDRAM2  128 MB DSP-only ◆ │   no-map Linux, cacheattr WT
            │   = HEAP_BUFFER géant V5     │   pour eq_iir + drc + matrix
0x88000000  ├──────────────────────────────┤
            │  Linux DDR usage             │
0x92400000  ├── SDRAM0 8 MB (V4.2 code) ───┤  inchangé
0x92C00000  ├── SDRAM1 8 MB (V4.2 heaps) ──┤  inchangé
0x933FFFFF  ├──────────────────────────────┤
            │  dsp_reserved_heap (libre)   │
0x942B0000  ├── npu_tap_buffer 256 KB ─────┤  V3.2.2 Linux-shared (mmap)
0x942FFFFF  ├──────────────────────────────┤
            │  Linux DDR usage             │
0xC0000000  └──────────────────────────────┘  DDR end côté DSP
```

128 MB à 0x80000000-0x87FFFFFF : largement dans la WT region 4, no-map côté Linux, accès direct DSP.

### 2.4 Stratégie d'allocation

**Approche fallback transparent (option b)** : SOF alloue d'abord dans `buffer[0]` (SDRAM1 actuel), et bascule automatiquement sur `buffer[3]` (SDRAM2 nouveau) quand buffer[0] est plein. Pas besoin de tagger les comps avec un caps spécial.

L'allocator SOF (`sof/src/lib/alloc.c:_balloc_unlocked`) parcourt déjà les `buffer[N]` séquentiellement et choisit le premier qui matche les caps demandés. Avec un nouveau `buffer[3]` aux mêmes caps `RAM | DMA | CACHE`, il sera utilisé en fallback automatique.

## 3. Architecture cible V5

```
                        SAI7 RX (8ch raw mics post-TAC ADC)
                           │
                           ├──► PCM SAI_Capture (host, mics secs)
                           │
                           ▼  ×8 instances per-voie en parallèle :
                    ┌──────────────────────────┐
                    │ eq_iir 4 bandes ch_n     │  EQ paramétrique
                    └──────┬───────────────────┘
                           ▼
                    ┌──────────────────────────┐
                    │ drc ch_n                 │  compresseur de piste
                    └──────┬───────────────────┘
                           ▼
                    ┌──────────────────────────┐
                    │ volume + pan + mute ch_n │  fader stéréo
                    └──────┬───────────────────┘
                           ▼
USB1 IN 8ch  ───────►──┐  │  (ces 8 + les 8 du SAI = 16 sources matrix)
                       │  │
   ┌───────── Matrix mixin/mixout 8 × 16 ─────────┐
   │ cellules gain dB par couple (in_i, out_j)    │
   └────┬─────────────────┬────────────────┬──────┘
        │                 │                │
        ▼                 ▼                ▼
   MASTER stereo bus    Direct out 8ch   Future USB send
   [V4.2 conservée :]                   (Phase 2)
   mixin → multiband_drc → pga → drc
        │
        ▼ (NPU tap V3.2.2 inchangé)
   PCM Master_Tap (NPU)
        │
        ▼
   Spread 2→8 → SAI7 TX 8ch ──► 4× TAC5212 DAC
```

**Composants SOF mobilisés** :
- **`eq_iir`** ×8 (`COMP_IIR=y`) : 4 bandes paramétriques (low-shelf + 2 peak + high-shelf), 3 biquads minimum
- **`drc`** ×8 (`COMP_DRC=y`) : compresseur de piste (threshold/ratio/attack/release/makeup)
- **`volume`** ×8 (`COMP_VOLUME=y`) : fader dB + soft-ramp
- Pan : à investiguer — pas de comp natif `pan`, possibilité via mixin/mixout avec route_matrix non-identité, ou via deux instances de volume L/R sur un comp_buffer stereo
- **`mixin/mixout`** ou **`mixer`** : matrix 8×16 — IPC3 mixer limité à 2 sources (= V4.2 conf), donc à investiguer si on peut utiliser mixin/mixout en IPC3 ou si on a besoin de plusieurs `mixer` cascadés
- **`multiband_drc`** + **`pga`** + **`drc`** master : conservé identique V4.2
- **NPU tap V3.2.2** : conservé via hook firmware déjà déployé

## 4. Spec détaillée

### 4.1 Inputs matrix (16 sources)

| ID | Source | Phase | Origine |
|---|---|---|---|
| 0..7 | SAI in 1..8 (post-effets per-voie : eq_iir + drc + volume) | 1a.3 | mics XLR / PDM |
| 8..15 | USB1 ASIO in 1..8 | 2 (USB gadget) | DAW host |

NB : les 8 inputs USB ne sont disponibles qu'après la Phase 2 USB UAC2 gadget. Pour V5 immédiat, on peut juste **réserver les routes 8..15 dans la matrice** sans qu'elles soient connectées à des PCMs. La structure matrix 8×16 est prête mais 8 colonnes sont silencieuses jusqu'à Phase 2.

### 4.2 Outputs matrix (8 destinations)

| ID | Sortie | Description |
|---|---|---|
| 0 | Master L (vers chain V4.2 stereo) | bus master |
| 1 | Master R (vers chain V4.2 stereo) | bus master |
| 2..7 | Direct out ch 3..8 (vers SAI TX directement, ou réservé) | monitor/HP individuels |

### 4.3 ALSA controls exposés (V5)

Pour chaque voie n=1..8 :
- `SAI In <n> EQ Band <m> Gain` (m=1..4, dB TLV -24..+24)
- `SAI In <n> EQ Band <m> Freq` (m=1..4, Hz)
- `SAI In <n> EQ Band <m> Q` (m=1..4, Q6.2)
- `SAI In <n> EQ Band <m> Type` (low-shelf/peak/high-shelf)
- `SAI In <n> Compressor Threshold` (dB)
- `SAI In <n> Compressor Ratio`
- `SAI In <n> Compressor Attack` (ms)
- `SAI In <n> Compressor Release` (ms)
- `SAI In <n> Compressor Makeup Gain` (dB)
- `SAI In <n> Volume` (dB TLV)
- `SAI In <n> Pan` (-100..+100)
- `SAI In <n> Mute` (bool)

Pour la matrix :
- `Matrix Cell In=<i> Out=<j>` (i=0..15, j=0..7, gain dB TLV)

Master (existant V4.2) :
- `MULTIBAND_DRC1.0`, `PGA1.0`, `DRC1.0`, etc. inchangés

### 4.4 Topology m4 V5 — fichier `sof-imx8mp-tac5212-v5.m4`

À écrire intégralement, basé sur :
- `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212-drc.m4` (V4.2 production = base)
- Patterns SOF : `sof/tools/topology/topology1/sof-smart-amplifier.m4` (référence multi-pipeline)

### 4.5 Estimation MIPS DSP

HiFi4 @ 800 MHz ≈ 800 MIPS pic.

| Composant | Coût estimé / instance | ×8 |
|---|---|---|
| eq_iir 4 bandes | ~3 MIPS | 24 MIPS |
| drc | ~5 MIPS | 40 MIPS |
| volume + pan | ~1 MIPS | 8 MIPS |
| mixin (input) | ~2 MIPS | 16 MIPS |
| mixout (output) | ~2 MIPS | 16 MIPS |
| Master chain V4.2 (mbdrc + pga + drc) | ~30 MIPS | 30 MIPS |
| NPU tap memcpy + memw | ~1 MIPS | 1 MIPS |
| **TOTAL** | | **~135 MIPS** |

Marge confortable : 17% du DSP utilisé. **Pas de risque de surcharge**.

### 4.6 Estimation mémoire

Pour 8 voies + matrix + master, comp_buffer audio (ring buffers) :
- 8 × buffer s32_le 2ch (post-pan) × 2 périodes × 2ms × 48kHz × 4B = 8 × 1536 × 2 = 24 KB
- 16 × buffer mixin source × 2 périodes = 49 KB
- 8 × buffer mixout output × 2 périodes = 24 KB
- Master 2ch buffers (déjà V4.2, ~6 KB)
- Comp metadata structures × ~30 instances × 256 B = 8 KB
- **Total estimé** : ~120 KB de comp_buffer audio + métadonnées

Plus les coefficients (eq_iir tables, drc params) et les state internes des composants : ~500 KB-1 MB.

→ **Largement < 128 MB SDRAM2**. La majorité de SDRAM2 sera inutilisée mais c'est de la marge confort pour Phase 2 (USB) et Phase 3 (FX returns).

## 5. Plan d'implémentation V5 (incremental, gates strictes)

### 5.1 Pré-requis — E0.5 SDRAM2 extension

| Sous-étape | Action | Test gate |
|---|---|---|
| **E0.5.a** | Script DT `apply-sdram2-dt.py` (ajout reserved-memory `sdram2_reserved@80000000` 128 MB no-map) | `cat /proc/iomem` → entrée à 0x80000000, `MemTotal` baisse de 128 MB, Linux boot OK |
| **E0.5.b** | SOF `memory.h` : `+#define SDRAM2_BASE 0x80000000`, `+#define SDRAM2_SIZE 0x8000000` | Compile OK |
| **E0.5.c** | SOF `imx8m.x.in` : ajouter `sof_sdram2 (rw) : org = SDRAM2_BASE, len = SDRAM2_SIZE` | west build map output montre nouvelle bank |
| **E0.5.d** | SOF `memory.c` : `+.buffer[3] = { ... .heap = SDRAM2_BASE, .size = SDRAM2_SIZE, .caps = SOF_MEM_CAPS_RAM \| DMA \| CACHE };` + `PLATFORM_HEAP_BUFFER 3 → 4` | Sign + deploy, dmesg "Firmware info" propre |
| **E0.5.e** | V4.2 régression PASS (T1-T5 inchangés) | OK obligatoire |
| **E0.5.f** | NPU tap V3.2.2 toujours fonctionnel (write_idx flow ~1.54 MB/s) | OK obligatoire |
| **E0.5.g** | Test allocation forcée dans buffer[3] (créer un comp_buffer de 8 MB pour saturer buffer[0] et vérifier fallback SDRAM2) | Allocation à `[0x80000000-0x88000000]` confirmée |

**Gate E0.5** : tout PASS avant E1.

### 5.2 Implementation V5 par étapes

| Étape | Composant ajouté | Test gate |
|---|---|---|
| **E1** | EQ_IIR ×8 sur capture path (mics → eq_iir → buf → host SAI_Capture) | V4.2 régression PASS, capture audio non-corrompue, EQ controls répondent à `amixer cset` |
| **E2** | DRC ×8 (compresseur après eq_iir) | V4.2 PASS, charge DSP < 60%, controls drc fonctionnels |
| **E3** | Volume + pan ×8 | V4.2 PASS, fader/pan audible |
| **E4** | Matrix mixin/mixout 8×16 (8 SAI in actifs + 8 USB in en placeholder) | Routage testable (ex: mic 1 vers out 3 + master), V4.2 PASS |
| **E5** | Re-attacher master chain V4.2 + NPU tap V3.2.2 sur la sortie de matrix master L/R | NPU tap toujours fonctionnel, V4.2 PASS |
| **E6** | Stress 10 min sustain | 0 underrun, 0 race, charge DSP stable, V4.2 PASS |

Chaque étape : commit isolé, topology .tplg version-able, rollback rapide en re-deployant le .tplg précédent + reboot.

### 5.3 Plan B (si une étape échoue)

- **E1 KO (eq_iir crash)** : rollback topology V4.2, investigation Kconfig EQ_IIR options
- **E2 KO (drc crash)** : idem
- **E3 KO (pan absent)** : implémenter pan via 2 volume L/R + scratch buffer, OU via mixin matrix gain (= matrix DOIT supporter cellules per-channel)
- **E4 KO (matrix limitée IPC3)** : fallback `mux` (= matrix simple) ou cascade de `mixer` 2-source
- **E5 KO (NPU tap cassé)** : revoir interaction master chain ↔ tap V3.2.2 hook position

## 6. Risques identifiés

| Risque | Probabilité | Mitigation |
|---|---|---|
| 0x80000000 inaccessible Linux ↔ DSP no-map | Faible | DSP-only, pas de sharing Linux requis pour V5 |
| Linux refuse no-map @0x80000000 (déjà en heap CMA) | Moyenne | Vérifier en avance `bootargs mem=` et `/proc/iomem`. Fallback : 0x84000000 ou 0x88000000 |
| `mixin/mixout` IPC3 indispo sur SOF Zephyr i.MX8MP | Moyenne | Investigation préalable — utiliser `mux` ou `mixer` cascadé si besoin (cf. V4.2 doc Phase 1a §5.10 : « `mux` permet routage (in_stream, in_ch) → out_ch » confirmé IPC3) |
| `eq_iir` charge DSP > prévu (4 bandes coûteuses) | Faible | Budget MIPS large (135/800), mesurable runtime. Réduire à 3 bandes si besoin |
| Régression V4.2 sur passage par matrix | **Haute** | Tests E1→E6 stricts à chaque étape, rollback topology disponible |
| NPU tap se retrouve attaché sur le mauvais buffer post-matrix | Moyenne | Le hook V3.2.2 est dans `dai_dma_cb()` côté SAI TX = INVARIANT, peu importe la topology amont. Vérifier empiriquement. |
| Kernel pgprot/cache issue sur 0x80000000 si on partage avec Linux plus tard | Hors scope V5 | DSP-only pour l'instant |

## 7. Hors scope (futurs)

- **Phase 2 USB UAC2 gadget** : alimenter les 8 inputs USB de la matrix (placeholders en V5)
- **Phase 3 FX send LV2 sur A53** : 4 bus aux send/return via JACK + plugins LV2
- **Phase 4 NPU mastering closed-loop** : modèle TFLite + service Python + write controls

V5 prépare l'infrastructure pour toutes ces phases (matrix, channel strips, routing flexible).

## 8. Validation

### 8.1 Investigation 6 workers (avant code)

Soumettre cette spec au critic.io (si MCP back online) avec questions :

Q1. Layout SDRAM2 @ 0x80000000 + 128 MB est-il optimal ? Risque cache coherency Linux (no-map suffit-il, ou faut-il config DMA-coherent côté A53 si jamais on partage plus tard) ?
Q2. Caps `SOF_MEM_CAPS_RAM | SOF_MEM_CAPS_DMA | SOF_MEM_CAPS_CACHE` corrects pour buffer[3] ? Faut-il aussi `SOF_MEM_CAPS_HEAP_EXT` ou un autre flag ?
Q3. Stratégie fallback transparent : SOF allocator parcourt vraiment `buffer[]` séquentiellement et bascule sur buffer[3] quand buffer[0] est plein ?
Q4. eq_iir 4 bandes + drc + volume × 8 voies : layout pipeline optimal ? Une grosse pipeline `pipe-channel-strip-x8.m4` ou 8 pipelines indépendantes co-scheduled ?
Q5. Pan via `volume` 2-instance (L/R) ou via mixin matrix non-identité ? Quelle approche est canonique en SOF ?
Q6. Matrix 8×16 en IPC3 : possible avec mixin/mixout, ou besoin de cascade `mixer` (limité 2 sources) ? `mux` peut-il faire 16 sources × 8 sinks ?
Q7. NPU tap V3.2.2 hook reste correct si la topology amont change radicalement (master chain alimentée par matrix au lieu de PCM_host direct) ?
Q8. MIPS budget réaliste à 800 MHz pour 135 MIPS estimés ?
Q9. Plan E0.5 → E1-E6 incremental sain ? Risques particuliers à prévenir ?
Q10. Risques OUBLIÉS dans la spec ?

### 8.2 Tests hardware (post-code)

- Tests V4.2 régression T1-T5 + nouveau tests V5 (EQ controls, DRC controls, matrix routing)
- Stress 10 min sustain
- NPU tap V3.2.2 stress 10 min en parallèle
- Mesure latence end-to-end (vs V4.2 baseline)
- Vérification audio bit-perfect quand controls neutres (= passthrough)

## 9. Dépendance / lien avec V3.2.2

V5 **n'invalide pas** V3.2.2. Le hook NPU tap firmware est dans `dai_dma_cb()` côté SAI7 TX, **après le master chain**, donc :

- V5 ajoute des composants AVANT le master chain (channel strips + matrix → master)
- Master chain inchangé V4.2 (multiband_drc + pga + drc)
- Hook V3.2.2 inchangé : capture le buffer post-master juste avant SAI TX
- → NPU continuera de voir l'audio post-effets master complet

**Aucune modification firmware nouvelle pour le tap** : V3.2.2 reste tel quel, V5 = topology m4 + Kconfig + memory.

## 10. Référence

- Topology V4.2 production : `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212-drc.m4`
- Spec NPU tap V3.2.2 : `PHASE_1A_2_NPU_TAP_V3.2.2.md`
- État global projet : `PROJECT_STATE.md`
- Plan platform global : `MASTERING_PLATFORM_PLAN.md` (Phase 1 §5)
- Manuel développeur : `docs/MANUEL_DEV_PHASE_1A_2.pdf`
- Pattern multi-pipeline référence : `sof/tools/topology/topology1/sof-smart-amplifier.m4`
- Compétents SOF Kconfig : `sof/src/audio/eq_iir/Kconfig`, `sof/src/audio/drc/Kconfig`, `sof/src/audio/mux/Kconfig`, `sof/src/audio/mixer/Kconfig`
