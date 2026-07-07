# Phase 1a.3 — DSP V5.3 (8 strips IN + fan-out + matrix 16×8 + 8 strips OUT + ASIO 8 in/out)

**Status** : V5.3 = V5.2 + corrections C20-C23 issues de l'investigation `6b1216e2-b636-4f44-9f9a-6920d4d125fd` (6 workers convergents). Architecture inchangée. À valider par investigation V5.3.
**Date** : 2026-04-28
**Branche** : `feature/audio-platform-v2`
**Précédent** : V5.2 (`PHASE_1A_3_DSP_V5.2.md`, commit 2543c087) — GO conditionnel avec 4 corrections.

---

## 1. Synthèse corrections V5.2 → V5.3

| # | Correction | Origine investigation V5.2 |
|---|---|---|
| **C20** | **Fan-out post-strip-IN explicite** : la dernière strip IN écrit dans 2 `comp_buffer` parallèles via `bsink_list` (un vers mixer16, un vers capture host). Pattern existant SOF (cf. `crossover.c:551`, `selector.c:672`). Aucun comp custom requis. | claude-code, glm-5.1, minimax, deepseek (4/6) |
| **C21** | **`PLATFORM_MAX_STREAMS 8→16`** dans `sof/src/platform/imx8m/include/platform/platform.h:44`. Sinon le 9ème PCM (1 cap + 8 play = 9) crashe. Impact : +64 octets table streams. | deepseek (CRITICAL B1), kimi (R10.1) |
| **C22** | **Charge MIPS ré-estimée 230-280 MIPS** (32-36% du budget 800), pas 198. Composant dominant : 8× multiband_drc mono = 120-140 MIPS. Marge confortable mais réduite vs spec V5.2. Bypass dynamique mb_drc ALSA pour voies non-utilisées = économie ~17 MIPS/voie. | qwen 230, kimi 168, glm-5.1 248, minimax 204, deepseek 306, claude-code 260 |
| **C23** | **Risque cross-pipeline TIMER scheduling explicité** : V4.2 = 2 pipelines DMA-driven, V5.3 = 27 TIMER. Risque répétition timeout V2.7 historique si `PIPELINE_SCHED_COMP` mal chaîné. Validation rigoureuse à E2/E3 + mesure latence E7. | claude-code (Q5 watchpoint, Q10.2), deepseek (R3) |

**Pré-requis E0.5 V5.1 confirmés non-appliqués** (glm-5.1 lecture fichiers) :
- `sof/src/platform/imx8m/imx8m.x.in:166` : cacheattr toujours `0x22212222`
- `sof/src/platform/imx8m/imx8m.x.in` : `sof_sdram2` MEMORY{} non déclaré
- `sof/src/platform/imx8m/lib/memory.c` : `buffer[3]` absent
- `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` : `CONFIG_COMP_IIR=y` absent

→ Ces 4 fixes restent prérequis E0.5 (V5.1 inchangé).

---

## 2. Architecture V5.3 figée (avec fan-out explicite)

```
                                          ┌──► buf_post_in_cap[n]
SAI7 RX 8ch → ×8 strips IN ──[fan-out]────┤    → interleave_8_cap → PCM capture 8ch
(post TAC ADC) bsink_list 2-way            │    ("ASIO IN" — Linux)
                                           ▼
                                       buf_post_in_mix[n]
                                           │
                                           ▼
                              ┌────────────────────────────┐
                              │ mixer16 (matrix 16×8)      │ ◄── 8 PCM playback hosts
                              │ PRE effets de sortie       │     ("ASIO OUT" mono)
                              │ gains Q1.31 (128 cellules) │
                              └────────────┬───────────────┘
                                           ▼ 8 sinks mono homogènes
                              ×8 strips OUT (multiband_drc + pga + drc per channel)
                                           ▼
                              interleave_8 (8 mono → 8ch S32_LE interleaved)
                                           ▼
                              SAI7 TX 8ch → NPU tap V3.2.2 → 4× TAC5212
```

### 2.1 Fan-out post-strip-IN (C20)

Pattern SOF existant — chaque dernière strip IN (volume L/R cascade ou un wrapping comp) attache 2 `comp_buffer` dans son `bsink_list` :

