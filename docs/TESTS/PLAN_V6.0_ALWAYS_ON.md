# V6.0 — Architecture always-on + ASIO IN/OUT piggyback

**Branche** : `feature/v6-always-on-async` (à créer dans le fork SOF Mjxkill)
**Statut** : plan en cours (date 2026-05-06)
**Origine** : abandon de E6.b-pivot (cross-pipeline B5 + matrix_2x8) après 10 jours de debug. La cause structurelle est le couplage PCM lifecycle ↔ DMA hardware en SOF IPC3.

---

## Architecture cible V6.0

```
PIPE 1 (always-on, démarre au boot DSP, ne s'arrête JAMAIS) :

  SAI RX 8ch
       ↓
  ┌─── Strips IN 8 voies indépendantes ──┐
  │  multiband_drc × 8 (1 par voie)       │
  │  drc × 8 (1 par voie)                 │
  │  pga × 8 (1 par voie, volume)         │
  └────────────────────────────────────────┘
       ↓
  matrix_2x8 (16×8, src0 = ASIO play injection, src1 = mics post-strips-IN)
       ↓
  ┌─── Strips OUT 8 voies indépendantes ─┐
  │  multiband_drc × 8 (1 par voie)       │
  │  pga × 8 (1 par voie, volume)         │
  │  drc × 8 (1 par voie)                 │
  └────────────────────────────────────────┘
       ↓
  NPU_TAP (hook dai_dma_cb préservé)
       ↓
  SAI TX 8ch

PIPE 2 (PCM cap = ASIO IN, on-demand, piggyback sur PIPE 1) :
  tap intra-PIPE 1 entre Effets RX et matrix_2x8 → host_buffer ALSA → /dev/snd/...

PIPE 3 (PCM play = ASIO OUT, on-demand, piggyback sur PIPE 1) :
  /dev/snd/... → host_buffer ALSA → matrix_2x8 src0 (PIPE 1)
```

**Garanties non-négociables** :
- DMA hardware (SAI RX chan + SAI TX chan) tournent **always-on** dès le boot DSP, jamais d'arrêt
- Période 2 ms (DMA scheduling)
- BCLK/FSYNC continus pour TAC5212 PLL (TX maître ASYNC)
- ASIO IN et ASIO OUT 8 ch s32le 48 kHz exposés en PCMs ALSA distincts
- Ouvrir/fermer un PCM ASIO ne touche **pas** au DMA hardware
- NPU tap V3.2.2 préservé sur le flux post-effets-OUT pré-SAI TX

---

## Patches à réaliser

### A. Patches firmware DSP (SOF, branche `feature/v6-always-on-async` dans Mjxkill/sof)

| ID | Fichier | Description |
|---|---|---|
| F1 | `src/include/sof/audio/pipeline.h` | Flags `PIPELINE_ATTR_ALWAYS_ON`, `PIPELINE_ATTR_IGNORE_STOP`, `PIPELINE_ATTR_NO_HOST` |
| F2 | `src/ipc/ipc3/handler.c` (`ipc_glb_tplg_pipe_complete`) | Auto-trigger START si flag ALWAYS_ON activé après tplg load |
| F3 | `src/audio/pipeline/pipeline-stream.c` (`pipeline_trigger_run`) | Return PATH_STOP si flag IGNORE_STOP et cmd=STOP/PAUSE |
| F4 | `src/include/ipc/header.h` + `src/ipc/ipc3/handler.c` | Nouveau IPC `SOF_IPC_PIPE_TRIGGER` qui prend pipeline_id (pas comp_id) → permet trigger sans pcm_dev |
| F5 | `src/audio/module_adapter/module_adapter_ipc3.c` | Tolérer `num_of_sources > 1` quand toutes sources intra-pipeline (pas de PATH_STOP injuste) |
| F6 | `src/audio/matrix_2x8/matrix_2x8.c` | Garantie silence-on-empty : si toutes sources vides ET nb_frames>0, écrire silence dans sink → DMA TX always-on |

### B. Patches kernel ASoC (Linux, fichiers dans sources/meta-imx/.../linux-imx ou directement dans le BSP)

| ID | Fichier | Description |
|---|---|---|
| K1 | `sound/soc/sof/imx/imx8m.c` (`imx8m_post_fw_run`) | Appeler `sof_trigger_always_on_pipelines()` après baseline post_fw_run |
| K2 | `sound/soc/sof/ipc3.c` (nouveau helper `sof_trigger_always_on_pipelines`) | Itérer sur les pipelines flagged always-on en topology, envoyer SOF_IPC_PIPE_TRIGGER START |
| K3 | `sound/soc/sof/topology.c` | Parser nouveau token `SOF_TKN_PIPE_ALWAYS_ON` |
| K4 | `sound/soc/sof/pcm.c` (`sof_pcm_trigger`) | Si pipeline associée est always-on, ne pas propager STOP IPC au DAI |
| K5 | `include/sound/sof.h` ou similaire | Ajouter `bool always_on` dans `struct snd_sof_pipeline` |

