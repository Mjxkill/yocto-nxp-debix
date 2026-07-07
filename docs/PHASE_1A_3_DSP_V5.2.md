# Phase 1a.3 — DSP V5.2 (8 strips IN + matrix 16×8 + 8 strips OUT + ASIO 8 in/out + SDRAM2 128 MB)

**Status** : V5.2 = architecture figée par l'utilisateur (cf. mémoire `project_v5_architecture.md`).
**Date** : 2026-04-28
**Branche** : `feature/audio-platform-v2`
**Précédent** : V4.2 production + V3.2.2 NPU tap + V5.1 (REJETÉE — asymétrie 2/6 + simplification 16×1 non conformes au cahier des charges utilisateur).

---

## 1. Architecture figée

```
                                   ┌──► PCM capture 8ch ("ASIO IN" — post strips IN)
SAI7 RX 8ch → ×8 strips IN ────────┤
(post TAC ADC)                      │
                                    ▼
                       ┌────────────────────────────┐
                       │ mixer16 (matrix 16×8)      │  ◄── 8 PCM playback hosts mono
                       │ PRE effets de sortie       │      ("ASIO OUT" — Linux mixe USB IN
                       │ gains Q1.31 (128 cellules) │       + lecture fichiers + FX returns
                       └────────────┬───────────────┘       en amont sur ces 8 voies)
                                    ▼ 8 sinks mono homogènes
                       ×8 strips OUT (multiband_drc + pga + drc per channel)
                                    ▼
                       interleave_8 (8 mono → 8ch S32_LE interleaved)
                                    ▼
                       SAI7 TX 8ch → NPU tap V3.2.2 → 4× TAC5212
```

### 1.1 Périmètre DSP

| Bloc | Quantité | Composant SOF | Rôle |
|---|---|---|---|
| strips IN | 8 mono parallèles | eq_iir + drc + 2× volume L/R | EQ + compresseur + fader+pan ALSA-indépendant par voie |
| capture host | 1 × 8ch | host PCM | expose "ASIO IN" 8ch côté Linux (lit le buffer post strips IN) |
| matrix | 1 | **mixer16 NEW** | 16×8 matrix, gains Q1.31, ALSA bytes blob 512B (128 cellules) |
| playback hosts | 8 mono | host PCM | exposent "ASIO OUT" 8ch côté Linux, alimentent mixer16 in[8..15] |
| strips OUT | 8 mono parallèles | multiband_drc + pga + drc | effets de sortie ALSA-indépendants par voie |
| interleave | 1 | **interleave_8 NEW** | 8 buffers mono → 1 buffer 8ch S32_LE |
| dai out | 1 | SAI7 TX 8ch | hook NPU tap V3.2.2 invariant |

### 1.2 Périmètre Linux (hors DSP)

- USB UAC2 : 100% Linux (sox/PA/JACK/native ALSA)
- FX send/return : 100% Linux (JACK + LV2)
- Lecture fichiers, multi-source mixing : 100% Linux
- Linux écrit le résultat sur les 8 PCM playback "ASIO OUT" → entrent dans mixer16 in[8..15]

---

## 2. Composants NEW à créer

### 2.1 mixer16 (matrix 16 sources mono × 8 sinks mono, IPC3)

**Fichiers** :
- `sof/src/audio/mixer16/mixer16.c`
- `sof/src/audio/mixer16/mixer16_generic.c`
- `sof/src/audio/mixer16/mixer16.h`
- `sof/src/audio/mixer16/Kconfig`
- `sof/src/audio/mixer16/CMakeLists.txt`
- patch `sof/src/audio/CMakeLists.txt`
- patch `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` (`+CONFIG_COMP_MIXER16=y`)

**Constantes** :
```c
#define MIXER16_MAX_SOURCES 16
#define MIXER16_MAX_SINKS    8
#define MIXER16_GAIN_BITS   31  /* Q1.31 */
```

**Mix loop** :
```c
for (frame = 0; frame < n_frames; frame++)
    for (j = 0; j < n_sinks; j++) {
        int64_t acc = 0;
        for (i = 0; i < n_sources; i++)
            acc += ((int64_t)src[i][frame] * gain[i][j]) >> 31;
        dst[j][frame] = sat_int32(acc);
    }
```

