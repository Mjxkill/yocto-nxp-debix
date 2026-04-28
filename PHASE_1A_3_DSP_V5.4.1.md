# Phase 1a.3 — DSP V5.4.1 (8 strips IN + tee_1to2 + matrix 16×8 + 8 strips OUT + ASIO 8 in/out)

**Status** : V5.4.1 = V5.4 + 7 ajustements rédactionnels et risques résiduels consolidés issus de l'investigation `65d71d8c-9d36-4efe-a07a-c8fa5c301390` (6 workers, 6/6 GO). Architecture inchangée. **GO E0.5+E0.7+E0.6 paliers.**
**Date** : 2026-04-28
**Branche** : `feature/audio-platform-v2`
**Précédent** : V5.4 (`PHASE_1A_3_DSP_V5.4.md`, commit 1f5dd3af) — GO 6/6, 7 ajustements rédactionnels mineurs requis avant code.

---

## 1. Synthèse ajustements V5.4 → V5.4.1

| # | Ajustement | Origine investigation V5.4 |
|---|---|---|
| **A1** | **R-K (HIGH) audit topology m4 sched_prev/sched_next AVANT E2** : sans chaînage explicite, latence end-to-end = 12 ms (au lieu de 4 ms single-tick). Pour 27 pipelines TIMER, toutes doivent avoir le même `PIPELINE_SCHED_COMP` (DAI SAI7 TX) ET être chaînées via `sched_prev`/`sched_next`. | claude-code (Q7+R-K), 4/6 confirment Q7 |
| **A2** | **Bypass mb_drc précisé prepare-time** (pas runtime). `cd->multiband_drc_func` est figé en prepare. Le toggle ALSA `process_enabled` runtime ne re-sélectionne PAS la fonction. Pour vrai runtime bypass : patch ~5 LOC dans `multiband_drc_ipc3.c:35-37` OU pipeline reset (~100 ms, audible click). | 6/6 (Q5) |
| **A3** | **R-C → HIGH** (au lieu de MEDIUM) : estimation MIPS moyenne réaliste 15-20 MIPS/instance mono × 8 = **120-160 MIPS** (claude-code 12-22, kimi 15, deepseek 8-14, glm-5.1 8-15, minimax 5-15, qwen 5-18). Marge confortable mais réduite vs V5.4. | claude-code (R-M, Q4) |
| **A4** | **SDRAM2_SIZE firmware ≠ DT carve** : déclarer `SDRAM2_SIZE=0x4000000` (64 MB) côté firmware (`memory.h`) mais carver seulement `0x800000` (8 MB) côté DT. Permet ajustement futur DT-only sans rebuild firmware (élargir DT carve sans modifier `memory.h`/`imx8m.x.in`). | claude-code (R-O, Q2), deepseek (Q2) |
| **A5** | **PLATFORM_HEAP_BUFFER 3→4 dans memory.h** explicite E0.5 (en plus de `buffer[3]` dans `memory.c`) | claude-code (R-N) |
| **A6** | **R-Q capture pipeline co-scheduling obligatoire** : `module_single_source_setup` calcule `min_frames` sur tous les sinks. Si pipeline capture pas démarrée (host PCM non ouvert), `num_output_buffers=0` et tee_1to2 ne produit nulle part. Solution : démarrer pipeline capture en même temps que playback OU tee_1to2 vérifie sinks actifs. | glm-5.1 (R-K), deepseek (R-N) |
| **A7** | **R-V tests mono crossover/mb_drc explicites** E5/E6 : aucun test connu de mono `nch=1` sur i.MX8M ; arrays dimensionnés `PLATFORM_MAX_CHANNELS=8` ⇒ tests obligatoires. | deepseek (R-K) |

**Pré-requis E0.5 V5.1 toujours non-appliqués** :
- `sof/src/platform/imx8m/imx8m.x.in:166` : cacheattr `0x22212222`
- `sof/src/platform/imx8m/imx8m.x.in` : `sof_sdram2` MEMORY{} non déclaré
- `sof/src/platform/imx8m/lib/memory.c` : `buffer[3]` absent
- `sof/src/platform/imx8m/include/platform/lib/memory.h` : `PLATFORM_HEAP_BUFFER=3`
- `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` : `CONFIG_COMP_IIR=y` absent

---

