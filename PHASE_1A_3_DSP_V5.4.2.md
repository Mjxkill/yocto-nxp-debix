# Phase 1a.3 — DSP V5.4.2 (DMA-driven scheduling — suppression mode TIMER)

**Status** : V5.4.2 = V5.4.1 + correction A8 (DMA-driven only, suppression mode TIMER pure). Décision utilisateur après E2 mix loop validé.
**Date** : 2026-04-28
**Branche** : `feature/audio-platform-v2`
**Précédent** : V5.4.1 (`PHASE_1A_3_DSP_V5.4.1.md`, commit 6713215d) + E0.5/E0.7/E1/E2 implémentés et validés board.

---

## 1. Correction V5.4.1 → V5.4.2

| # | Correction | Raison |
|---|---|---|
| **A8** | **DMA-driven scheduling pour TOUTES les pipelines V5.4+** (au lieu de TIMER). Toutes les pipelines utilisent `SCHEDULE_TIME_DOMAIN_DMA` avec `sched_comp = N_PCMP(SAI7_TX_PCM)` (DAI playback comme master clock). Le `pipeline_copy()` descend en cascade dans le contexte de l'IRQ DMA TX (single-tick implicite). | Clips audio historiques en mode TIMER (mémoire `sof_async_solution.md`). V4.2 actuelle marche en DMA-driven sans clips → continuer ce pattern. |

## 2. Risques V5.4.1 supprimés / dégradés

| Risque V5.4.1 | Statut V5.4.2 | Note |
|---|---|---|
| **R-K** Topology M4 sched_prev/sched_next (HIGH) | **SUPPRIMÉ** | DMA-driven utilise un seul `sched_comp` partagé (pattern V4.2). Plus besoin de chaînage explicit. |
| **R-D** 27 pipelines TIMER : aucun précédent (Moyen) | **SUPPRIMÉ** | DMA-driven est le pattern V4.2 production, validé en service. |
| **C25** Plan E0.6.c-e paliers TIMER 5/10/27 | **SUPPRIMÉ** | Pas de paliers TIMER nécessaires en DMA-driven. |
| R-J Latence > 12 ms (Moyen) | **DÉGRADÉ → Faible** | DMA-driven single-tick implicite garantit latence ≈ 1 period DAI (2 ms) + processing. |
| R-R Stack SOF avec PLATFORM_MAX_STREAMS=16 (Faible) | Inchangé | E0.7 validé sans régression. |

## 3. Architecture topology m4 V5.4.2 (DMA-driven)

```
sched_comp = SAI7_TX_PCM (master clock)

Pipeline TX (DMA-driven, declenche TOUT le cascade upstream) :
  SAI7 TX 8ch ◄── interleave_8 ◄── 8 strips OUT ◄── mixer16 ◄── ...

  cascade upstream via comp_buffer chain :
    mixer16.in[0..7]   ◄── 8 buf_post_in_mix ◄── 8 strips IN ◄── SAI7 RX 8ch
    mixer16.in[8..15]  ◄── 8 host_buffers ◄── 8 PCM playback hosts (host DMA pulls them)

Pipeline capture host (DMA-driven sur host DMA) :
  host_capture ◄── interleave_8_cap ◄── 8 buf_post_in_cap ◄── 8 strips IN (shared with main TX cascade)
```

**Mécanisme** :
- `sched_comp = N_PCMP(SAI7_TX_PCM)` — la pipeline du DAI playback est le maître
- Toutes les pipelines internes (strips, tee, mixer, interleave) ont leur `pipe_task` schedulé via le DMA TX IRQ
- `pipeline_copy()` walk descend amont dans le contexte de l'IRQ → single-tick
- Les hosts PCM playback sont déclenchés par leur propre DMA host (alimente `mixer16.in[8..15]`)
- Le host capture est déclenché par son DMA host (consomme `buf_post_in_cap[0..7]`)

## 4. Plan d'implémentation V5.4.2 (E3 et suite)

