# ARCHI V6.0 — Always-on DAI-to-DAI loopback + ASIO IN/OUT piggyback

**Date** : 2026-05-08
**Statut** : design en place, E0 (loopback minimal) en attente de validation board
**Branches** :
- SOF firmware : `feature/v6-always-on-async` (push github confirmé, HEAD `0b28f8074`)
- yocto-nxp-debix (kernel patcher + topology Yocto + docs) : `feature/audio-platform-v2` (push origin confirmé, HEAD `73f2b2bf`)

---

## 1. Objectif

Créer un firmware SOF dont le DSP fait un **loopback hardware permanent** entre SAI7 RX et SAI7 TX (via le buffer B0), géré par un pipeline marqué **always-on** qui démarre au probe DSP et n'est jamais arrêté par le lifecycle ALSA.

But final : exposer 2 PCM ALSA piggyback (ASIO IN 8ch + ASIO OUT 8ch) qui se branchent sur ce pipeline always-on **sans interrompre** le DMA hardware. Permet :
- L'audition continue des micros (TAC5212 line-in) sur les speakers (TAC5212 line-out) sans aucun PCM ouvert.
- L'ouverture/fermeture des PCMs ASIO ne casse pas la boucle hardware → pas de glitch BCLK, TAC PLL toujours lockée.
- Le NPU tap (mémoire `npu_tap_v1_proposal`) hooke `dai_dma_cb` côté SAI TX → samples post-effets exportés vers DDR/NPU.

Pourquoi pas le design SOF standard ? Le couplage PCM-lifecycle ↔ DMA-hardware en SOF IPC3 force un STOP/PAUSE du SAI DMA à chaque close PCM, ce qui interrompt BCLK, fait perdre la lock TAC PLL, et est incompatible avec le besoin produit (audition continue + ASIO temps réel).

---

## 2. Topology cible

```
┌────────────────────────────────────────────────────────────────────────────┐
│ PIPE 1 (always-on, démarre au boot DSP, ne s'arrête JAMAIS)                │
│                                                                            │
│   SAI7 RX 8ch ──→ B0 ──→ SAI7 TX 8ch                                       │
│                                                                            │
│   E0 minimal : passthrough direct (pas d'effet, pas de matrix)             │
│   E1 → V6.1  : enrichi avec strips IN×8 + matrix 16×8 + strips OUT×8       │
│                                                                            │
│   Attributes : ALWAYS_ON | IGNORE_STOP | NO_HOST                           │
│   Sched comp : DAI capture scheduler (anchor PIPELINE_ALWAYS_ON, 76d819f83)│
│   Period    : 2 ms (DMA scheduling, mémoire feedback_dma_2ms_definitive)   │
└────────────────────────────────────────────────────────────────────────────┘

V6.1 (à ajouter après E0 validé) :

┌─────────────────────────┐    ┌─────────────────────────────────────────────┐
│ PIPE 2 (PCM cap ASIO IN)│    │ PIPE 3 (PCM play ASIO OUT)                  │
│ tap intra-PIPE 1        │    │ /dev/snd/... → host buffer                  │
│ ──→ host buffer ALSA    │    │                ──→ matrix_2x8 src0          │
│ ──→ /dev/snd/... 8ch    │    │                    (intra-PIPE 1)           │
└─────────────────────────┘    └─────────────────────────────────────────────┘
```

E0 = loopback minimal sans effets, sans PCM → priorité = valider que l'always-on activte BCLK + DMA stable, isolément.

---

## 3. Invariants hardware (à NE JAMAIS oublier)

### 3.1. Topologie clock SAI7 + TAC5212

- **SAI7 TX = master** : `TCR2.BCD_MSTR=1` + `TCR4.FSD_MSTR=1`. Quand `TCSR.TERE=1`, BCLK et FSYNC sortent sur les pins.
- **SAI7 RX = slave** : `RCR2.BCD_MSTR=0`, `RCR4.FSD_MSTR=0`, `RCR2.SYNC=0` (async). RX reçoit BCLK/FSYNC depuis ses pins d'entrée.
- **Chaînage hardware** : sur la PCB Debix Model AB, les pins SAI7_TX_BCLK/FSYNC sont **physiquement bouclées** vers SAI7_RX_BCLK/FSYNC (boucle PCB). Donc TX clock vivante = RX clock vivante automatiquement.
- **TAC5212 PLL** : reste lockée sur BCLK/FSYNC. Toute interruption de BCLK = re-lock + glitch audio audible.

### 3.2. Découplage clock vs dataflow