## 2. Architecture V5.4.1 figée (inchangée)

```
                                          ┌──► buf_post_in_cap[n]
SAI7 RX 8ch → ×8 strips IN → tee_1to2[n] ─┤    → interleave_8_cap → PCM capture 8ch
(post TAC ADC)               (NEW comp,    │    ("ASIO IN" — Linux)
                             1 src × 2 sinks)
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

Cf. V5.4 §2.1 (composants NEW), §2.2 (tee_1to2 spec), §2.3 (périmètre DSP final).

---

## 3. DT carve + SDRAM2_SIZE firmware (A4 nuance)

**DT côté Linux** : 8 MB
```diff
sdram2_reserved@a0000000 {
    no-map;
-    reg = <0 0xA0000000 0 0x8000000>;  /* 128 MB */
+    reg = <0 0xA0000000 0 0x800000>;   /* 8 MB — V5.4.1 */
};
```

**memory.h côté firmware** : 64 MB déclarés (mais 8 MB seulement carvés DT)
```diff
+#define SDRAM2_BASE 0xA0000000
+#define SDRAM2_SIZE 0x4000000  /* 64 MB déclaré firmware ; 8 MB carvés DT (extensible DT-only) */
```

**imx8m.x.in MEMORY{}** : utiliser `SDRAM2_SIZE` (firmware = 64 MB)
```
sof_sdram2 :
        org = SDRAM2_BASE,
        len = SDRAM2_SIZE