```
strip_IN_n (eq_iir + drc + vol L/R)
    │
    ├── comp_buffer = buf_post_in_mix[n]   → mixer16.in[n]   (PIPE-18)
    │
    └── comp_buffer = buf_post_in_cap[n]   → interleave_8_cap (PIPE-9, capture)
```

Le pipeline scheduler appelle `comp_update_buffer_produce()` sur chaque buffer du `bsink_list` après que le composant a produit ses données. Référence : `module_adapter.c:872-881` itère `num_output_buffers`.

**Coût** : +8 `comp_buffer` (1 par voie) ≈ 8 × 1.5 KB = 12 KB SDRAM. Négligeable.

**Aucun comp custom requis** : le pattern est natif SOF (utilisé dans crossover, selector). Si tests ultérieurs montrent un blocage, fallback comp `tee_1to2` (~30 LOC).

### 2.2 Périmètre DSP (table)

| Bloc | Quantité | Composant SOF | Rôle |
|---|---|---|---|
| strips IN | 8 mono parallèles | eq_iir + drc + 2× volume L/R | EQ + compresseur + fader+pan ALSA-indépendant |
| **fan-out IN** | 8 instances | `bsink_list` 2-way (pattern existant) | duplique buf_post_in[n] vers mixer16 + capture host |
| capture host | 1 × 8ch | host PCM | "ASIO IN" 8ch côté Linux |
| matrix | 1 | **mixer16 NEW** | 16×8 matrix, gains Q1.31, ALSA bytes blob 512B |
| playback hosts | 8 mono | host PCM | "ASIO OUT" 8ch côté Linux, alimentent mixer16 in[8..15] |
| strips OUT | 8 mono parallèles | multiband_drc + pga + drc | effets de sortie ALSA-indépendants |
| interleave | 1 | **interleave_8 NEW** | 8 buffers mono → 1 buffer 8ch S32_LE |
| dai out | 1 | SAI7 TX 8ch | hook NPU tap V3.2.2 invariant |

### 2.3 Périmètre Linux (hors DSP)

- USB UAC2 : 100% Linux
- FX send/return : 100% Linux (JACK + LV2 ou équivalent)
- Lecture fichiers, multi-source mixing : 100% Linux
- Linux écrit le résultat sur les 8 PCM playback "ASIO OUT" → mixer16 in[8..15]

---

## 3. Composants NEW

### 3.1 mixer16 (inchangé V5.2)

Voir `PHASE_1A_3_DSP_V5.2.md §2.1`. Constantes, mix loop, ALSA bytes blob 512B, MIPS ~12.

### 3.2 interleave_8 (inchangé V5.2)

Voir `PHASE_1A_3_DSP_V5.2.md §2.2`. Memcpy avec stride, ~50 LOC, MIPS ~2.

---

## 4. Pré-requis E0.5 (V5.1 inchangé) — récap fichiers à patcher

| Fichier | Modif | C# |
|---|---|---|
| `sof/src/platform/imx8m/imx8m.x.in:166` | `0x22212222 → 0x22112222` (cacheattr region 5 = WT) | C2 |
| `sof/src/platform/imx8m/imx8m.x.in` MEMORY{} | `+sof_sdram2 (rw) : org = SDRAM2_BASE, len = SDRAM2_SIZE` | C7 |
| `sof/src/platform/imx8m/include/platform/lib/memory.h` | `+SDRAM2_BASE 0xA0000000`, `+SDRAM2_SIZE 0x8000000`, `PLATFORM_HEAP_BUFFER 3→4` | C5 |
| `sof/src/platform/imx8m/lib/memory.c` | `+buffer[3] = SDRAM2 caps RAM\|DMA\|CACHE` + heap_map associé | — |
| `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` | `+CONFIG_COMP_IIR=y` | C4 |
| `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` | `+CONFIG_COMP_MIXER16=y`, `+CONFIG_COMP_INTERLEAVE_8=y` (E1) | E1 |
| **`sof/src/platform/imx8m/include/platform/platform.h:44`** | **`PLATFORM_MAX_STREAMS 8→16`** | **C21 NEW** |
| `sof/src/include/sof/audio/module_adapter/module/generic.h:33` | `MODULE_MAX_SOURCES 8→16` (E0.7) | C14 |
| DT (apply-sdram2-dt.py NEW) | reserved-memory `sdram2_reserved@a0000000 + 128 MB no-map` + ajout au memory-region dsp@3b6e8000 | C6 |

