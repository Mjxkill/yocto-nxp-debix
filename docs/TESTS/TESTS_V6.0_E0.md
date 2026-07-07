# Test Fiche : V6.0 — E0 (always-on DAI-to-DAI minimal, baseline pré-board)

**Date** : 2026-05-08
**Statut** : EN ATTENTE — fiche créée AVANT validation board pour rattraper la dette de traçabilité (rétro-cadrage de la session V6.0 démarrée 2026-05-06).

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.4 (V6.0 always-on DAI-to-DAI baseline) |
| Version de référence | V6.0 |
| Étape | E0 — Loopback DAI-to-DAI minimal (SAI7 RX → B0 → SAI7 TX), pas d'effets, pas de matrix, pas de PCM |
| Commit SOF | `0b28f8074` (branche `feature/v6-always-on-async`, push github confirmé) |
| Commit yocto-nxp-debix | `73f2b2bf` (branche `feature/audio-platform-v2`, push origin confirmé) |
| Topologie de référence | `tools/topology/topology1/sof-imx8mp-tac5212-V6.0.m4` |
| Firmware sof-imx8m.ri md5 (build local actuel) | `a3150f44` (ATTENTION : built sur HEAD parent `a1e24059c` snk-only, **PAS encore le dual-anchor `0b28f8074`**) |
| Firmware sof-imx8m.ri md5 board | À confirmer par utilisateur (`md5sum /lib/firmware/imx/sof/sof-imx8m.ri` sur board) |
| Topology .tplg md5 board | À confirmer par utilisateur (`md5sum /lib/firmware/imx/sof-tplg/sof-imx8mp-tac5212.tplg`) |
| Kernel Image md5 board | inchangé ou à reconfirmer si patches K0+K2+K4+K5+K1 redéployés |
| DTB md5 board | inchangé |

⚠️ **Rebuild firmware nécessaire** avant tout nouveau test : le commit `0b28f8074` n'est pas encore intégré au firmware local (build daté 8 mai 11:17, commit poussé 8 mai 16:14).

## Patches V6.0 actifs sur la branche

### Firmware DSP (commits dans `feature/v6-always-on-async`)
| Patch | Fichier | Status | Commit |
|---|---|---|---|
| F1 | `src/include/sof/audio/pipeline.h` | ✅ commité | inclus dans la chaîne F2/F4 |
| F2 | `src/ipc/ipc3/handler.c` (`ipc_glb_tplg_pipe_complete`) | ✅ commité | `18dbde7f4` |
| F3 | `src/audio/pipeline/pipeline-stream.c:632-644` (IGNORE_STOP filter) | ✅ commité | inclus avec F1 |
| F4 | `src/ipc/ipc3/handler.c` (`ipc_glb_tplg_pipe_trigger`) | ✅ commité | `18dbde7f4` puis `99a52d7fc` puis dual-anchor `0b28f8074` |
| F5 | `src/audio/module_adapter/module_adapter_ipc3.c:153-185` (`module_sources_all_intra_pipeline`) | ✅ commité | dans la chaîne |
| F6 | `src/audio/matrix_2x8/matrix_2x8.c` silence-on-empty | ⏸️ pas appliqué (matrix pas dans topology V6.0 E0) |

### Kernel ASoC (patcher additif Yocto)
Fichier : `meta-local/recipes-kernel/linux/files/apply-v6-always-on.py` (commit `73f2b2bf` parent `14f6dcbf`)
| Patch | Fichier kernel | Status |
|---|---|---|
| K0 | `include/uapi/sound/sof/tokens.h` (token 224) + `include/sound/sof/header.h` (cmd 0x014) + `include/sound/sof/topology.h` (struct) | ✅ |
| K2 | `sound/soc/sof/ipc3-topology.c` (parser token) | ✅ |
| K4 | `sound/soc/sof/sof-audio.h` (`bool always_on` dans widget) | ✅ |
| K5 | `sound/soc/sof/ipc3-topology.c` (helper `sof_ipc3_send_pipe_trigger` étendu rate/ch/fmt/dir) | ✅ commit `14f6dcbf` |
| K1 | `sound/soc/sof/ipc3-topology.c` (warn-not-fail sur PIPE_TRIGGER échec) | ✅ commit `ac929d80` |