```

**memory.c buffer[3]** : utiliser `SDRAM2_BASE`/`SDRAM2_SIZE`
```c
.buffer[3] = {
    .heap = SDRAM2_BASE,
    .size = SDRAM2_SIZE,  /* 64 MB déclaré, mais l'allocation runtime ne dépasse pas le DT carve */
    .info = {.free = SDRAM2_SIZE,},
    .caps = SOF_MEM_CAPS_RAM | SOF_MEM_CAPS_DMA | SOF_MEM_CAPS_CACHE,
}
```

**Conséquence** : agrandir DT carve 8 MB → 16 / 32 / 64 MB futur = simple modification DT, pas de rebuild firmware.

Cacheattr `0x22112222` inchangé (region 5 = 0xA0000000-0xBFFFFFFF en WT).

---

## 4. Plan d'implémentation V5.4.1 (réordonné avec audit M4 et tests mono)

| Étape | Action | Test gate |
|---|---|---|
| **E0.0** | Baseline board V4.2 + V3.2.2 NPU tap | test-sof-dsp.sh PASS, NPU tap PASS |
| **E0.5.a** | Pré-check `cat /proc/iomem` sur board, vérifier 0xA0000000-0xA07FFFFF libre | Zone non listée → OK |
| **E0.5.b** | DT `apply-sdram2-dt.py` : `sdram2_reserved@a0000000 + 8 MB no-map` + ajout au memory-region du dsp@3b6e8000 | `cat /proc/iomem` show "a0000000-a07fffff : reserved", MemTotal -8 MB |
| **E0.5.c** | SOF `memory.h` : `+SDRAM2_BASE 0xA0000000`, `+SDRAM2_SIZE 0x4000000` (64 MB firmware), `PLATFORM_HEAP_BUFFER 3→4` | Build OK |
| **E0.5.d** | SOF `imx8m.x.in` : MEMORY `+sof_sdram2`, cacheattr `0x22212222 → 0x22112222` | Build OK, linker OK |
| **E0.5.e** | SOF `memory.c` : `+buffer[3]` SDRAM2 caps RAM\|DMA\|CACHE | Build OK |
| **E0.5.f** | `imx8mp_evk_mimx8ml8_adsp.conf` : `+CONFIG_COMP_IIR=y` | Build OK |
| **E0.5.g** | Sign + deploy + reboot V4.2 baseline avec SDRAM2 dispo | dmesg "Firmware info" propre, V4.2 régression PASS, NPU tap V3.2.2 PASS |
| **E0.5.h** | Test alloc forcée buffer[3] (créer comp_buffer pour saturer SDRAM1 → bascule SDRAM2) | trace adresse dans [0xA0000000, 0xA0800000] |
| **E0.6.a** | **POC tee_1to2** : skeleton + topology test 1 voie : 1 host → tee → 2 hosts capture, sha256 identique | bit-perfect 2 captures |
| **E0.6.b** | **Cycle count multiband_drc mono S32_LE 3-bandes** sur HW (target ≤ 17 MIPS, CRITICAL > 25 MIPS) | MIPS confirmé |
| **E0.6.c** | **5 pipelines TIMER co-scheduled** workload identique, mesurer cycles_max/min/avg | 0 timeout, slope log-log ≈ 1 |
| **E0.6.d** | **10 pipelines TIMER**, idem | 0 timeout |
| **E0.6.e** | **27 pipelines TIMER** (target full V5.4.1) | 0 timeout, cycles_max < 1.12 M (marge 30% sur 2 ms × 800 MHz) |
| **E0.7** | **Patches isolés C14 + C21 + V4.2 régression bit-perfect** : <br>- `MODULE_MAX_SOURCES 8→16` (`generic.h:33`)<br>- `PLATFORM_MAX_STREAMS 8→16` (`platform.h:44`)<br>- Test V4.2 8ch passthrough sha256sum avant/après | V4.2 régression PASS (sha256 identique) |
| **E1** | Skeleton tee_1to2 + mixer16 + interleave_8 + Kconfig + CMakeLists | Build OK 3 NEW comps |
| **E2** | mixer16 mix loop 16×8 gains identité + bytes blob 512B + interleave_8 process. **Audit M4 sched_prev/sched_next OBLIGATOIRE** (A1) | Audio passthrough, NPU tap PASS, V4.2 régression PASS |
| **E3** | PCM capture "ASIO IN" 8ch (depuis buf_post_in_cap[0..7] via interleave_8_cap) avec **co-scheduling capture+playback** (A6) | arecord 8ch OK, fan-out bit-perfect |
| **E4** | 8 PCM playback "ASIO OUT" mono → mixer16 in[8..15] | aplay 8 mono streams OK, mix audible |
| **E5** | Strips IN complets (eq_iir + drc + 2× volume L/R par voie) + **tests mono explicites** (A7) | 8 jeux EQ/DRC/Vol amixer fonctionnels, mono OK |
| **E6** | Strips OUT complets (multiband_drc + pga + drc) + **mesure cycle count mb_drc ×8 réelle + tests mono** (A7) | 8 jeux mb_drc/pga/drc OK, charge DSP < 50% |
| **E7** | NPU tap V3.2.2 régression complète + mesure latence end-to-end (target < 4 ms si single-tick, < 12 ms si cascade) | period_bytes=3072 stable |
| **E8** | Stress 10 min (aplay 8ch + arecord 8ch + amixer Matrix + Strips parallèles) | 0 underrun, charge DSP < 60% |

---

## 5. Risques résiduels V5.4.1 consolidés (R-A à R-V)

| Risque | Source | Sévérité | Mitigation |
|---|---|---|---|
| **R-A** tee_1to2 STREAM mode rejeté par module_adapter | V5.4 | Faible | E0.6.a POC + check `max_sources=1, max_sinks=2` non bloqué (6/6 workers confirment) |
| **R-B** PLATFORM_MAX_STREAMS 8→16 régression V4.2 | V5.4 | Faible | E0.7 test isolé bit-perfect |
| **R-C** multiband_drc MIPS > 150 sur HW | V5.4 + A3 | **HIGH** | Bypass dynamique ALSA (prepare-time, A2) ; réduction à 2 bandes ; bypass mb_drc voies non actives via patch upstream (~5 LOC) |
| **R-D** 27 pipelines TIMER : aucun précédent IPC3 connu | V5.4 | Moyen | E0.6.c-e POC paliers 5/10/27 ; rollback si timeout |
| **R-E** DT carve 8 MB insuffisant si Phase 2-4 | V5.4 | Faible | Élargir DT (rebuild kernel uniquement) — A4 décorrèle SDRAM2_SIZE firmware (64 MB) du DT carve |
| **R-F** Buffer multi-consumer rptr partagé | V5.4 | Très faible | tee_1to2 produit dans 2 `comp_buffer` distincts → r_ptr séparés |
| **R-G** 8 SDMA channels playback côté hosts ASIO OUT | V5.4 | Faible | Vérifier DT iomux + `cat /sys/class/dma/` |
| **R-H** 0xA0000000 utilisé par CMA Linux | V5.4 | Faible | E0.5.a pré-check + reserved-memory no-map |
| **R-I** 265 controls ALSA + bytes blob 512B fragmenté IPC3 | V5.4 | Faible | Bytes blob 512B fragmenté (SOF_IPC_MSG_MAX_SIZE=384) ; init time +0.5-1s acceptable |
| **R-J** Latence end-to-end > 12 ms (6 hops × 2 ms pire cas) | V5.4 | Moyen | E7 mesure ; si > 12 ms, période 1 ms (4 ms latence min) |
| **R-K** **Topology M4 sched_prev/sched_next manquant** (A1) | V5.4.1 NEW | **HIGH** | Audit M4 complet AVANT E2. Toutes les 27 pipelines doivent avoir même `PIPELINE_SCHED_COMP` (DAI SAI7 TX) et chaînage explicite |
| **R-L** **Bypass mb_drc prepare-time** (A2) | V5.4.1 NEW | Moyen | Patch upstream multiband_drc_ipc3.c +5 LOC pour runtime bypass OU pipeline reset accepté avec glitch |
| **R-M** **MIPS estimation 120-160 dépasse budget initial 150** (A3) | V5.4.1 NEW | **HIGH** | Mesure E6 obligatoire. Si > 150 : bypass voies inactives, réduction 3→2 bandes |
| **R-N** **PLATFORM_HEAP_BUFFER 3→4 dans memory.h** explicite (A5) | V5.4.1 NEW | Faible | Inclus E0.5.c |
| **R-O** **SDRAM2_SIZE firmware ≠ DT carve** (A4) | V5.4.1 NEW | Faible | Décorrélé : 64 MB firmware déclaré, 8 MB DT carvé |
| **R-P** **min_frames cross-pipeline data loss** | V5.4.1 NEW (glm-5.1) | Moyen | Co-scheduling capture+playback (A6) ; ou tee_1to2 vérifie sinks actifs |
| **R-Q** **Capture pipeline désynchronisée** (A6) | V5.4.1 NEW (deepseek) | Moyen | Démarrer pipeline capture en même temps que playback |
| **R-R** **Stack SOF avec PLATFORM_MAX_STREAMS=16** | V5.4.1 NEW (glm-5.1) | Faible | Vérifier stack 3072 B suffit avec 27 pipelines, +128 octets stack frames module_adapter |
| **R-S** **265 controls + blob 512B fragmentation IPC3** | V5.4.1 NEW (glm-5.1, deepseek) | Faible | Test E2 init time |
| **R-T** **tee position tracking module_update_buffer_position** | V5.4.1 NEW (deepseek) | Faible | Cosmétique (stats), framework gère le reste via comp_update_buffer_produce |
| **R-U** **IPC4 process_enabled=true workaround kernel ≤ 6.6** | V5.4.1 NEW (deepseek) | Non applicable | V5.4.1 = IPC3 |
| **R-V** **Mono non testé crossover/mb_drc** (A7) | V5.4.1 NEW (deepseek) | Moyen | Tests E5/E6 obligatoires avec `nch=1` |

---

## 6. Hors scope V5.4.1

- USB UAC2 gadget côté kernel — 100% Linux
- FX send/return — 100% Linux (JACK + LV2)
- Phase 4 NPU mastering closed-loop — séparé
- Optimisation HiFi4 SIMD multiband_drc — différée à Phase 1a.4 si E6 montre dépassement
- Patch upstream `multiband_drc_ipc3.c` runtime bypass — différé à Phase 1a.4 si R-C confirmé

---

## 7. Référence

- Investigation V5.4 : job `65d71d8c-9d36-4efe-a07a-c8fa5c301390` (6 workers, 462s)
- V5.4 : `PHASE_1A_3_DSP_V5.4.md` (commit 1f5dd3af)
- V5.3 : `PHASE_1A_3_DSP_V5.3.md` (commit 329fe804)
- V5.2 : `PHASE_1A_3_DSP_V5.2.md` (commit 2543c087)
- V5.1 : `PHASE_1A_3_DSP_V5.1.md` (commit 8f2765f5)
- Architecture figée : mémoire `project_v5_architecture.md`
- V4.2 production : `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212-drc.m4`
- NPU tap V3.2.2 : `PHASE_1A_2_NPU_TAP_V3.2.2.md`