Patch idempotence SAI (mémoire `sof_sai_idempotence_patch`, déjà en place) :
- `sai_set_config()` arme TX TRCE+TERE+MCLK_EN au DAI_CONFIG → BCLK/FSYNC tournent **dès le load topology, 24/7**.
- `FCONT=1` (TCR4) maintient le bit clock même si le FIFO TX est vide (pas de stall).
- `sai_start()` côté TX, branche `sai->configured=true` : ne touche QUE `FRDE` (DMA request enable). Pas de TERE/TRCE/MCLK touchés au runtime.
- `sai_stop()` côté TX : ne clear QUE `FRDE`. Clock reste vivante.

→ Conséquence : l'invariant "TX clock first" est satisfait par construction du driver SAI. À aucun moment du lifecycle V6.0, le BCLK ne s'arrête.

### 3.3. Ordre de démarrage

Hors ALWAYS_ON (cas standard PCM) : `pipeline_trigger_run()` walk topologique unique depuis l'anchor → ordre dicté par direction du host PCM.

V6.0 NO_HOST DAI-to-DAI : pas de host. F4 handler appelle **deux fois** `pipeline_trigger_run()` :
1. Anchor = `p->source_comp` (DAI RX) avec direction CAPTURE → walk UPSTREAM = `bsource_list` vide pour DAI source → seul DAI RX trigger
2. Anchor = `p->sink_comp` (DAI TX) avec direction PLAYBACK → walk DOWNSTREAM = `bsink_list` vide pour DAI sink → seul DAI TX trigger

**Ordre temporel : RX (slave) AVANT TX (master)** dans le handler. TX clock étant déjà active depuis sai_set_config, RX peut démarrer immédiatement, puis TX FRDE consomme B0 qui contient déjà des données fraîches du RX. Pas d'underflow TX au démarrage.

### 3.4. Pourquoi le single-walk standard ne marche pas

Pour un anchor=DAI_RX avec direction CAPTURE (=PPL_DIR_UPSTREAM=1) :
- `pipeline_for_each_comp(DAI_RX, ctx, UPSTREAM)` regarde `bsource_list` de DAI_RX → vide (DAI source = début du pipeline) → walk s'arrête immédiatement
- Seul DAI_RX est trigger, B0 et DAI_TX jamais visités

Pour un anchor=DAI_TX avec direction PLAYBACK (=PPL_DIR_DOWNSTREAM=0) :
- `pipeline_for_each_comp(DAI_TX, ctx, DOWNSTREAM)` regarde `bsink_list` de DAI_TX → vide (DAI sink = fin du pipeline) → walk s'arrête immédiatement
- Seul DAI_TX est trigger, RX et B0 jamais visités

**Donc** : le walk topologique unique ne traverse pas la chaîne RX→B0→TX. Il faut deux walks séparés depuis chaque endpoint pour tout couvrir. C'est exactement ce que fait le dual-anchor F4 (commit `0b28f8074`).

Réf : 5 workers critic convergent sur ce point dans les jobs `5f4ee23e` et `f25615e1`.

---

## 4. Mécanismes SOF utilisés

### 4.1. Patches firmware (branche `feature/v6-always-on-async`)

| ID | Rôle | Fichier | Statut |
|---|---|---|---|
| F1 | Flags `PIPELINE_ATTR_ALWAYS_ON / IGNORE_STOP / NO_HOST` | `src/include/sof/audio/pipeline.h` | ✅ |
| F2 | Auto-trigger via IPC `SOF_IPC_TPLG_PIPE_TRIGGER` après tplg load | `src/ipc/ipc3/handler.c:ipc_glb_tplg_pipe_complete` | ✅ |
| F3 | `pipeline_trigger_run()` swallow STOP/PAUSE pour pipelines IGNORE_STOP | `src/audio/pipeline/pipeline-stream.c:632-644` | ✅ |
| F4 | Handler `ipc_glb_tplg_pipe_trigger` IPC3 dual-anchor RX-then-TX | `src/ipc/ipc3/handler.c:1406-1593` | ✅ (dual-anchor au commit `0b28f8074`) |
| F5 | `module_adapter_set_state` short-circuit `module_sources_all_intra_pipeline` | `src/audio/module_adapter/module_adapter_ipc3.c:142-185` | ✅ |
| F6 | `matrix_2x8` silence-on-empty (output-driven garanti) | `src/audio/matrix_2x8/matrix_2x8.c` | ⏸️ V6.1 (matrix pas dans E0) |

### 4.2. Patches kernel (patcher Yocto)

Fichier patcher : `meta-local/recipes-kernel/linux/files/apply-v6-always-on.py` (additif, idempotent)

