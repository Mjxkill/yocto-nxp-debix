# Test Fiche : V6.0 — E1.c (Fix #3 claude-code intrusif, combo Fix #2 + Fix #3)

**Date** : 2026-05-10
**Statut** : TERMINÉ — Fix #3 (enroll-all-active-channels) ÉCHEC, pipeline_task body toujours non exécuté.

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.4 (V6.0 always-on DAI-to-DAI) |
| Étape | E1.c — test Fix #3 (claude-code, intrusif) en combo avec Fix #2 |
| Branche SOF | `feature/v6-always-on-async` |
| Commit SOF baseline (avant fix) | `3c14c8185` (per-channel SDMA mapping diag) |
| Working tree | dirty (3 fichiers modifiés non commités) |
| Topologie | `sof-imx8mp-tac5212-V6.0.m4` (inchangée) |
| Topology .tplg md5 board | inchangé vs E0/E1/E1.b |

## Contexte et motivation

À l'issue de E1.b, le Fix #2 seul a structurellement réussi (chan 3 RX registered, chan 4 TX déregistré) mais introduit une **régression bloquante** : `pipe_task body` jamais exécuté (0x570 = 0xFFFFFFFF).

Hypothèses formulées :
- **Hyp A** (re-entry guard / `task_is_active`) : Fix #2 lance un schedule_task au PRE_START qui ré-entre dans `pipeline_schedule_copy`, le guard `task_is_active` bloque les wakes ultérieurs
- **Hyp B** (IRQ storm chan 4 TX) : avec Fix #2, chan 4 TX est déregistré côté SOF mais SDMA continue à émettre des IRQ qui ne sont jamais clear → IRQ storm starvent le DT thread
- **Hyp C** (timing handler dual-anchor) : `pipeline_schedule_triggered` est appelé prématurément avant que les deux DAI channels soient ACTIVE

Ordre de test décidé : **A puis B puis C** (du moins intrusif au plus structurel). L'utilisateur a choisi **A** (Fix #3 claude-code intrusif).

## Modifications appliquées (combo Fix #2 + Fix #3)

| Fichier | Changement |
|---|---|
| `src/ipc/ipc-helper.c:234-260` | Fix #2 : fallback `sched_comp = ipc_ppl_source` si DAI-to-DAI structurellement détecté |
| `src/schedule/zephyr_dma_domain.c` register_dma_irq (~l.355) | Fix #3-1 : drop le filtre `is_scheduling_source`, enroll TOUS les channels actifs sur le core |
| `src/schedule/zephyr_dma_domain.c` register_dma_irq (~l.420) | Fix #3-2 : `chan_data->pipe_task = is_scheduling_source ? pipe_task : NULL` (co-tenants pipe_task=NULL) |
| `src/schedule/zephyr_dma_domain.c` register_dma_irq | Fix #3-3 : ne PAS `return 0` après le 1er enroll, continuer pour enroller tous |
| `src/schedule/zephyr_dma_domain.c` dma_irq_handler (~l.215) | Fix #3-4 : `if (chan_data->pipe_task)` avant `pending=true` / `any_fired=true` (co-tenants ne wake pas le DT thread) |
| `src/schedule/zephyr_dma_domain.c` is_pending (~l.776) | Fix #3-5 : `if (!chan_data->pipe_task) continue` skip co-tenants côté is_pending |
| `src/audio/pipeline/pipeline-schedule.c:470-500` | Diag inchangé (slots 0x180-0x190) |

**Justification critic_analyze** : approuvé (artefacts post-compaction = blocages mineurs, pas réels).

## Build et déploiement

| Champ | Valeur |
|---|---|
| Méthode | `west build -d build-sof` puis `west sign -d build-sof --tool rimage --tool-path build-rimage/rimage` |
| Firmware md5 local | `ee031bf52bd9edde945a5f5cd5ebf18e` |
| xxd offset 0x2e0 | `Reef` ✓ |
| Board md5 cohérent | ✓ |
| dmesg version | `2:10:0-3c14c` (commit + dirty) |
| dmesg V6.0 | `pipeline 1 (comp_id 3) started (always-on)` ✓ |

## Résultats E1.c (mailbox `/sys/kernel/debug/sof/debug`)