**proc_type / API** : `MODULE_PROCESS_TYPE_SOURCE_SINK` + `.process_audio` (sink_src API). Manipule `sources[]`/`sinks[]` directement (pas d'audio_stream_type_copy).

**ALSA** : 1 control bytes blob 512B (128 cellules × 4 octets Q1.31). Layout `int32_t gain[16][8]`.

**Estimation MIPS @ 48 kHz, 96 frames/period, 500 Hz period rate** : ≈ 12 MIPS HiFi4 SIMD.

### 2.2 interleave_8 (8 mono → 1 buffer 8ch interleaved S32_LE)

**Fichiers** :
- `sof/src/audio/interleave_8/interleave_8.c` (~50 LOC)
- `sof/src/audio/interleave_8/Kconfig`
- `sof/src/audio/interleave_8/CMakeLists.txt`
- patch `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` (`+CONFIG_COMP_INTERLEAVE_8=y`)

**Process** : memcpy avec stride :
```c
for (frame = 0; frame < n_frames; frame++)
    for (ch = 0; ch < 8; ch++)
        dst[frame*8 + ch] = src[ch][frame];
```

**proc_type / API** : `MODULE_PROCESS_TYPE_SOURCE_SINK` + `.process_audio`. 8 sources mono → 1 sink 8ch.

**Estimation MIPS** : ≈ 2 MIPS.

---

## 3. Pré-requis SDRAM2 (V5.1 inchangé)

Voir `PHASE_1A_3_DSP_V5.1.md` §3 :
- DT carve `sdram2_reserved@a0000000 + 128 MB no-map` ajouté au memory-region du dsp@3b6e8000
- `imx8m.x.in` : MEMORY{ sof_sdram2 (rw) } + cacheattr `0x22112222` (region 5 = WT)
- `memory.h` : `SDRAM2_BASE/SIZE` + `PLATFORM_HEAP_BUFFER 4`
- `memory.c` : `buffer[3] = SDRAM2 caps RAM|DMA|CACHE`
- `imx8mp_evk_mimx8ml8_adsp.conf` : `+CONFIG_COMP_IIR=y`

---

## 4. Topology m4 V5.2 (squelette)

### 4.1 Pipelines indépendantes co-scheduled

```
PIPE-1..8 (in)        : SAI capture → strip_IN_n (eq_iir + drc + vol L/R) → buf_post_in[n]
PIPE-9    (cap)       : buf_post_in[0..7] → interleave_8 → host PCM "ASIO IN"
PIPE-10..17 (play)    : host PCM "ASIO OUT" n → mixer16.in[8 + n]
PIPE-18   (matrix)    : buf_post_in[0..7] + host[0..7] → mixer16 → buf_post_mix[0..7]
PIPE-19..26 (out)     : buf_post_mix[n] → strip_OUT_n (mb_drc + pga + drc) → buf_post_out[n]
PIPE-27   (interleave): buf_post_out[0..7] → interleave_8 → DAI SAI7 TX 8ch
```

Tous co-scheduled via `PIPELINE_SCHED_COMP_N` sur le scheduler timer-based.

### 4.2 ALSA controls exposés (résumé)

| Strip | Controls par voie | Total |
|---|---|---|
| Strip IN | eq_iir 4 bandes (12 controls) + drc (4) + 2× volume (2) | 18 × 8 = 144 |
| Matrix | bytes blob 512B "Matrix Gains" | 1 |
| Strip OUT | mb_drc (≈10) + pga (1) + drc (4) | 15 × 8 = 120 |
| **Total** | | **≈ 265 controls** |

Tous indépendants par voie (aucune mutualisation). À vérifier IPC3 limit table size.

---

## 5. Plan d'implémentation V5.2

| Étape | Action | Test gate |
|---|---|---|
| **E0.0** | Confirmer baseline board : V4.2 + V3.2.2 NPU tap fonctionnels | test-sof-dsp.sh PASS, NPU tap PASS |
| **E0.5** | SDRAM2 (DT + cacheattr + memory.h + linker + heap[3]) — V5.1 inchangé | alloc test PASS dans buffer[3] |
| **E0.6** | **Investigation code SOF (avant E1)** : confirmer `sources[]`/`sinks[]` API ; charge multiband_drc en mono ; pattern multi-sink sur un buffer (post-strips-IN consommé par matrix + capture host) | Q1..Q5 documentés |
| **E0.7** | Patch C14 (`MODULE_MAX_SOURCES 8→16`) **isolé** + test V4.2 régression | V4.2 régression PASS |
| **E1** | Skeleton mixer16 + interleave_8 (proc_type=SOURCE_SINK, process_audio stub) + Kconfig + CMakeLists | Build OK CONFIG_COMP_MIXER16/INTERLEAVE_8=y |
| **E2** | Mix loop mixer16 16×8 gains identité + bytes blob 512B + interleave_8 process | Topology test 8 mics → mixer16 (gains identité I_8 sur in[0..7]) → 8 strips OUT placeholder (passthrough) → interleave_8 → SAI TX. Audio passthrough OK |
| **E3** | Ajout PCM capture "ASIO IN" 8ch (1 host depuis buf_post_in[0..7] via interleave_8 dédié ou buffer share) | arecord 8ch OK, samples non nuls |
| **E4** | Ajout 8 PCM playback "ASIO OUT" mono → mixer16 in[8..15] | aplay 8 mono streams OK, mix audible côté SAI TX |
| **E5** | Ajout strips IN complets (eq_iir + drc + 2× volume L/R par voie) | 8 jeux EQ/DRC/Vol fonctionnels en runtime amixer |
| **E6** | Ajout strips OUT complets (multiband_drc + pga + drc par voie) | 8 jeux mb_drc/pga/drc fonctionnels en runtime amixer |
| **E7** | NPU tap V3.2.2 régression complète | period_bytes=3072 stable, magic NPAT, write_idx flow |
| **E8** | Stress 10 min (aplay 8ch + arecord 8ch + amixer Matrix + Strips parallèles) | 0 underrun, charge DSP < 60% |

Chaque étape : commit isolé, .tplg version-able, rollback en re-deployant le précédent.

---

## 6. Risques résiduels V5.2

| Risque | Probabilité | Mitigation |
|---|---|---|
| `multiband_drc` ne supporte pas mono natif | **Moyen** | E0.6 vérifie. Fallback : 4 instances stereo (paires 0-1, 2-3, 4-5, 6-7) — perte d'indépendance par voie, mais moins lourd |
| Buffer post-strips-IN partagé entre matrix + capture host (multi-sink) non supporté IPC3 | **Faible** | Pattern SOF standard (un comp_buffer peut avoir plusieurs sinks dans `comp_buffer.sink_list`) ; à confirmer E0.6 |
| Charge DSP > 60% (8× mb_drc lourd) | **Moyen** | Estimation 198/800 = 25% ; mb_drc FFT 3 bandes = 10 MIPS/instance ; mesurer E6 |
| 265 controls ALSA saturent IPC3 control table | **Faible** | À mesurer E5/E6 ; bytes blobs réduisent le compte si nécessaire |
| C14 (MODULE_MAX_SOURCES 8→16) régression V4.2 | **Faible** | E0.7 test isolé |
| 0xA0000000 utilisé déjà par CMA Linux | **Faible** | E0.5.a pré-check + reserved-memory no-map |

---

## 7. Hors scope V5.2

- USB UAC2 gadget côté kernel — 100% Linux (drivers + ALSA plumbing)
- FX send/return (réverbe, delay, etc.) — 100% Linux (JACK + LV2)
- Phase 4 NPU mastering closed-loop — séparé

---

## 8. Investigation critic V5.2 (questions ciblées)

Q1. `multiband_drc` SOF supporte-t-il le mode mono (1 channel input/output) ? Si non, fallback 4 instances stereo viable ?
Q2. Pattern un comp_buffer → 2 sinks (matrix + capture host) supporté en IPC3 SOF (multi-sink consumer) ?
Q3. Charge DSP estimée ~198 MIPS sur 800 budget — réaliste pour 8× eq_iir 4 bandes + 8× drc + 16× volume + mixer16 16×8 + 8× mb_drc + 8× pga + 8× drc OUT + interleave_8 ?
Q4. ALSA controls : ~265 controls IPC3 — saturation possible ? Limites table ?
Q5. Topology m4 IPC3 : pattern 27 pipelines co-scheduled (8 IN + 8 PCM hosts play + 8 OUT + matrix + capture + interleaves) faisable ? Limites SOF sur nombre de pipelines simultanées ?
Q6. `MODULE_MAX_SOURCES 8→16` (C14) impact réel sur `struct processing_module` (sources[]+sinks[] = 16+16 = +128 octets/instance) — tous les comps existants non-régressés ?
Q7. `interleave_8` minimal (8 sources mono → 1 sink 8ch) — pattern SOF natif équivalent existant (mux ?) ou comp custom obligatoire ?
Q8. NPU tap V3.2.2 invariance : SAI7 TX 8ch S32_LE @ 48 kHz period 96 frames préservée par interleave_8 amont ?
Q9. Charge mémoire SDRAM2 128 MB suffisante pour : 27 pipelines comp_buffer + coefficients eq_iir/drc/mb_drc + matrix gains 512 B ?
Q10. Risques OUBLIÉS V5.2 ?

---

## 9. Référence

- Architecture figée : mémoire `project_v5_architecture.md`
- V5.1 obsolète : `PHASE_1A_3_DSP_V5.1.md`
- V4.2 production : `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212-drc.m4`
- NPU tap V3.2.2 : `PHASE_1A_2_NPU_TAP_V3.2.2.md`
- État global : `PROJECT_STATE.md`
