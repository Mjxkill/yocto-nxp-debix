# Test Fiche : V6.0 — E1 (always-on DAI-to-DAI, post fix offset SAI_OFS + per-channel diag)

**Date** : 2026-05-10
**Statut** : EN COURS — fix direction `e51782a7a` validé, fix offset SAI_OFS=8 `a7d4eff36` validé, mais **pipeline_task ne tourne qu'1 cycle après PRE_START** alors que 3 IRQ chan TX firent → audio inaudible. Investigation multi-workers en cours (job critic `55f3643b`).

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.4 (V6.0 always-on DAI-to-DAI loopback) |
| Étape | E1 — Loopback minimal (DAI cap → B0 → DAI play, NO_HOST + ALWAYS_ON) avec **offsets diag corrigés** + **mapping per-channel SDMA** |
| Branche SOF | `feature/v6-always-on-async` |
| Commit SOF HEAD | `3c14c8185` (push github confirmé) |
| Commit yocto-nxp-debix | branche `feature/audio-platform-v2` HEAD `935e4c89` (clean) |
| Topologie | `tools/topology/topology1/sof-imx8mp-tac5212-V6.0.m4` (inchangée vs E0) |
| Firmware sof-imx8m.ri md5 build | `bfbe5316eb4b96a068324144496f0d64` |
| Firmware sof-imx8m.ri md5 board | `bfbe5316eb4b96a068324144496f0d64` (cohérent) ✓ |
| Topology .tplg md5 board | inchangé vs E0 |
| Kernel Image / DTB md5 board | inchangé |

## Patches V6.0 actifs (en plus de E0)

### Commits ajoutés depuis E0 (`0b28f8074`) jusqu'à HEAD (`3c14c8185`)

| Hash | Description | Catégorie |
|---|---|---|
| `99a52d7fc` | F4 : `pipeline_params()` avant `pipeline_prepare()` sur PRE_START | Acquis E0 |
| `0b28f8074` | F4 : dual-anchor RX-then-TX pour V6.0 NO_HOST DAI-to-DAI | Acquis E0 |
| `314411eb6` | Fix base SAI dans diag (SAI5 → SAI7) | Acquis E0 |
| `2e8a662f9` | Fix A (save/restore direction) + Fix B (skip schedule_triggered NO_HOST) | Acquis E0 |
| **`e51782a7a`** | **CRITIQUE** : `dev->direction = dai->direction` dans `dai_common_new()` | **E1 acquis** |
| `c7d428b18` | Diag granulaire `pipeline_schedule_triggered` + Fix B **temporairement désactivé** pour observer | E1 acquis |
| **`a7d4eff36`** | **Fix SAI_OFS=8** dans MMIO direct reads (handler.c + pipeline-stream.c) | **E1 acquis** |
| **`3c14c8185`** | **Diag per-channel SDMA mapping** (chan_type / hw_event / direction par chan) | **E1 acquis** |

## Acquis empiriques E1

### Étapes IPC validées (mailbox SRAM_DEBUG)

| Adresse | Valeur observée | Décodage | Verdict |
|---|---|---|---|
| 0x488 | 0x00000002 | src anchor comp_id = DAI capture | ✓ |
| 0x48C | 0x00000004 | snk anchor comp_id = DAI playback | ✓ |
| 0x4B0 | 0x00000001 | `src->direction` = CAPTURE (post fix `e51782a7a`) | ✓ |
| 0x4B4 | 0x00000000 | `snk->direction` = PLAYBACK | ✓ |
| 0x498 / 0x49C | 0xA1 / 0xA2 | markers F4 handler après pipeline_params src/snk | ✓ |
| 0x4A4 / 0x4A8 | 0xB1 / 0xB2 | markers F4 handler avant/après pipeline_trigger_run src | ✓ |
| 0x370-0x3AC | 4 entries ring buffer | PRE_START src → PRE_START snk → START src → START snk | ✓ dual-anchor |

### États SAI hardware (snapshots DSP-side, post fix offset)