---

## 5. Plan d'implémentation V5.3

| Étape | Action | Test gate |
|---|---|---|
| **E0.0** | Confirmer baseline board V4.2 + V3.2.2 NPU tap fonctionnels | test-sof-dsp.sh PASS, NPU tap PASS |
| **E0.5** | SDRAM2 (DT + cacheattr + memory.h + linker + heap[3] + COMP_IIR) | alloc test PASS dans buffer[3] |
| **E0.6** | **Investigation code SOF (avant E1)** : valider que `bsink_list` 2-way fonctionne sur `volume` ou wrapping comp ; mesurer cycle count `multiband_drc` mono ; vérifier `PIPELINE_SCHED_COMP` chaînage | POC fan-out OK + numbers MIPS confirmés |
| **E0.7** | **Patches isolés C14 + C21 + V4.2 régression** : <br>- `MODULE_MAX_SOURCES 8→16`<br>- `PLATFORM_MAX_STREAMS 8→16`<br>- Test bit-perfect V4.2 8ch passthrough avant/après | V4.2 régression PASS, sha256sum capture identique |
| **E1** | Skeleton mixer16 + interleave_8 (proc_type=SOURCE_SINK, process_audio stub) + Kconfig + CMakeLists | Build OK CONFIG_COMP_MIXER16/INTERLEAVE_8=y |
| **E2** | Mix loop mixer16 16×8 gains identité + bytes blob 512B + interleave_8 process. Topology test : 8 mics → fan-out → mixer16(I_8) → 8 strips OUT placeholder → interleave_8 → SAI TX | Audio passthrough OK, NPU tap PASS, V4.2 régression PASS |
| **E3** | Ajout PCM capture "ASIO IN" 8ch (depuis buf_post_in_cap[0..7] via interleave_8_cap) | arecord 8ch OK, fan-out fonctionnel (bit-perfect entre matrix-input et capture-output) |
| **E4** | Ajout 8 PCM playback "ASIO OUT" mono → mixer16 in[8..15] | aplay 8 mono streams OK, mix audible côté SAI TX |
| **E5** | Ajout strips IN complets (eq_iir + drc + 2× volume L/R par voie) | 8 jeux EQ/DRC/Vol fonctionnels en runtime amixer |
| **E6** | Ajout strips OUT complets (multiband_drc + pga + drc par voie). **Mesure cycle count multiband_drc ×8** sur hardware (target ≤ 140 MIPS). | 8 jeux mb_drc/pga/drc fonctionnels, charge DSP < 40% |
| **E7** | NPU tap V3.2.2 régression complète + **mesure latence end-to-end** (SAI RX → SAI TX) | period_bytes=3072 stable, latence < 12 ms |
| **E8** | Stress 10 min (aplay 8ch + arecord 8ch + amixer Matrix + Strips parallèles) | 0 underrun, charge DSP < 60% |

Chaque étape : commit isolé, .tplg version-able, rollback en re-deployant le précédent.

---

## 6. Risques résiduels V5.3

| Risque | Probabilité | Mitigation |
|---|---|---|
| **R-A** Fan-out `bsink_list` 2-way mal supporté par scheduler IPC3 | **Moyen** | E0.6 POC sur 1 voie. Fallback : comp `tee_1to2` custom (~30 LOC) |
| **R-B** `PLATFORM_MAX_STREAMS 8→16` régression V4.2 | **Faible** | E0.7 test isolé bit-perfect avant/après |
| **R-C** Charge DSP > 50% (8× mb_drc lourd, ~140 MIPS) | **Moyen** | E6 mesure réelle ; si > 280 MIPS : (1) bypass mb_drc ALSA pour voies non-actives, (2) réduire à 2 bandes au lieu de 3 |
| **R-D** Cross-pipeline TIMER scheduling : 27 pipelines TIMER vs V4.2 2 pipelines DMA — risque timeout V2.7 historique | **Moyen** | E0.6 valide chaînage `PIPELINE_SCHED_COMP` ; E2 mesure latence ; E7 stress |
| R-E `multiband_drc` ne supporte pas mono (alors que 6/6 workers disent OUI) | **Très faible** | E6 test isolé mono ; fallback 4 instances stereo (perte indép. ALSA paires) |
| R-F Buffer multi-consumer rptr partagé (cas r/w mal synchro entre matrix+capture) | **Faible** | C20 utilise `bsink_list` 2-way (2 buffers indépendants), donc r_ptr séparés |
| R-G 8 SDMA channels playback côté hosts ASIO OUT — saturation si autres usages SAI/BT | **Faible** | Vérifier DT iomux + `cat /sys/class/dma/` au boot |
| R-H 0xA0000000 utilisé déjà par CMA Linux | **Faible** | E0.5.a pré-check + reserved-memory no-map |
| R-I 265 controls ALSA saturent IPC3 control table | **Faible** | Bytes blob 512B fragmenté sur 2 messages (SOF_IPC_MSG_MAX_SIZE=384) ; init time +0.5-1s acceptable |