| ID | Fichier kernel | Rôle |
|---|---|---|
| K0 | `include/uapi/sound/sof/tokens.h` + `include/sound/sof/header.h` + `include/sound/sof/topology.h` | Token `SOF_TKN_PIPE_ALWAYS_ON` (224), cmd IPC `SOF_IPC_TPLG_PIPE_TRIGGER` (0x014), struct `sof_ipc_pipe_trigger` étendue (rate/ch/fmt/dir) |
| K2 | `sound/soc/sof/ipc3-topology.c` (`pipeline_tokens[]`) | Parser le token `SOF_TKN_PIPE_ALWAYS_ON` |
| K4 | `sound/soc/sof/sof-audio.h` (`struct snd_sof_widget`) | Champ `bool always_on` |
| K5 | `sound/soc/sof/ipc3-topology.c` (helper `sof_ipc3_send_pipe_trigger`) | Helper qui envoie l'IPC PRE_START/START/STOP/PAUSE étendu rate/ch/fmt/dir |
| K1 | `sound/soc/sof/ipc3-topology.c` fin de `sof_ipc3_set_up_all_pipelines` | Itère widgets schedulers, pour ceux flagged always_on=1, envoie via K5 : PRE_START puis START. Warn-not-fail si IPC échoue. |

### 4.3. Patches topology (m4)

| ID | Fichier | Rôle |
|---|---|---|
| T1 | `tools/topology/topology1/m4/sof/tokens.m4` | Définition du token `SOF_TKN_PIPE_ALWAYS_ON` 224 |
| T2 | `tools/topology/topology1/m4/pipeline.m4` | Macro `PIPELINE_ALWAYS_ON_ADD` (dérivée de PIPELINE_PCM_ADD, sans HOST, avec flag always_on=1) |
| T3 | (intégré T6 pour E0) | Pipeline DAI-to-DAI loopback inline |
| T6 | `tools/topology/topology1/sof-imx8mp-tac5212-V6.0.m4` | Topology complète E0 : SAI7 DAI_CONFIG + 1 PIPELINE_ALWAYS_ON_ADD avec sched_comp DAI capture |
| T4 | `tools/topology/topology1/sof/pipe-host-only-capture.m4` | ⏸️ V6.1 (PIPE 2 ASIO IN) |
| T5 | `tools/topology/topology1/sof/pipe-host-only-playback.m4` | ⏸️ V6.1 (PIPE 3 ASIO OUT) |

---

## 5. Plage mailbox réservée V6.0

État actuel des écritures `mailbox_sw_reg_write` ajoutées par V6.0 :

| Offset | Sens | Source code |
|---|---|---|
| 0x720 | sai_start TX count (ring start à 0x210) | sai.c:561-563 (existant E6.b, conservé) |
| 0x724 | sai_start TX-cfg path count | sai.c |
| 0x728 | TCSR snapshot inside sai_start | sai.c |
| 0x730 | sai_start RX count | sai.c |
| 0x7B0 | TCSR après PRE_START handler exit | handler.c F4 (V6.0) |
| 0x7B4 | TCSR après START handler exit | handler.c F4 (V6.0) |
| 0x7B8 | TCSR après STOP handler exit | handler.c F4 (V6.0) |
| 0x7BC | TCSR après PAUSE handler exit | handler.c F4 (V6.0) |
| 0x7C0 | PRE_START handler exit count | handler.c F4 (V6.0) |
| 0x7C4 | START handler exit count | handler.c F4 (V6.0) |

### 5.1. Adresses réservées pour V6.0 E0/E1 nouveau diag

Plage propre confirmée par audit `MAILBOX_DEBUG_MAP.md` : **`0x470-0x4FF` (144 B)**.

Allocation V6.0 E0 (à utiliser dès le prochain build firmware) :

| Offset | Sens proposé | À implémenter |
|---|---|---|
| 0x470 | RCSR snapshot après START handler exit | handler.c F4 (à ajouter) |
| 0x474 | src trigger return code (RX) | handler.c F4 (à ajouter) |
| 0x478 | snk trigger return code (TX) | handler.c F4 (à ajouter) |
| 0x47C | src pipeline_trigger_run call count | handler.c F4 (à ajouter) |
| 0x480 | snk pipeline_trigger_run call count | handler.c F4 (à ajouter) |
| 0x484 | RCSR snapshot après PRE_START | handler.c F4 (à ajouter) |
| 0x488–0x4FF | réserve V6.1 | — |

**Règle stricte** : aucune adresse ne doit être réutilisée d'un commit à l'autre dans la même session de diag (mémoire `feedback_unique_mailbox_addresses`). Si une nouvelle session de diag commence, soit on fait un boot frais (zéro mailbox), soit on alloue une nouvelle plage.

---

## 6. Critères de validation par étape

