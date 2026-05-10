# Test Fiche : V6.0 — E1.b (test isolé Fix #1 / Fix #2 issus job critic 55f3643b)

**Date** : 2026-05-10
**Statut** : TERMINÉ — les 2 fixes testés isolément, AUCUN ne résout, le Fix #2 introduit une régression. Revert complet et nouvelle investigation lancée.

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.4 (V6.0 always-on DAI-to-DAI) |
| Étape | E1.b — validation isolée des 2 fixes proposés par job critic 55f3643b |
| Branche SOF | `feature/v6-always-on-async` |
| Commit SOF baseline (avant tests) | `3c14c8185` (per-channel SDMA mapping diag) |
| Topologie | `sof-imx8mp-tac5212-V6.0.m4` (inchangée) |
| Topology .tplg md5 board | inchangé vs E0/E1 |

## Contexte

L'investigation `55f3643b` a fait converger 6/6 workers sur 2 bugs :
- **Fix #1 (CRITIQUE)** : `bool pending` dans `zephyr_dma_domain.c:74` coalesce les IRQ multiples → propose `uint32_t pending_count`
- **Fix #2 (HIGH)** : `ipc-helper.c:239` fallback sur `ipc_ppl_sink` (DAI playback) au lieu de `ipc_ppl_source` (DAI capture) pour pipelines NO_HOST DAI-to-DAI structurels → propose détection structurelle

L'utilisateur a demandé de tester chaque fix **isolément** avant de combiner, pour mesurer leur effet réel.

## Test 1 — Pending counter SEUL (Fix #1 seulement, sans Fix #2)

### Modifications
| Fichier | Changement |
|---|---|
| `src/schedule/zephyr_dma_domain.c:74` | `bool pending` → `uint32_t pending_count` |
| `src/schedule/zephyr_dma_domain.c:223` | `pending = true` → `pending_count++` |
| `src/schedule/zephyr_dma_domain.c:740-756` | `pending` check/consume → `pending_count == 0` / `pending_count--` |
| `src/ipc/ipc-helper.c:239` | **inchangé** (fallback ipc_ppl_sink original) |

### Build et déploiement
- Firmware md5 : `dc95b6f4be3b44e2b3c443e3c9d1e90d`
- xxd offset 0x2e0 : `Reef` ✓
- Board md5 cohérent ✓
- dmesg : `pipeline 1 (comp_id 3) started (always-on)` ✓

### Résultats Test 1 (mailbox)

| Adresse | Valeur Test 1 | Valeur baseline E1 | Verdict |
|---------|---------------|--------------------|---------|
| 0x488 src anchor | 0x2 | 0x2 | identique |
| 0x48C snk anchor | 0x4 | 0x4 | identique |
| 0x4B0 src->direction | 1 (CAP) | 1 (CAP) | identique |
| 0x4B4 snk->direction | 0 (PLAY) | 0 (PLAY) | identique |
| 0x254 status_true | **3** | 3 | identique |
| 0x258 PIPE 1 raw fires | **3** | 3 | identique |
| **0x26C chan 3 RX fires** | **0xFFFFFFFF** | 0xFFFFFFFF | inchangé (chan 3 toujours non-registered) |
| **0x270 chan 4 TX fires** | **3** | 3 | inchangé |
| **0x570 pipe_task PIPE 1** | **1** (throttled) | 1 | identique |
| **0x600 sdma_copy total** | **1** | 1 | identique |
| 0x68C chan 3 sdma_copy | 1 | 1 | identique |
| 0x690 chan 4 sdma_copy | 1 | 1 | identique |
| 0x6A4 PLAY dma_copy (non-thr) | 1 | 1 | identique |
| 0x6A8 CAP dma_copy (thr) | 1 | 1 | identique |
| 0x610 dai_dma_cb PLAY (non-thr) | 1 | 1 | identique |
| 0x614 dai_dma_cb CAP (thr) | 1 | 1 | identique |
| 0x280 pipeline_comp_copy | 2 | 2 | identique |

### Verdict Test 1
**Aucun effet visible empiriquement.** Le comportement est strictement identique au baseline E1 :
- 3 IRQ TX fired
- 1 seul cycle complet de pipeline_copy
- Buffer B0 traité 1 fois
- Pas d'audio (TX continue à demander mais buffer plus rempli)

**Hypothèse** : avec `pending_count` au lieu de `bool pending`, on s'attendrait à 3 cycles complets. Mais on observe 1. Soit :
- `task_is_active(p->pipe_task)` empêche les wakes ultérieurs
- Soit le DT thread ne consomme pas les 3 `k_sem_give` (mais 0x250 entry raw montre que le thread tourne ~144M fois)
- Soit les 3 IRQ TX arrivent en burst avant que le 1er wake termine, et le `task_is_active` filtre les suivants
- Soit il y a un autre filtre que je n'ai pas identifié

## Test 2 — sched_comp SEUL (Fix #2 seulement, sans Fix #1)

### Modifications
| Fichier | Changement |
|---|---|
| `src/schedule/zephyr_dma_domain.c` | **reverté** (bool pending original) |
| `src/ipc/ipc-helper.c:234-240` | détection structurelle DAI-to-DAI → fallback `ipc_ppl_source` au lieu de `ipc_ppl_sink` |

### Build et déploiement
- Firmware md5 : `a2eee16a4387fd3b7c4a8a46aff92384`
- xxd offset 0x2e0 : `Reef` ✓
- Board md5 cohérent ✓
- dmesg : `pipeline 1 (comp_id 3) started (always-on)` ✓

### Résultats Test 2 (mailbox)