| Adresse | Valeur | Décodage | Verdict |
|---|---|---|---|
| 0x700 / 0x704 | 0xCAFE5A17 / 1 | `sai_set_config` a tourné UNE fois | ✓ |
| 0x708 | 0x90010000 | TCSR après cfg : TERE=1, BCE=1, FRDE=0 | ✓ TX clock armé |
| 0x710 | 0xC0000000 | MCTL après cfg : MCLK_EN=1 | ✓ |
| 0x728 | 0x90030001 | TCSR après `sai_start TX` : TERE=1, BCE=1, FRDE=1 | ✓ TX armé |
| 0x734 | 0x90000001 | RCSR après `sai_start RX` : RERE=1, BCE=1, FRDE=1 | ✓ RX armé |
| 0x7B0 | **0x90100001** | TCSR dans handler (post fix offset SAI_OFS=8) : TERE=1, BCE=1, FRDE=1 | ✓ **SAI reste armé après PRE_START** |
| 0x7C8 | **0x90110001** | TCSR pendant pipeline_comp_copy walk : TERE=1, BCE=1, FRF=1 (FIFO Request actif), FRDE=1 | ✓ |

### SDMA per-channel mapping (commit `3c14c8185`, preuve directe)

| chan | sdma_chan_type | hw_event | direction | rôle |
|------|----------------|----------|-----------|------|
| 1 | AP2AP (0) | -1 (sw-trig) | MEM_TO_MEM | `memcpy_dma_init` (jamais appelée en V6.0 NO_HOST minimal) |
| 3 | SHP2MCU (3) | **12** | DEV_TO_MEM | **SAI7 RX (capture)** |
| 4 | MCU2SHP (4) | **13** | MEM_TO_DEV | **SAI7 TX (playback)** |

### Compteurs runtime (mailbox)

| Adresse | Valeur | Sens | Verdict |
|---|---|---|---|
| 0x250 | ~165M raw | zephyr_dma_domain entry tick (scheduler vivant) | ✓ |
| 0x254 | 3 | status_get true count = **3 IRQ DMA vraies** | ⚠️ peu |
| 0x258 | 3 | PIPE 1 raw fires (= 3) | ⚠️ |
| 0x270 | **3** | chan 4 (TX) IRQ fires | ⚠️ |
| 0x26C | 0xFFFFFFFF | chan 3 (RX) IRQ fires (NON registered, voir analyse) | ❓ aveugle |
| 0x6A4 | **1** | PLAY dma_copy fired (non-throttled) | ❌ devrait être 3 |
| 0x6A8 | 1 | CAP dma_copy fired (throttled 1/16) | ⚠️ |
| 0x610 | **1** | dai_dma_cb PLAY count (non-throttled) | ❌ |
| 0x614 | 1 | dai_dma_cb CAP count (throttled 1/16) | ⚠️ |
| 0x68C / 0x690 | 1 / 1 | sdma_copy chan 3 / chan 4 | ❌ |
| 0x280 | **2** | pipeline_comp_copy total entries (= 1 walk × 2 visites) | ❌ **1 seul walk** |
| 0x340 | 0xFFFFFFFF | sai_stop jamais appelé | ✓ pas de stop volontaire |

### Mailbox propre

Scan complet des 512 slots SRAM_DEBUG (0x000-0x7FC) : **177 adresses connues + ring `pipeline_trigger_run` (0x370-0x3AC) + 2 mineurs** (0x7AC, 0x7FC). **Aucune écriture orpheline**, aucune corruption détectée.

## Diagnostic V6.0 E1