---

## 7. Hors scope V5.3

- USB UAC2 gadget côté kernel — 100% Linux
- FX send/return — 100% Linux (JACK + LV2)
- Phase 4 NPU mastering closed-loop — séparé
- Optimisation HiFi4 SIMD multiband_drc — différée à Phase 1a.4 si E6 montre dépassement

---

## 8. Investigation critic V5.3 (questions ciblées)

Q1. **Fan-out `bsink_list` 2-way (C20)** : pattern existant (crossover.c:551, selector.c:672) directement applicable à `volume` ou wrapping comp pour produire dans 2 `comp_buffer` indépendants ? Ou nécessite modification minimale du dernier comp de strip IN ?
Q2. **`PLATFORM_MAX_STREAMS 8→16` (C21)** : impact sur autres tables/structures ? `ipc_comp_dev[]`, scheduler tables, free streams bitmap ? Régression V4.2 garantie négative ?
Q3. **Confirmation MIPS multiband_drc mono** sur HiFi4 : 17.5 MIPS/instance estimation deepseek/claude-code vs 10 MIPS (kimi) ou 15 (glm-5.1) — laquelle plus précise pour 3 bandes mono @ 48 kHz ? Conseils HiFi4 SIMD path activé ?
Q4. **`PIPELINE_SCHED_COMP` chaînage TIMER 27 pipelines** : pattern correct pour garantir l'ordre d'exécution strips IN → matrix → strips OUT en single-tick ? Référence canonique SOF ?
Q5. **R-A fallback comp `tee_1to2`** : si `bsink_list` 2-way KO, le comp tee minimal (~30 LOC) doit-il utiliser `proc_type=SOURCE_SINK` (1 source × 2 sinks) ou `STREAM` ? Problème de `max_sources>1 && max_sinks>1` mentionné par glm-5.1 module_adapter.c:258-261 — applicable à 1×2 ?
Q6. **Latence cumulée 27 pipelines TIMER 2 ms** : pire cas 4 hops × 2 ms = 8 ms, est-ce conforme à un usage temps-réel mastering live ? Valeur acceptable ou bloquante pour user perception ?
Q7. **DT carve 128 MB vs 4 MB** (suggestion deepseek) : taille minimale réaliste sachant utilisation réelle ~250-500 KB ? Trade-off Linux RAM perdue vs marge future Phase 2/3/4 ?
Q8. **Bypass dynamique multiband_drc ALSA** (C22 mitigation) : mécanisme SOF existant pour désactiver un comp en runtime sans le retirer du pipeline ? Coût CPU bypass-on (passthrough) vs comp absent ?
Q9. **Risques RÉSIDUELS V5.3** non identifiés ?
Q10. **Verdict GO/REVIEW/NOK** sur V5.3 prête pour E0.5+E0.7 ?

---

## 9. Référence

- Investigation V5.2 : job `6b1216e2-b636-4f44-9f9a-6920d4d125fd` (6 workers, 505s)
- V5.2 : `PHASE_1A_3_DSP_V5.2.md` (commit 2543c087)
- V5.1 : `PHASE_1A_3_DSP_V5.1.md` (commit 8f2765f5)
- Architecture figée : mémoire `project_v5_architecture.md`
- V4.2 production : `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212-drc.m4`
- NPU tap V3.2.2 : `PHASE_1A_2_NPU_TAP_V3.2.2.md`