| Adresse | Compteur | Valeur E1.c | Valeur E1.b Test 2 (Fix #2 seul) | Valeur baseline E1 | Verdict |
|---|---|---|---|---|---|
| 0x180 | psc entry | **1** | n/a (pas de diag) | n/a | inchangé |
| 0x188 | task->state à entrée | **0 (INIT)** | n/a | n/a | task pas active |
| 0x18C | early-return guard | **0xFFFFFFFF** | n/a | n/a | **jamais pris (Hyp A REJETÉE)** |
| 0x190 | schedule_task done | **1** | n/a | n/a | schedule_task OK |
| 0x250 | entry_raw IRQ handler | **~107M (0x066AA6C6)** | ~165M | ~144M | **diminution mais énorme** |
| 0x254 | status_true | 2 | 2 | 3 | identique Fix #2 seul |
| 0x258 | PIPE 1 raw fires | 2 | 2 | 3 | identique Fix #2 seul |
| 0x26C | chan 3 RX fires | **2** ✓ | 2 ✓ | 0xFFFFFFFF | RX registered (Fix #2 OK) |
| 0x270 | chan 4 TX fires | **0xFFFFFFFF** ❌ | 0xFFFFFFFF ❌ | 3 | **TX jamais d'IRQ matched** |
| 0x500 | SDMA mapping marker | 0xCAFE0420 | 0xCAFE0420 | 0xCAFE0420 | OK |
| 0x504 | sdma_set_config calls | 3 | 3 | 3 | OK |
| 0x514 | chan 1 type | 0 (AP2AP) | 0 | 0 | OK |
| 0x51C | chan 3 type | 3 (SHP2MCU) | 3 | 3 | OK (RX) |
| 0x520 | chan 4 type | 4 (MCU2SHP) | 4 | 4 | OK (TX) |
| 0x53C | chan 3 hw_event | 12 | 12 | 12 | OK (SAI7_RX) |
| 0x540 | chan 4 hw_event | 13 | 13 | 13 | OK (SAI7_TX) |
| **0x570** | **pipe_task body** | **0xFFFFFFFF** ❌ | 0xFFFFFFFF ❌ | 1 | **JAMAIS exécuté — régression non corrigée** |

## Verdict E1.c

- **Hypothèse A (re-entry guard) REJETÉE empiriquement** : 0x18C = 0xFFFFFFFF, l'early-return n'est jamais pris.
- **Fix #3 (claude-code, enroll-all + co-tenants) ÉCHEC** : 0x570 reste 0xFFFFFFFF, pipeline_task body jamais exécuté.
- **Effet partiel mesuré** : entry_raw 165M → 107M (~35% de baisse). Donc Fix #3 a effectivement réduit l'IRQ storm, mais pas suffisamment.
- **chan 4 TX `fires=0xFFFFFFFF`** : le compteur dans `dma_irq_handler` n'est jamais incrémenté pour chan 4 → soit chan 4 n'a pas vraiment été enrollé par Fix #3 (bug d'application), soit chan 4 ne fire pas d'IRQ matched par `dma_interrupt_legacy(...DMA_IRQ_STATUS)`, soit le polling ne le voit pas via la SDMA_INTR. Ce résultat invalide partiellement le mécanisme d'enroll co-tenant.
- **Conclusion** : le DT thread reçoit bien 2 IRQ chan 3 (k_sem_give×2 implicite) mais le pipeline_task body n'est jamais exécuté — donc le filter `is_pending` ne matche pas, OU le task n'est pas dans `sch->tasks`, OU il y a un autre filtre en aval.

## Décisions

1. **Hyp A confirmée FAUSSE** (early-return jamais pris).
2. **Fix #3 ne suffit pas** isolément ni en combo avec Fix #2.
3. **Action** : revert tout (Fix #2 + Fix #3 + diag pipeline-schedule.c), retour propre à `3c14c8185`, lancer une nouvelle investigation avec ces résultats empiriques pour cibler la cause racine du `pipe_task body` jamais exécuté.

Pistes à investiguer (à proposer aux workers) :
- Pourquoi chan 4 TX `fires=0xFFFFFFFF` malgré l'enroll Fix #3 (le code Fix #3 enroll-il vraiment chan 4 ? Audit du flow `register_dma_irq` post-Fix #2 où sched_comp=DAI capture)
- Que fait le DT thread entre `k_sem_give(chan 3)` et le retour à `is_pending` ? `entry_raw=107M` suggère que la boucle handler tourne sans cesse
- Le pipe_task est-il dans `sch->tasks` au moment où `is_pending` itère ? Diag à ajouter : compteur `is_pending` total + match success + match fail
- Existe-t-il un filtre en aval de `is_pending` qui empêcherait le run du `pipe_task->ops.run` ?

## Test utilisateur

| Critère | OUI / NON |
|---|---|
| Audio loopback audible | NON (aucun copy SDMA) |
| Validation E1.c GO | **NON** — revert + nouvelle investigation |