| Adresse | Valeur Test 2 | Valeur baseline E1 | Verdict |
|---------|---------------|--------------------|---------|
| 0x488 src anchor | 0x2 | 0x2 | identique |
| 0x48C snk anchor | 0x4 | 0x4 | identique |
| 0x4B0 src->direction | 1 (CAP) | 1 (CAP) | identique |
| 0x4B4 snk->direction | 0 (PLAY) | 0 (PLAY) | identique |
| 0x254 status_true | **2** | 3 | différent (RX fires) |
| 0x258 PIPE 1 raw fires | **2** | 3 | différent |
| **0x26C chan 3 RX fires** | **2** ✓ | 0xFFFFFFFF | **Fix #2 fonctionne** : RX registered |
| **0x270 chan 4 TX fires** | **0xFFFFFFFF** ✓ | 3 | **Fix #2 fonctionne** : TX déregistré |
| **0x570 pipe_task PIPE 1** | **0xFFFFFFFF** ❌ | 1 | **RÉGRESSION** : task jamais run |
| **0x600 sdma_copy total** | **0xFFFFFFFF** ❌ | 1 | aucun copy |
| **0x6A4 PLAY dma_copy** | **0xFFFFFFFF** ❌ | 1 | aucun |
| **0x6A8 CAP dma_copy** | **0xFFFFFFFF** ❌ | 1 | aucun |
| **0x610 dai_dma_cb PLAY** | **0xFFFFFFFF** ❌ | 1 | aucun |
| **0x614 dai_dma_cb CAP** | **0xFFFFFFFF** ❌ | 1 | aucun |
| **0x280 pipeline_comp_copy** | **0xFFFFFFFF** ❌ | 2 | aucun walk |
| 0x4C0 pipeline_schedule_triggered entry | 0xC0 | 0xC0 | atteint |
| 0x4E4 pipeline_schedule_triggered exit | 0xE4 | 0xE4 | atteint |
| 0x4F0 Fix B path | 0xCC | 0xCC | atteint |
| 0x520 chan 4 sdma_chan_type | MCU2SHP (4) | MCU2SHP (4) | identique |
| 0x540 chan 4 hw_event | 13 | 13 | identique |
| 0x51C chan 3 sdma_chan_type | SHP2MCU (3) | SHP2MCU (3) | identique |
| 0x53C chan 3 hw_event | 12 | 12 | identique |

### Verdict Test 2
- **Fix #2 atteint son objectif structurel** : sched_comp est bien le DAI capture, chan 3 RX est registered et reçoit ses 2 IRQ.
- **MAIS introduit une régression majeure** : `pipeline_task` body **jamais exécuté** (0x570 = 0xFFFFFFFF), aucun walk pipeline_copy, aucun sdma_copy, aucun dai_dma_cb.
- pipeline_schedule_triggered traversé entièrement (markers 0x4C0-0x4E4), donc le PRE_START est OK.
- Le DT thread est wake (chan 3 fires comptés = 2), mais le filter `is_pending` doit retourner false ou le task n'est pas dans `sch->tasks`.

**Hypothèses pour la régression** :
1. Le pipeline_task n'est plus dans `sch->tasks` quand le DT thread itère (peut-être que `task_is_active(p->pipe_task)` retourne true et empêche `schedule_task` dans `pipeline_schedule_copy`)
2. Avant Fix #2, le PRE_START déclenchait un 1er wake immédiat (probablement via TX IRQ qui arrive dès `sai_start`). Avec Fix #2, TX déregistré → ce wake initial perdu
3. Le `register_dma_irq` filter `crt_chan->status == COMP_STATE_ACTIVE` peut filtrer chan 3 si la registration arrive avant `sdma_start`
4. `chan_data->pipe_task` mal positionné

## Comparaison synthétique

| | Baseline E1 (pre-fix) | Test 1 (Fix #1 seul) | Test 2 (Fix #2 seul) | Fix #1+#2 (précédent) |
|---|---|---|---|---|
| chan 3 RX fires | 0xFFFFFFFF | 0xFFFFFFFF | **2** ✓ | 2 ✓ |
| chan 4 TX fires | 3 | 3 | 0xFFFFFFFF ✓ | 0xFFFFFFFF ✓ |
| pipe_task body | 1 | 1 | 0xFFFFFFFF ❌ | 0xFFFFFFFF ❌ |
| sdma_copy | 1 | 1 | 0 ❌ | 0 ❌ |
| dai_dma_cb | 1/1 | 1/1 | 0/0 ❌ | 0/0 ❌ |
| pipeline_comp_copy | 2 | 2 | 0 ❌ | 0 ❌ |

## Décisions

1. **Fix #1 (pending counter) seul** : aucun effet visible empiriquement. Soit le bug n'est pas là, soit les 3 IRQ ne se traduisent pas en 3 wakes effectifs (autre filtre).
2. **Fix #2 (sched_comp) seul** : atteint l'objectif structurel mais casse le pipeline_task. Inutilisable en l'état.
3. **Fix #1 + #2 combiné** : même régression que Test 2 (pipeline_task perdu).

→ **Aucun des fixes proposés par les workers ne suffit isolément ou combiné.**

→ **Action** : revert complet à HEAD `3c14c8185` (état E1 baseline avec diag), nouvelle investigation avec ces résultats empiriques pour cibler la régression.

## Test utilisateur

| Critère | OUI / NON |
|---|---|
| Audio loopback audible Test 1 | NON (identique baseline) |
| Audio loopback audible Test 2 | NON (encore plus dégradé : aucun copy) |
| Validation E1.b GO | **NON** — relance investigation nécessaire |