| Étape | Action | Test gate |
|---|---|---|
| **E3** | Topology m4 mini test : 1 voie SAI RX → strip IN minimal → tee_1to2 → mixer16 (16×8 identité) → strip OUT placeholder → interleave_8 → SAI7 TX. **Tout en `SCHEDULE_TIME_DOMAIN_DMA` avec sched_comp = SAI7_TX_PCM**. | Audio passthrough OK, NPU tap PASS, 0 xrun |
| **E3.b** | Ajout PCM capture "ASIO IN" 8ch via second pipeline DMA-driven (host DMA) | arecord 8ch OK, fan-out via tee bit-perfect |
| **E4** | 8 PCM playback "ASIO OUT" mono → mixer16 in[8..15] (host DMA per pipeline) | aplay 8 mono streams OK, mix audible |
| **E5** | Strips IN complets (eq_iir + drc + 2× volume L/R par voie) | 8 jeux EQ/DRC/Vol amixer fonctionnels |
| **E6** | Strips OUT complets (multiband_drc + pga + drc) + mesure cycle count mb_drc ×8 | 8 jeux fonctionnels, charge DSP < 50% |
| **E7** | NPU tap V3.2.2 régression complète + mesure latence end-to-end | period_bytes=3072 stable, latence < 8 ms attendu |
| **E8** | Stress 10 min (aplay 8ch + arecord 8ch + amixer Matrix + Strips parallèles) | 0 underrun, charge DSP < 60% |

## 5. Risques résiduels V5.4.2 mis à jour

| Risque | Probabilité | Mitigation |
|---|---|---|
| R-A tee_1to2 STREAM mode rejet | Faible | E1 validé, build OK. E3 valide le datapath. |
| R-B PLATFORM_MAX_STREAMS 8→16 régression V4.2 | Faible | E0.7 validé bit-perfect. |
| R-C multiband_drc MIPS > 150 | **HIGH** | Bypass dynamique ALSA prepare-time. Mesure E6. |
| ~~R-D 27 pipelines TIMER~~ | SUPPRIMÉ | DMA-driven, plus applicable. |
| R-E DT carve 8 MB insuffisant Phase 2-4 | Faible | Élargir DT (rebuild kernel uniquement). |
| R-F Buffer multi-consumer rptr partagé | Très faible | tee_1to2 produit dans 2 comp_buffer distincts. |
| R-G 8 SDMA channels playback | Faible | Vérifier DT iomux + cat /sys/class/dma/. |
| R-H 0xA0000000 utilisé par CMA Linux | Faible | E0.5 validé : reserved-memory no-map OK. |
| R-I 265 controls ALSA blob fragmenté | Faible | Test E5/E6 init time. |
| ~~R-J Latence > 12 ms (cascade)~~ | DÉGRADÉ → Faible | DMA-driven single-tick : latence ≈ 4 ms (1 period + processing). |
| ~~R-K Topology M4 sched_prev/sched_next~~ | SUPPRIMÉ | Un seul sched_comp partagé. |
| R-L Bypass mb_drc prepare-time | Moyen | Patch upstream 5 LOC OU pipeline reset. |
| R-M MIPS estimation 120-160 dépasse budget | HIGH | Mesure E6 obligatoire. |
| R-N PLATFORM_HEAP_BUFFER 3→4 explicit | Faible | E0.5 validé. |
| R-O SDRAM2_SIZE firmware ≠ DT carve | Faible | Aligné 8 MB en V5.4.1. |
| R-P min_frames cross-pipeline data loss | Moyen | Co-scheduling capture+playback (DMA-driven naturel). |
| R-Q Capture pipeline désynchronisée | Moyen | DMA-driven garantit sync via SAI7 TX master. |
| ~~R-R Stack SOF PLATFORM_MAX_STREAMS=16~~ | Faible | E0.7 validé, +64 B/frame. |
| R-S 265 controls + blob fragmentation IPC3 | Faible | Test E5. |
| R-T tee position tracking | Faible | Cosmétique. |
| ~~R-U IPC4 process_enabled~~ | NA | V5.4.2 = IPC3. |
| R-V Mono non testé crossover/mb_drc | Moyen | Tests E5/E6 obligatoires. |

## 6. Référence

- V5.4.1 : `PHASE_1A_3_DSP_V5.4.1.md` (commit 6713215d)
- V5.4 / V5.3 / V5.2 / V5.1 : commits sur `feature/audio-platform-v2`
- E2 mix loop SOF : commit `04eadda17`
- Architecture figée : mémoire `project_v5_architecture.md`
- DMA-driven obligatoire : mémoire `feedback_dma_driven_only.md`
- V4.2 production (référence pattern DMA-driven) : `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212-drc.m4`
- NPU tap V3.2.2 : `PHASE_1A_2_NPU_TAP_V3.2.2.md`