### Topology m4 (commits dans `feature/v6-always-on-async`)
| Patch | Fichier | Status | Commit |
|---|---|---|---|
| T1 | `tools/topology/topology1/m4/sof/tokens.m4` (token always_on) | ✅ commité | `bbbed9bb2` |
| T2 | `tools/topology/topology1/m4/pipeline.m4` (macro `PIPELINE_ALWAYS_ON_ADD`) | ✅ commité | `bbbed9bb2` |
| T3 | pipeline DAI-to-DAI loopback m4 | ✅ commité | `bbbed9bb2` (intégré directement dans T6) |
| T6 | `tools/topology/topology1/sof-imx8mp-tac5212-V6.0.m4` | ✅ commité | `bbbed9bb2` puis `76d819f83` (anchor PIPELINE_ALWAYS_ON sur DAI capture scheduler) |
| T4 | `pipe-host-only-capture.m4` (PIPE 2 ASIO IN piggyback) | ⏸️ pas créé (V6.1) |
| T5 | `pipe-host-only-playback.m4` (PIPE 3 ASIO OUT piggyback) | ⏸️ pas créé (V6.1) |

## Architecture E0

```
SAI7 RX (capture, slave)  →  B0 (buffer)  →  SAI7 TX (playback, master)

Pipeline 1 attributes : PIPELINE_ATTR_ALWAYS_ON | PIPELINE_ATTR_IGNORE_STOP | PIPELINE_ATTR_NO_HOST
Anchor : p->source_comp = SAI7 RX (capture DAI)
Sched comp : DAI capture scheduler (PIPELINE_ALWAYS_ON ancré ici, commit 76d819f83)
DMA scheduling : period 2 ms (mémoire feedback_dma_2ms_definitive)
```

## Invariant hardware (ne JAMAIS oublier)

- **SAI7 TX = master** : génère BCLK + FSYNC. TCSR.TERE/TRCE armés dès `sai_set_config()` (24/7), patch idempotence verrouille (mémoire `sof_sai_idempotence_patch`).
- **SAI7 RX = slave** : reçoit BCLK/FSYNC du TX via PCB (boucle physique).
- **Ordre de démarrage requis** : TX clock continue (déjà armée) → DMA RX (FRDE_RX) → DMA TX (FRDE_TX). Cohérent avec le dual-anchor `src→snk` du F4 handler.
- **TAC5212 PLL** : reste lockée tant que BCLK ne s'interrompt pas. Aucun STOP propagé au DAI (F3 IGNORE_STOP + K4 prévu en V6.1).

## Tests prévus E0 (à exécuter après rebuild + flash)

| # | Test | Commande board | Attendu |
|---|---|---|---|
| 1 | Boot DSP charge topology V6.0 | dmesg \| grep -i sof | Aucun "tplg load failed", `firmware info: version 2:10:0-<hash>` |
| 2 | K1 envoie PIPE_TRIGGER après fw_run | dmesg \| grep -i "pipe_trigger\|always_on" | "always-on: pipeline N triggered" + ret >= 0 |
| 3 | F4 handler exécute dual-anchor | mailbox 0x7C0 (PRE_START exit count), 0x7C4 (START exit count) | ≥ 1 chacun |
| 4 | sai_start TX appelé | mailbox 0x720 (sai_start TX count) | ≥ 1 |
| 5 | sai_start RX appelé | mailbox 0x730 (sai_start RX count) | ≥ 1 |
| 6 | TCSR final stable | mailbox 0x7B4 (TCSR after START) | TE=1 (bit 31), FRDE=1 (bit 9), FEF=0 (bit 18), SEF=0 (bit 19) |
| 7 | RCSR final stable | mailbox à allouer (voir §plage mailbox) | RE=1, FRDE=1, FEF=0, SEF=0 |
| 8 | BCLK continu sur scope | scope sur SAI7 TX_BCLK | 12.288 MHz stable, pas d'arrêt |
| 9 | Loopback hardware audible | inject signal sur SAI7 RX_DATA0 (TAC5212 line-in), écouter sur TAC5212 OUT | son passe sans glitch |

## Test utilisateur

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | NON (en attente rebuild firmware + flash board) |
| Type prévu | écoute audio + mesure BCLK scope |
| Résultat | — |

## Plage mailbox réservée V6.0

État actuel des écritures mailbox V6.0 (à valider non-collisionnant avec `MAILBOX_DEBUG_MAP.md`) :