### C. Patches topology m4 (dans Mjxkill/sof tools/topology/)

| ID | Fichier | Description |
|---|---|---|
| T1 | `tools/topology/m4/sof/tokens.m4` | Nouveau token `SOF_TKN_PIPE_ALWAYS_ON` |
| T2 | `tools/topology/m4/pipeline.m4` | Macro `PIPELINE_ALWAYS_ON_ADD` (dérivée de `PIPELINE_PCM_ADD` mais sans HOST + flag always-on) |
| T3 | `tools/topology/topology1/sof/pipe-dai-to-dai-loopback.m4` (nouveau) | Pipeline DAI-to-DAI : SAI RX source, SAI TX sink, sans HOST ; avec chaîne `multiband_drc×8 + drc×8 + pga×8 + matrix_2x8 + multiband_drc×8 + pga×8 + drc×8`. **Pas d'EQ côté DSP — les EQ paramétriques sont sur les codecs TAC5212.** |
| T4 | `tools/topology/topology1/sof/pipe-host-only-capture.m4` (nouveau) | Pipeline ASIO IN : juste host buffer + HOST capture, source = buffer cross depuis tap PIPE 1 |
| T5 | `tools/topology/topology1/sof/pipe-host-only-playback.m4` (nouveau) | Pipeline ASIO OUT : juste host buffer + HOST playback, sink = matrix_2x8 src0 dans PIPE 1 |
| T6 | `tools/topology/topology1/sof-imx8mp-tac5212-V6.0.m4` (nouveau) | Topologie complète assemblant T3 + T4 + T5 + SectionGraph cross-pipeline + DAI_CONFIG SAI7 |

---

## Ordre d'application + tests par étape

### Étape 1 — Préparation des patches "isolés" (testables sans changer la topologie)

| Sous-étape | Patches | Test | Critère de succès |
|---|---|---|---|
| 1.1 | F6 seul | Cold-boot E6b-pivot 1er aplay | Plus d'I/O error (la chaîne tient car silence-on-empty alimente DMA TX) |
| 1.2 | F5 + F6 | Cold-boot E6b-pivot 1er aplay + son audible | TAC5212 reçoit du son |

Si étape 1.1 échoue → abandonner V6.0, passer en plan B (E6.a+).

### Étape 2 — Always-on flag firmware seul (test sur E6a comme baseline qui marche déjà)

| Sous-étape | Patches | Test | Critère |
|---|---|---|---|
| 2.1 | F1 + F3 | Charger E6a tplg avec attribut always-on hardcodé en m4 | DSP boot, pipeline marquée mais pas auto-démarrée car F2 absent |
| 2.2 | F1 + F2 + F3 | Charger E6a tplg modifiée avec flag always-on | Pipeline démarre seule au boot DSP, son passe sans aplay |

Si étape 2.2 échoue → tenter Plan A IPC variant (F4 au lieu de F2).

### Étape 3 — IPC alternative + kernel (si Plan A IPC choisie)

| Sous-étape | Patches | Test | Critère |
|---|---|---|---|
| 3.1 | F4 + K1 + K2 + K3 + K5 | Topologie E6a + token always-on, kernel envoie IPC PIPE_TRIGGER au probe | Pipeline démarre depuis le kernel post-fw-ready |

### Étape 4 — Bascule topology V6.0

| Sous-étape | Patches | Test | Critère |
|---|---|---|---|
| 4.1 | T1 + T2 + T3 + T6 (pipeline 1 seule, sans T4/T5) | Loopback hardware mics → SAI TX sans aucun PCM ouvert | Mics audibles dans speakers, BCLK continu |
| 4.2 | + T4 | Ajout PCM cap ASIO IN (piggyback) | arecord capture le flux mics post-effets-RX, ouverture/fermeture PCM cap n'arrête pas le loopback |
| 4.3 | + T5 + K4 | Ajout PCM play ASIO OUT (piggyback) | aplay injecte dans matrix src0, ouverture/fermeture PCM play n'arrête pas le loopback |

### Étape 5 — Régression + integration finale

