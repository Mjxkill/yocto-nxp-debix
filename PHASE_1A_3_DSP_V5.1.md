# Phase 1a.3 — DSP V5.1 (channel strips ×8 + matrix mixer16 8×16 + SDRAM2 128 MB @ 0xA0000000)

**Status** : V5 RÉVISER 6/6 par investigation `ef3c1ede-4bba-45f1-8b70-d8938907e693`. V5.1 = V5 + corrections issues investigation, vise GO unanime.
**Date** : 2026-04-27
**Branche** : `feature/audio-platform-v2`
**Précédent** : V4.2 production figée + V3.2.2 NPU tap validé hardware. V5 obsolète (blocages structurels gpu_reserved + matrix IPC3).

---

## 1. Synthèse des corrections V5 → V5.1

| # | Correction | Source verdict |
|---|---|---|
| **C1** | **SDRAM2 déplacée 0x80000000 → 0xA0000000** (collision V5 avec `gpu_reserved` confirmée par claude-code DT lecture). User a tranché : utiliser 0xA0000000 pour préserver le GPU. | claude-code, glm-5.1, deepseek-v4-pro |
| **C2** | **Cacheattr modifié** : `0x22212222 → 0x22112222` (digit 5 : `2 → 1`) pour activer write-through sur region 5 (0xA0000000-0xBFFFFFFF). 1 char dans `imx8m.x.in:166`. | déduction physique cacheattr Xtensa |
| **C3** | **Nouveau comp `mixer16` séparé** (au lieu de patcher `mixer` upstream). V4.2 utilise toujours `mixer` 2-source classique. V5.1 utilise `mixer16` — pattern N→M avec gains par cellule. ~150 LOC, IPC3 compatible. | tous les workers + user décision |
| **C4** | **`CONFIG_COMP_IIR=y` à ajouter** dans `imx8mp_evk_mimx8ml8_adsp.conf` (manquant pour eq_iir ×8) | glm-5.1, kimi |
| **C5** | **`PLATFORM_HEAP_BUFFER 3 → 4`** + vérifier dimension `struct mm.buffer[]` dans `mm_heap.h` | glm-5.1, deepseek |
| **C6** | **DT `memory-region` du noeud `dsp@3b6e8000`** doit inclure `&sdram2_reserved` pour rproc translation (sinon DSP n'accède pas à 0xA0000000) | deepseek |
| **C7** | **Linker `imx8m.x.in`** doit déclarer `sof_sdram2` dans MEMORY{} avant que `memory.c` y référence buffer[3] | 4/6 workers |
| **C8** | **Pan via 2 instances volume L/R avec gains croisés** (pas de comp pan natif, mixin/mixout IPC4-only) | tous workers |
| **C9** | **Plan E réordonné : E4 (matrix) AVANT E3 (volume/pan)**. Matrix = composant le plus risqué, mieux vaut le découvrir tôt | tous workers |
| **C10** | **Garantir 8ch S32_LE en sortie master chain** pour que NPU tap V3.2.2 reste invariant (period_bytes=3072) | claude-code, glm-5.1, deepseek |
| **C11** | **/proc/iomem pré-check** : confirmer que 0xA0000000-0xA8000000 est libre côté Linux avant carve no-map | tous workers |
| **C12** | **Description allocator fallback corrigée** dans la spec : c'est un parcours séquentiel `buffer[0..N]` par caps matching + alloc test, pas une "bascule intelligente" | glm-5.1, deepseek |
| **C13** | **Pattern channel strip** : 8 pipelines indépendantes co-scheduled (PIPELINE_SCHED_COMP_N) au lieu d'une grosse pipeline. Pas de macro M4 forloop, écrire les 8 blocs PIPELINE_PCM_ADD à la main. | claude-code, glm-5.1 |

## 2. Architecture cible V5.1 (inchangée vs V5 sauf adresse SDRAM2)

```
                        SAI7 RX (8ch raw mics post-TAC ADC)
                           │
                           ├──► PCM SAI_Capture (host, mics secs)
                           │
                           ▼  ×8 pipelines indépendantes co-scheduled :
                    ┌──────────────────────────┐
                    │ eq_iir 4 bandes ch_n     │  EQ paramétrique
                    └──────┬───────────────────┘
                           ▼
                    ┌──────────────────────────┐
                    │ drc ch_n                 │  compresseur de piste
                    └──────┬───────────────────┘
                           ▼
                    ┌──────────────────────────┐
                    │ 2× volume L/R ch_n       │  fader + pan (gains croisés)
                    └──────┬───────────────────┘
                           ▼
USB1 IN 8ch  ───────►──┐  │  (8 placeholders en V5.1, alimentés Phase 2)
                       │  │
   ┌── mixer16 (NEW comp) — Matrix 16 sources × 8 sinks ─┐
   │   gain par cellule (i=0..15, j=0..7) en Q1.31      │
   │   ALSA control 'Matrix Cell In=<i> Out=<j>'         │
   └────┬─────────────────┬────────────────┬─────────────┘
        │                 │                │
        ▼                 ▼                ▼
   MASTER stereo bus    Direct out 6ch   (futur USB send)
   [V4.2 conservée :]   slots 3..8       Phase 2
   mixin → multiband_drc → pga → drc
        │
        ▼ (conserver 8ch S32_LE en sortie pour NPU tap V3.2.2)
   ┌── spread 2→8 (mixer placement to 8 SAI slots) ──┐
   │   slot 0=L, slot 1=R, slot 2..7 = direct outs  │
   └─────────────────────┬───────────────────────────┘
                         ▼
   [hook NPU tap V3.2.2 dai_dma_cb()]
                         ▼
   PCM Master_Tap (host) ◄── via 0x942B0000 mmap
                         ▼
   SAI7 TX 8ch ──► 4× TAC5212 DAC
```

**Composants SOF mobilisés** :
- `eq_iir` ×8 (`COMP_IIR=y` à ajouter) : 4 bandes paramétriques, 1 instance par voie mono
- `drc` ×8 (`COMP_DRC=y` existant V4.2) : compresseur de piste
- `volume` ×16 (`COMP_VOLUME=y` existant) : 2 instances L/R par voie pour pan stereo
- **`mixer16` ×1 (NEW)** : matrix 16×8 avec gains par cellule, IPC3 compatible
- `multiband_drc + pga + drc` master : V4.2 conservé inchangé
- NPU tap V3.2.2 : conservé via hook firmware déjà déployé
- `spread` (cf. V4.2) ou mixer placement : extension stereo master vers 8 slots SAI TX

## 3. Pré-requis : extension SDRAM2 128 MB @ 0xA0000000

### 3.1 Layout DDR définitif

```
0x40000000  ┌──────────────────────────────┐  DDR région A start (Linux 352 MB visible)
            │  Linux usage                 │
0x50000000  ├──────────────────────────────┤
            │  M7 zone (32 MB, futur)      │  réservé M7 si activé un jour
0x56000000  ├──────────────────────────────┤
            │  HOLE M7                     │  invisible Linux
0x58000000  ├──────────────────────────────┤  DDR région B start (Linux 2.625 GB visible)
            │  Linux usage                 │
0x80000000  ├── gpu_reserved 256 MB ───────┤  conservé pour Wayland/GPU (user le veut)
0x90000000  ├──────────────────────────────┤
            │  Linux usage                 │
0x92400000  ├── SDRAM0 8 MB (V4.2 code) ───┤  inchangé
0x92C00000  ├── SDRAM1 8 MB (V4.2 heaps) ──┤  inchangé (HEAP_BUFFER plein 99.66%)
0x933FFFFF  ├──────────────────────────────┤
            │  dsp_reserved_heap (libre)   │
0x942B0000  ├── npu_tap_buffer 256 KB ─────┤  V3.2.2 Linux-shared (mmap)
0x942FFFFF  ├──────────────────────────────┤
            │  Linux usage                 │
0xA0000000  ╞══════════════════════════════╡ ◄── ◆ SDRAM2 V5.1 ◆ DSP-only
            │ ◆ SDRAM2  128 MB DSP-only ◆ │   no-map Linux
            │   cacheattr region 5 = WT    │   write-through cacheable (modif imx8m.x.in)
            │   = HEAP_BUFFER géant V5.1   │   pour eq_iir + drc + volumes + mixer16
0xA8000000  ├──────────────────────────────┤
            │  Linux usage                 │
0xC0000000  └──────────────────────────────┘  fin DDR
```

### 3.2 Modif cacheattr Xtensa (C2)

`sof/src/platform/imx8m/imx8m.x.in:166` :
```diff
- _memmap_cacheattr_imx8_wt_allvalid = 0x22212222;
+ _memmap_cacheattr_imx8_wt_allvalid = 0x22112222;   /* V5.1: digit 5 (region 5) 2→1 = WT */
```

Décodage final :
- digit 4 (region 4 = 0x80000000-0x9FFFFFFF) = `1` = WT (V4.2 inchangé : code SOF + npu_tap_buffer)
- digit 5 (region 5 = 0xA0000000-0xBFFFFFFF) = `1` = WT (V5.1 NEW : SDRAM2)
- autres digits = `2` = bypass (inchangé)

**Impact** : aucun, car aucun comp DSP n'accède à 0xA0000000-0xBFFFFFFF avant V5.1. La modif active simplement le caching pour la nouvelle zone.

### 3.3 Modifications par fichier

| Fichier | Modif | Effort |
|---|---|---|
| `meta-local/recipes-kernel/linux/files/apply-sdram2-dt.py` (NEW) | Ajout reserved-memory `sdram2_reserved@a0000000 + 128 MB no-map` + ajout dans `&{/dsp@3b6e8000} memory-region = <... &sdram2_reserved>` | ~50 lignes Python (pattern apply-npu-tap-dt.py) |
| `meta-local/recipes-kernel/linux/linux-imx_%.bbappend` | `+SRC_URI += "file://apply-sdram2-dt.py"` + appel dans `do_patch:append` | 2 lignes |
| `sof/src/platform/imx8m/include/platform/lib/memory.h` | `+#define SDRAM2_BASE 0xA0000000`, `+#define SDRAM2_SIZE 0x8000000`, `-PLATFORM_HEAP_BUFFER 3` `+PLATFORM_HEAP_BUFFER 4` | 4 lignes |
| `sof/src/platform/imx8m/imx8m.x.in` | `+sof_sdram2 (rw) : org = SDRAM2_BASE, len = SDRAM2_SIZE` au MEMORY{} + cacheattr `0x22112222` | 4 lignes |
| `sof/src/platform/imx8m/lib/memory.c` | `+.buffer[3] = { .heap = SDRAM2_BASE, .size = SDRAM2_SIZE, .caps = SOF_MEM_CAPS_RAM \| DMA \| CACHE };` + heap_map associé | ~15 lignes |
| `sof/lib/mm_heap.h` (vérification) | Confirmer `struct mm.buffer[PLATFORM_HEAP_BUFFER]` est dimensionné dynamiquement (pas hardcodé à 3) | lecture seule |
| `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` | `+CONFIG_COMP_IIR=y` (C4) | 1 ligne |

### 3.4 Stratégie d'allocation (C12 description corrigée)

**Comportement réel de `_balloc_unlocked` (alloc.c:908)** :
```c
for (i = 0, n = PLATFORM_HEAP_BUFFER, heap = memmap->buffer;
     i < PLATFORM_HEAP_BUFFER;
     i = heap - memmap->buffer + 1, n = PLATFORM_HEAP_BUFFER - i,
     heap++) {
    heap = get_heap_from_caps(heap, n, caps);
    if (!heap) break;
    ptr = alloc_heap_buffer(heap, flags, caps, bytes, alignment);
    if (ptr) break;
}
```

= parcours séquentiel `buffer[0..N]` par caps matching + alloc test :
- Si `buffer[0]` (SDRAM1, caps `RAM|DMA|CACHE`) plein → `alloc_heap_buffer` retourne NULL
- Boucle passe à `buffer[1]` (DRAM0, caps `RAM|DMA|HP`) — caps ne matchent pas `CACHE` → skip
- Idem `buffer[2]` (DRAM1) → skip
- Arrive à `buffer[3]` (SDRAM2, caps `RAM|DMA|CACHE`) → matche → alloc → succès

**OK fonctionnellement**, mais ce n'est PAS une "bascule intelligente" — c'est un parcours séquentiel.

## 4. Composant `mixer16` (NEW, C3)

### 4.1 Pourquoi pas patcher `mixer` upstream ?

- `mixer` IPC3 est limité à 2 sources par design upstream (`MIXER_MAX_SOURCES = 2` dans `mixer.h:33`)
- V4.2 utilise `mixer` 2-source classique pour son master chain
- Modifier `mixer.h` upstream introduirait une régression V4.2 et casserait la compatibilité
- Mieux : nouveau composant `mixer16` qui coexiste avec `mixer`

### 4.2 Spec `mixer16`

**Fichiers à créer** :
- `sof/src/audio/mixer16/mixer16.h` (struct config + IPC payload)
- `sof/src/audio/mixer16/mixer16.c` (comp_driver + ipc handlers)
- `sof/src/audio/mixer16/mixer16_generic.c` (mix_n_s32 avec gains/cellule)
- `sof/src/audio/mixer16/Kconfig` (`CONFIG_COMP_MIXER16=y`)
- `sof/src/audio/mixer16/CMakeLists.txt`
- Patch `sof/src/audio/CMakeLists.txt` pour inclure mixer16

**Constantes** :
```c
#define MIXER16_MAX_SOURCES   16
#define MIXER16_MAX_SINKS     8
#define MIXER16_GAIN_BITS     31      /* Q1.31 fixed-point */
```

**Struct config** (bytes blob ALSA control) :
```c
struct mixer16_cell_gains {
    int32_t gain[MIXER16_MAX_SOURCES][MIXER16_MAX_SINKS];   /* Q1.31 */
};
```

**Mix loop** (mixer16_generic.c) :
```c
static void mixer16_s32_default(struct comp_dev *dev, ...)
{
    struct mixer16_cd *cd = comp_get_drvdata(dev);
    int32_t (*gain)[MIXER16_MAX_SINKS] = cd->cell_gains;

    for (frame = 0; frame < n_frames; frame++) {
        for (out_j = 0; out_j < n_sinks; out_j++) {
            int64_t acc = 0;
            for (in_i = 0; in_i < n_sources; in_i++) {
                acc += ((int64_t)src[in_i][frame] * gain[in_i][out_j]) >> 31;
            }
            dst[out_j][frame] = sat_int32(acc);
        }
    }
}
```

**ALSA controls exposés** :
- `Matrix Cell In=<i> Out=<j>` (i=0..15, j=0..7) — chaque cellule = 1 contrôle integer TLV dB
- 16×8 = **128 contrôles** (raisonnable, exposés via topology m4)

OU alternative plus compacte :
- `Matrix Gains` (1 seul bytes blob 128×4 B = 512 B) — modifié via `amixer cset` avec un payload structured
- Plus efficace pour batch update mais moins user-friendly via amixer

**Recommandation** : commencer avec bytes blob (1 control), ajouter 128 contrôles individuels en Phase 4 si UI nécessite.

### 4.3 Estimation MIPS `mixer16` 8 sinks × 16 sources @ 48 kHz

- 16 multiplications + 15 additions × 8 sinks × 96 frames/period × 500 Hz period rate = **~12 MIPS** (HiFi4 SIMD : ~1 cycle/MAC)
- + buffer_copy_to/from = ~2 MIPS
- **Total mixer16 ≈ 15 MIPS** sur 800 MIPS budget

### 4.4 Validation

- Bit-perfect : matrice identité (gain unitaire en diagonale, 0 ailleurs) → audio passthrough comme V4.2
- Fonctionnel : `amixer cset name='Matrix Gains' <blob>` change le routage en runtime
- Pas de race condition : updates de gains via `comp_data_blob` SOF (mécanisme atomique existant)

## 5. Plan d'implémentation V5.1 (réordonné, C9)

| Étape | Action | Test gate |
|---|---|---|
| **E0.5.a** | Pré-check : `cat /proc/iomem` sur board, vérifier 0xA0000000-0xA8000000 libre | Zone non listée → OK |
| **E0.5.b** | DT `apply-sdram2-dt.py` : ajout reserved-memory @0xA0000000 + ajout au memory-region du dsp node | `cat /proc/iomem` show "a0000000-a7ffffff : reserved", MemTotal -128 MB |
| **E0.5.c** | SOF `memory.h` (SDRAM2_BASE/SIZE + PLATFORM_HEAP_BUFFER 4) + `imx8m.x.in` (cacheattr `0x22112222` + sof_sdram2 MEMORY{}) + `memory.c` (buffer[3]) | Build OK |
| **E0.5.d** | Sign + deploy + reboot | dmesg "Firmware info" propre, V4.2 régression PASS, NPU tap V3.2.2 PASS |
| **E0.5.e** | Test allocation forcée dans buffer[3] : créer comp_buffer ~7 MB pour saturer SDRAM1 → next alloc bascule SDRAM2 | trace adresse dans `[0xA0000000, 0xA8000000]` |
| **E1** | Implémentation `mixer16` (composant SOF NEW, ~150 LOC) + Kconfig + CMakeLists | Build OK avec `CONFIG_COMP_MIXER16=y`, mixer16_init compile |
| **E2** | Topology V5.1 minimaliste : 8 mics SAI → mixer16 16×8 (8 placeholders USB en silence) → master chain V4.2 → SAI TX. Test routage avec gains identité | Audio passthrough OK, controls Matrix répondent à amixer cset |
| **E3** | Ajout `eq_iir` ×8 entre SAI capture et mixer16 | EQ controls fonctionnels, V4.2 régression PASS, charge DSP < 30% |
| **E4** | Ajout `drc` ×8 après eq_iir | DRC controls OK, charge DSP < 50% |
| **E5** | Ajout 2× `volume` L/R par voie pour pan, après drc | Pan audible, charge DSP < 60% |
| **E6** | Validation NPU tap V3.2.2 toujours fonctionnel | period_bytes = 3072 stable, magic NPAT, write_idx flow |
| **E7** | Stress 10 min (aplay loop + amixer changes parallèles) | 0 underrun, 0 race, charge DSP stable, V4.2 régression PASS |

Chaque étape : commit isolé, topology .tplg version-able, rollback en re-deployant le .tplg précédent.

## 6. Risques résiduels

| Risque | Probabilité | Mitigation |
|---|---|---|
| 0xA0000000 utilisé déjà par CMA Linux | **Faible** | Pré-check E0.5.a + reserved-memory no-map traité par memblock_reserve() avant buddy init |
| Cacheattr modif region 5 casse autre chose | **Très faible** | Region 5 (0xA0000000-0xBFFFFFFF) n'est pas utilisée par le DSP avant V5.1, modif local au build firmware |
| `mixer16` bug (nouveau code C ~150 LOC) | **Moyen** | Tests bit-perfect avec matrice identité avant gains réels, code review post-implé |
| 8 instances eq_iir/drc en parallèle DSP overload | **Faible** | Budget MIPS 165/800 = 21%, marge confortable |
| NPU tap cassé par changement format master | **Faible** | C10 garantit 8ch S32_LE en sortie master via spread 2→8 inchangé |
| DT `memory-region` du dsp node invalidé par carve sdram2 | **Faible** | C6 add &sdram2_reserved au memory-region |

## 7. Hors scope V5.1

- **Phase 2 USB UAC2 gadget** : alimenter les 8 inputs USB de la matrix (placeholders en V5.1)
- **Phase 3 FX send LV2 sur A53** : 4 bus aux send/return via JACK + plugins LV2
- **Phase 4 NPU mastering closed-loop** : modèle TFLite + service Python + write controls

## 8. Validation par investigation 7 workers

À soumettre via `critic_submit job_type=investigation priority=critical` avec questions Q1-Q12 :

Q1. Layout 0xA0000000+128 MB optimal ? Cacheattr digit 5 → 1 OK ?
Q2. Comp `mixer16` design (struct, mix loop, ALSA controls) sain ?
Q3. `PLATFORM_HEAP_BUFFER 3→4` impact sur `struct mm` (vérifier `mm_heap.h` dimension) ?
Q4. DT `memory-region` du dsp node : ajout `&sdram2_reserved` correct ?
Q5. Linker `imx8m.x.in` : ajout `sof_sdram2 MEMORY{}` + cacheattr `0x22112222` validé ?
Q6. Pan via 2 volume L/R (option a) — vraiment canonique SOF ? Pas de mauvaise surprise ?
Q7. NPU tap V3.2.2 invariant si master chain produit toujours 8ch S32_LE — confirmation ?
Q8. MIPS budget V5.1 (165 MIPS estimé après cascade et mixer16) — réaliste ?
Q9. Plan E0.5 → E7 sain ?
Q10. Cacheattr modif region 5 — risques effets de bord (pas d'autre chose à 0xA0000000+) ?
Q11. mixer16 IPC3 compatibility : CONFIG_COMP_MIXER16 + IPC3 model fonctionnel ?
Q12. Risques OUBLIÉS V5.1 ?

## 9. Référence

- Spec V5 obsolète : `PHASE_1A_3_DSP_V5.md` (commit 19101cd3)
- Investigation V5 RÉVISER : job `ef3c1ede-4bba-45f1-8b70-d8938907e693`
- Topology V4.2 production : `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212-drc.m4`
- NPU tap V3.2.2 : `PHASE_1A_2_NPU_TAP_V3.2.2.md`
- État global : `PROJECT_STATE.md`
- Pattern multi-pipeline : `sof/tools/topology/topology1/sof-smart-amplifier.m4`
- Composants SOF référence : `sof/src/audio/mixer/`, `sof/src/audio/eq_iir/`, `sof/src/audio/drc/`, `sof/src/audio/volume/`