### E0 — Loopback hardware passthrough sans PCM

Voir fiche `TESTS/TESTS_V6.0_E0.md`. Tests 1-9 :
- DSP boot + tplg load OK
- K1 envoie PIPE_TRIGGER post-fw_run
- F4 handler dual-anchor exécute (compteurs 0x7C0, 0x7C4 ≥ 1)
- sai_start TX et RX appelés (compteurs 0x720, 0x730 ≥ 1)
- TCSR final stable : TE=1, FRDE=1, FEF=0, SEF=0
- BCLK 12.288 MHz continu sur scope
- Audio loopback audible : signal injecté sur RX → entendu sur TX

**GO E0 → E1** si tous les 9 tests PASS.

### E1 — Ajout PCM cap ASIO IN (T4)

Tests à définir après E0 GO. Critère principal : ouverture/fermeture du PCM cap n'arrête pas le loopback E0 (BCLK reste continu, audio loopback continue).

### E2 — Ajout PCM play ASIO OUT (T5 + K4)

Idem critère que E1 mais en playback.

### E3 — Ajout strips IN/OUT + matrix_2x8 (V6.1)

Activation des effets DSP. Inclut F6 (silence-on-empty matrix). Nécessite portage multiband_drc per-channel (étape 0 du plan).

### E4 — NPU tap

Hook `dai_dma_cb` sur SAI TX → reserved-memory dédiée (mémoire `npu_tap_v1_proposal`).

### E5 — Validation finale full duplex

aplay+arecord simultané sur ASIO IN/OUT + écoute loopback continue + amixer cset effets en temps réel.

---

## 7. Hors scope V6.0

- Pas de modification du chemin SOF standard PCM-host-driven (les PCMs E6.a continuent de fonctionner sur d'autres topologies).
- Pas de modification du driver SAI au-delà de l'idempotence existante.
- Pas de modification du driver SDMA.
- Pas de nouveau composant DSP.
- Pas de changement du DTB.
- Pas d'amélioration de la latence ALSA (sprint séparé après V6.0 — mémoire `project_alsa_low_latency_todo`).

---

## 8. Plans de repli

### Plan B — V6.0a (E6.a+)

Si E0 échoue empiriquement au niveau hardware (pas de BCLK même avec dual-anchor + always-on), abandonner l'always-on et basculer sur architecture E6.a + chaîne d'effets en sortie :
- PIPE 1 cap : SAI RX → eq×8 + drc×8 + pga×8 → host PCM 0 (ASIO IN, 8ch)
- PIPE 2 play : host PCM 1 (ASIO OUT) → deinterleave_8 → mixer16 → interleave_8 → eq×8 + drc×8 + pga×8 → SAI TX

Limitations Plan B : pas de loopback hardware mics → SAI TX (besoin user pour audition continue à étudier séparément). Pas de matrix mics tap (pas de cross-pipeline B5).

### Plan C — Firmware DSP custom sans SOF

Si V6.0 et V6.0a échouent toutes les deux : abandonner SOF complètement.
- Reprendre uniquement les modules d'effets (eq_iir.c, drc.c, pga.c, multiband_drc.c) comme bibliothèques C portables.
- Écrire un firmware Zephyr-only minimal sur le M7.
- Implémenter directement : drivers SAI7, drivers SDMA, loopback simple, exposition de 2 PCMs ALSA via driver custom Linux.
- Validé par l'utilisateur comme option viable (produit commercial, pas Arduino, refonte framework acceptable).

---

## 9. Références

- Plan opérationnel : `TESTS/PLAN_V6.0_ALWAYS_ON.md`
- Patches détaillés : `TESTS/PATCHES_V6.0_DETAILS.md`
- Carte mailbox + collisions historiques : `TESTS/MAILBOX_DEBUG_MAP.md`
- Fiche test E0 : `TESTS/TESTS_V6.0_E0.md`
- Investigations critic clés :
  - `5f4ee23e-9e06-46f5-9924-f23b40171b38` — Walk direction wrong (5/6 workers convergent)
  - `f25615e1-0f15-40e7-a704-81d5cde53efd` — Invariant TX-master/RX-slave (5/6 workers convergent)
- Mémoires bloquantes :
  - `feedback_dma_2ms_definitive` — period 2ms
  - `sof_sai_idempotence_patch` — TX clock continue
  - `feedback_unique_mailbox_addresses` — adresses uniques
  - `feedback_critic_only_committed` — critic uniquement sur code pushé
  - `feedback_post_compact_checklist` — 7 points post-compaction
  - `feedback_test_fiches` — format des fiches TESTS
  - `feedback_architecture_doc_per_phase` — ce doc en est l'application