| Sous-étape | Test | Critère |
|---|---|---|
| 5.1 | NPU tap fonctionnel sur le flux post-effets-OUT | dai_dma_cb hook capture les samples avant SAI TX |
| 5.2 | amixer cset modifie matrix gains et effets RX/OUT en temps réel | Pas de glitch audio, paramètres appliqués sans STOP |
| 5.3 | aplay+arecord simultanés (full duplex via les 2 PCMs séparés) | Loopback continue, ASIO IN et ASIO OUT fonctionnent indépendamment |

---

## Plans de repli (à activer si V6.0 échoue à une étape critique)

### Plan B — V6.0a (E6.a+) : effets en sortie, pas de always-on, pas de cross-pipeline

Architecture :
```
PIPE 1 cap : SAI RX → eq×8 + drc×8 + pga×8 → host PCM 0 (ASIO IN, 8ch)
PIPE 2 play : host PCM 1 (ASIO OUT) → deinterleave_8 → mixer16 → interleave_8
                                     → eq×8 + drc×8 + pga×8 + tone_control → SAI TX
```

Pas de tap mics dans la matrix (différence avec V6.0). Pas d'always-on. Pas de cross-pipeline. Architecture E6.a validée + chaîne d'effets en sortie.

**Limitation** : pas de loopback hardware mics → SAI TX. Pour avoir les mics dans le mix, il faudra un mécanisme userspace ou un patch matrix_2x8 sans cross-pipeline (à étudier séparément).

### Plan C — Firmware DSP custom sans SOF

Si V6.0 et V6.0a échouent toutes les deux, abandonner SOF complètement :
- Reprendre uniquement les modules d'effets (eq_iir.c, drc.c, pga.c) comme bibliothèques C portables
- Écrire un firmware Zephyr-only minimal sur le M7
- Implémenter directement :
  - Drivers SAI7 RX/TX
  - Drivers SDMA chan
  - Loopback simple avec chaîne d'effets
  - Interface kernel via mailbox ou IPC custom
  - Exposition de 2 PCMs ALSA via driver custom Linux
- Risques : non-compatibilité upstream, maintenance 100% interne, debug ASoC complexifié
- Avantages : contrôle total du lifecycle, pas de patches massifs SOF, simplification de la chaîne

**Décision** : Plan C est validé comme option viable par l'utilisateur (produit commercial, pas Arduino, refonte framework acceptable si nécessaire).

---

## Suivi de progression

| Étape | Statut | Date | Notes |
|---|---|---|---|
| Plan validé | ✓ | 2026-05-06 | Utilisateur valide V6.0, Plans B et C en backup |
| Branche créée | ✓ | 2026-05-06 | `feature/v6-always-on-async` à partir de `feature/audio-platform-v2` (commit c87b07465) |
| Investigation workers | ✓ | 2026-05-06 | Job a9764aa6 + 69bcdeed validés, 6 workers chacun |
| Décisions tranchées | ✓ | 2026-05-06 | F1=token+IPC SET, F3=1 endroit puis 2 si besoin, multiband_drc=patch per-channel |
| Étape 0 multiband_drc per-channel | | | Pattern DRC D3 portage (~1-2 jours) |
| F1-F6 firmware corrigés | | | Selon retours workers |
| K1-K5 kernel | | | |
| T1-T6 topology | | | |
| Étapes 1-8 testées | | | |

## Décisions architecturales (2026-05-06)

1. **F1 ABI** : pas de break — utiliser **token topology + IPC SET séparé** pour passer les attributs pipeline. Plus complexe à implémenter mais préserve la compat ABI host/firmware.
2. **F3 portée** : démarrer par **1 endroit** (`pipeline_trigger_run`). Si le STOP propage encore via `pipeline_comp_trigger` cross-pipeline, étendre à 2 endroits dans une itération suivante.
3. **multiband_drc patch** : **étape 0** — porter le pattern DRC D3 (`ea984a266`) à `multiband_drc_generic.c` pour avoir 8 configs indépendantes per-channel. Pas de break ABI (détection auto multi-config par taille blob).

---

## Note sur la rétention d'information

Cette planification a été déclenchée après que l'utilisateur a constaté que je filtrais les propositions des workers critic (job a9764aa6, 5 workers). J'ai retenu les patches concrets et poussé une alternative qui ne demandait pas de patches, à l'opposé de la demande utilisateur ("comment faire", pas "si possible").

Une règle a été ajoutée à la mémoire : `feedback_no_filter_workers.md`. Présenter intégralement les propositions workers, ne jamais filtrer pour pousser ma préférence, l'utilisateur tranche.

Si je récidive : signal-mot "tu filtres encore" → correction sur-le-champ. Si malgré tout je continue, abandon de Claude légitime.