| Offset | Sens | Source |
|---|---|---|
| 0x720 | sai_start TX count | sai.c (existant E6.b, conservé V6.0) |
| 0x724 | sai_start TX-cfg path count | sai.c (existant) |
| 0x728 | TCSR snapshot inside sai_start | sai.c (existant) |
| 0x730 | sai_start RX count | sai.c (existant) |
| 0x7B0 | TCSR après PRE_START handler exit | handler.c F4 (V6.0) |
| 0x7B4 | TCSR après START handler exit | handler.c F4 (V6.0) |
| 0x7B8 | TCSR après STOP handler exit | handler.c F4 (V6.0) |
| 0x7BC | TCSR après PAUSE handler exit | handler.c F4 (V6.0) |
| 0x7C0 | PRE_START handler exit count | handler.c F4 (V6.0) |
| 0x7C4 | START handler exit count | handler.c F4 (V6.0) |

**Zone libre confirmée pour nouveau diag E0/E1** : `0x470-0x4FF` (144 B, source `MAILBOX_DEBUG_MAP.md`). À utiliser pour :
- RCSR snapshot (équivalent 0x7B4 pour TX) — proposition `0x470` (TCSR à 0x7B4, RCSR à 0x470 pour différencier)
- src trigger return code, snk trigger return code — proposition `0x474`, `0x478`
- Compteurs séparés src/snk pipeline_trigger_run — proposition `0x47C`, `0x480`

⚠️ Toute nouvelle adresse mailbox doit suivre la mémoire `feedback_unique_mailbox_addresses` : grep préalable, plage documentée, jamais réutiliser une adresse déjà écrite.

## Devices ALSA attendus E0

| Device | Card | Rôle | Format |
|---|---|---|---|
| (aucun) | softac5212tdm card 2 | E0 = pas de PCM exposé (NO_HOST). PCMs viendront en E1 avec T4+T5. | — |

E0 ne crée AUCUN device ALSA — c'est purement un loopback DSP-only sans interaction userspace. C'est intentionnel : valider l'always-on en isolation avant d'ajouter le piggyback ASIO.

## Conclusion E0

| Statut | À déterminer après tests board (rebuild firmware nécessaire au préalable) |
|---|---|
| GO | Tous les tests 1-9 PASS + scope BCLK + audio loopback audible → passer en E1 (ajouter T4 PCM cap ASIO IN) |
| REVIEW | Tests 1-7 PASS mais 8 ou 9 KO → diag instrumentation supplémentaire avant E1 |
| NOK | Tests 1-7 KO → revert dual-anchor, repenser approche, plan B (V6.0a sans always-on) |

## Logs significatifs (à remplir après tests board)

```
=== dmesg sof ===
(à remplir)

=== mailbox V6.0 (od -An -tx4 /sys/kernel/debug/sof/debug | …) ===
(à remplir)

=== TCSR/RCSR snapshot via devmem2 ===
TCSR @ 0x30c50008 = ?
RCSR @ 0x30c50088 = ?
TCR2 @ 0x30c50010 = ?
RCR2 @ 0x30c50090 = ?
```

## Référence (autres docs/spec liées)

- Plan V6.0 complet : `TESTS/PLAN_V6.0_ALWAYS_ON.md`
- Patches détaillés : `TESTS/PATCHES_V6.0_DETAILS.md`
- Carte mailbox exhaustive + collisions : `TESTS/MAILBOX_DEBUG_MAP.md`
- Fiche précédente (E6.b matrix_2x8 — pivot V6.0 décidé après celle-ci) : `TESTS/TESTS_V5.4.1_E6.b.md`
- Investigations critic associées :
  - Walk direction wrong (job `5f4ee23e-9e06-46f5-9924-f23b40171b38`, 5/6 workers convergent)
  - Invariant hardware TX-master/RX-slave (job `f25615e1-0f15-40e7-a704-81d5cde53efd`, 5/6 workers convergent)
- Mémoires applicables :
  - `feedback_dma_2ms_definitive.md` — period 2ms NON-NÉGOCIABLE
  - `sof_sai_idempotence_patch.md` — TX clock continue, ne PAS revert
  - `feedback_unique_mailbox_addresses.md` — adresses uniques par diag
  - `feedback_critic_only_committed.md` — git status propre AVANT critic
  - `feedback_test_fiches.md` — format de cette fiche