### Ce qui marche
- ✅ Direction propagée (post fix `e51782a7a`)
- ✅ Snapshots TCSR/RCSR cohérents (post fix `a7d4eff36` SAI_OFS=8)
- ✅ Hardware SAI reste armé (TERE=1, BCE=1, FRDE=1 dans tous les snapshots)
- ✅ FCONT=1 maintient BCLK 24/7 (pas d'arrêt clock observé)
- ✅ Dual-anchor F4 dépose les 4 triggers attendus dans l'ordre RX-then-TX

### Ce qui ne marche pas
- ❌ `pipeline_task` ne tourne qu'**1 fois** (1 walk de pipeline_copy)
- ❌ `dai_dma_cb PLAY` non-throttled = 1 (devrait incrémenter à chaque IRQ TX)
- ❌ Pas de son physique en sortie (TX FRF=1 = SAI demande data, mais buffer B0 plus rempli)
- ❓ chan 3 (RX) IRQ fires : `0xFFFFFFFF` — peut signifier "pas d'IRQ" OU "non registered dans zephyr_dma_domain" (instrumentation aveugle)

### Cause racine identifiée (à confirmer par investigation)

**Fallback `sched_comp` mal orienté en V6.0 NO_HOST :**
- topology V6.0 passe `sched_comp = 0` au `PIPELINE_ALWAYS_ON_ADD`
- comp_id 0 n'existe pas → `ipc-helper.c:239` fallback sur `ipc_ppl_sink` = **DAI playback (id 4)**
- `comp_is_scheduling_source(DAI_play) = true` → chan 4 TX registered dans zephyr_dma_domain
- `comp_is_scheduling_source(DAI_cap) = false` → **chan 3 RX NON registered** → instrumentation aveugle, IRQ peut fire mais notre chaîne wake ne la voit pas

**Symptôme additionnel :** 3 IRQ chan 4 → 1 seul pipeline_copy. Hypothèses :
- Coalescing du `k_sem_give` Zephyr (binary sem ?)
- Early return dans pipeline_task body après le 1er cycle
- Le scheduler ne re-pingue plus pipeline_task

## Décision : investigation multi-workers

**Job critic** : `55f3643b-c910-4164-bc0a-e7f758345eac`
- 6 workers actifs (opencode×4 + gemini-3-pro + claude-code)
- Branche/HEAD investigué : `feature/v6-always-on-async @ 3c14c8185`
- Questions posées :
  1. Sémantique `sched_comp` pour V6.0 NO_HOST DAI-to-DAI
  2. Cause "3 IRQ → 1 walk" (coalescing sem ? early return task ?)
  3. Best fix parmi 4 options :
     - **A (firmware)** : `ipc-helper.c:239` fallback sur `ppl_source` si `PIPELINE_ATTR_NO_HOST`
     - **B (topology)** : modifier `pipe-dai-to-dai-loopback.m4` pour `W_PIPELINE(SCHED_COMP, ...)`
     - **C (multi-channel)** : enregistrer 2 channels dans zephyr_dma_domain
     - **D (timer-driven)** : `SCHEDULE_TIME_DOMAIN_TIMER` au lieu de `_DMA`
  4. Anticipation des régressions sur PCM standard

## Tests utilisateur

- [ ] Audio loopback audible (signal injecté RX → entendu TX) : **NON** (pas de son en E1)
- [ ] BCLK 12.288 MHz continu sur scope : à confirmer
- [ ] Aucun glitch / interruption clock : à confirmer

## Action items pour passage E1 → GO

1. Attendre résultats critic `55f3643b` (typique ~10-20 min)
2. Choisir l'option de fix retenue par les workers (analyse intégrale, sans filtrage)
3. `critic_analyze` Phase 1 sur la solution choisie
4. Implémenter, build, sign, deploy
5. Re-tester sur board (per-channel diag + audio loopback)
6. Si audio loopback OK → fiche E1 GO et passage E2 (ajout PCMs ASIO IN/OUT piggyback)

## Mémoires actualisées dans cette session

- `sof_sai_ofs_imx8mp.md` (créée) — décalage +8 systématique sur i.MX8MP
- `sof_firmware_build.md` (mise à jour) — méthode west build directe (PAS bitbake)
- `feedback_tx_master_drives_all.md` (créée) — TX pilote tout, ne jamais soupçonner les TAC

## Test utilisateur (à compléter au passage E1 GO)

| Critère | OUI / NON | Note |
|---|---|---|
| Audio loopback audible | (attente fix) | — |
| BCLK continu sur scope | (à mesurer) | — |
| pipeline_task tourne en continu | (attente fix) | — |
| Validation E1 GO | **EN ATTENTE** | dépend du résultat investigation `55f3643b` |
