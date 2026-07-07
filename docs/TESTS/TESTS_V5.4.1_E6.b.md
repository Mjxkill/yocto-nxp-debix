# Test Fiche : V5.4.1 — E6.b (matrix_2x8 cleanup)

**Date** : 2026-05-03
**Statut** : NOK (full-duplex KO — bug structurel identifié, hors matrix_2x8)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E6.b — matrix_2x8 8ch native cleanup (post 10 itérations) |
| Commit SOF | `critic/E6b-pivot-matrix2x8-crash` (branch local non-mergé) |
| Commit yocto-nxp-debix | `feature/audio-platform-v2` (commit `0580b5f14` parent) |
| Topologie de référence | `tools/topology/topology1/sof-imx8mp-tac5212-V5.4.1-E6b-pivot.m4` |
| Firmware sof-imx8m.ri md5 (clean E6.b) | `111468f8` (build local, non déployé pour cette fiche) |
| Firmware sof-imx8m.ri md5 board (au moment des tests dans la session) | `7c165399` (iter10 break-attractor) |
| Topology .tplg md5 board | `aab14e0a` (E6b-pivot 13808 bytes) |
| Kernel Image md5 board | inchangé (baseline E6.a) |
| DTB md5 board | inchangé |

## Devices ALSA + audio

| Device | Card | Rôle | Format |
|---|---|---|---|
| `hw:2,0p` (TAC5212 playback) | softac5212tdm card 2 | matrix_2x8 → SAI TX → TAC5212 → speakers | 8ch S32 LE @ 48kHz TDM 8 slots |
| `hw:2,0c` (TAC5212 capture) | softac5212tdm card 2 | TAC5212 mics → SAI RX → eq/drc/pga → tee → ASIO IN | 8ch S32 LE @ 48kHz |

## Tests réalisés (Claude — automatisés sur board)

⚠️ **IMPORTANT** : test 1 sur cette fiche initialement noté "5.218s OK" était TROMPEUR — le DSP avait un état post-loopback-c (PIPE 1 active). Tests refait COLD-BOOT après reboot frais → échec I/O error. Le vrai comportement cold-boot est documenté ci-dessous.

### Résultats COLD-BOOT (après reboot frais)

| # | Test | Commande | Attendu | Résultat cold-boot |
|---|---|---|---|---|
| 1 | Play seul siren 8ch 5s | `aplay -D hw:CARD=softac5212tdm,DEV=0 -c 8 -f S32_LE -r 48000 /root/tests/siren.wav` | ~5.2s | ❌ **0.666s + I/O error** (xrun immédiat — iter10 skip-if-not-period bloque la production car B5 vide en play seul) |
| 2 | Capture seule 5s | `arecord -D ... -d 5 /tmp/cap.wav` | ~5.2s + fichier non-zéro | ✅ **5.245s + 7.68MB** |
| 3 | arecord \| aplay full-duplex pipe Unix 5s | `arecord ... \| aplay ...` | ~5-6s | ❌ **0.724s + I/O error** (même cause que test 1) |
| 4 | loopback-c full-duplex (snd_pcm_link) | `cd /root/tests && timeout 6 ./loopback-c` | out≈48k | ❌ **out=4096 fps stable (12× sous-débit)** |

### Note sur le test 1 "trompeur"

Test 1 fait après que loopback-c avait tourné précédemment (= PIPE 1 cap activée préalablement) → play seul ensuite passe OK 5.218s. Test 1 fait après reboot frais sans cap → I/O error 0.666s.

→ matrix_2x8 **dépend de PIPE 1 cap active** pour pouvoir produire en play seul, AVEC ou SANS iter10. C'est une régression structurelle. Il faut soit :
- (A) Modifier matrix_2x8 pour gérer le cold-boot play-seul (produire période même si sources vides — pas juste skip)
- (B) Garantir que PIPE 1 démarre toujours, même sans capture user

### Comparaison empirique iter10 vs clean (sans iter10)

Test fait avec MÊME tplg, MÊME board, après reboot frais :

| Test | clean (md5 111468f8 sans iter10) | iter10 (md5 7c1653 avec iter10) |
|------|-----------------------------------|----------------------------------|
| Play seul | 0.631s I/O error ❌ | 0.643s I/O error ❌ |
| Cap seule | 5.239s ✅ | 5.247s ✅ |
| arecord\|aplay | 0.735s I/O error ❌ | 0.724s I/O error ❌ |
| **loopback-c** | **out=0 stuck (bloqué après 1 chunk)** ❌ | **out=4096 fps stable** ⚠️ |
| Score | 1/4 PASS | 1/4 PASS |

**iter10 est NÉCESSAIRE** : sans, le mixer bloque totalement en loopback-c (out=512 stuck après 1 chunk). Avec, il produit 4096 fps (déficit 12× mais non-bloqué).

iter10 reste donc dans le code final.

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | NON |
| Type de test | écoute audio sur haut-parleurs |
| Résultat | **PAS DE SON** sur play seul malgré rate=48k OK (matrice identité gain Q1.31 INT32_MAX) — bug "rate OK pas de son" séparé du bug 4k fps full-duplex |
| Commentaires | Le test play seul a passé 5.218s confirmant le débit ALSA, mais aucun son entendu. Bug de routing TDM / DAC TAC5212 à investiguer indépendamment. |

## Tests à intégrer (régression script créé)

Script `/root/tests/regression_full.sh` déployé sur board avec 4 tests :
1. play seul siren 8ch (durée + match expected)
2. cap seule (durée + taille fichier)
3. `arecord | aplay` full-duplex pipe Unix
4. loopback-c (parse last out fps)

Sortie pass/fail global. À lancer à chaque firmware build avant de présenter résultat.

## Itérations matrix_2x8 testées (toutes empiriquement = 4096 fps stable en loopback-c)

| Iter | Approche | Résultat |
|---|---|---|
| 1 | set_underrun(B5) cross-pipeline trigger | DSP crash @ t=28.5s |
| 2 | wrap fix per-frame advance | DSP stable, **out=4096** |
| 3-4 | avail filter (skip src avail=0, real_avail via bsource_list walk) | out=4096 |
| 5 | force min_frames = mod->dev->frames | out=4096 |
| 6 | hardcode min_frames = 96 + bypass sink_free clamp | out=4096 |
| 7 | conversion mode SOURCE_SINK → AUDIO_STREAM (.process_audio_stream) | out=4096 |
| 8 | MAX-over-sources au lieu de MIN | out=4096 |
| 9 | audio_stream_set_align(8, 2) dans prepare | out=4096 |
| 10 | skip-if-not-period (claude-code Approche B) | out=4096 |

**Cleanup (ce commit)** :
- revert iter8 (MAX-over-sources — empiriquement = MIN, sémantiquement MIN est meilleur)
- iter10 RESTE (vérifié empiriquement nécessaire, sans il bloque loopback-c à out=0)
- iter6 (MATRIX_2X8_PERIOD_FRAMES define) : garde car utilisé par iter10
- iter2 (wrap fix per-frame manuel) : OBSOLETE, remplacé par iter7 fragments

**Code final** : wrap fix via fragments (iter7) + avail filter MIN (iter4) + AUDIO_STREAM mode (iter7) + set_align(8,2) (iter9) + set_underrun cross-pipeline (iter1) + force-period-or-skip (iter10). Code maintenable.

## Logs significatifs

```
=== Test play seul ===
Playing WAVE '/root/tests/siren.wav' : Signed 32 bit Little Endian, Rate 48000 Hz, Channels 8
real    0m5.218s    ← OK (= 48k fps)
(no audio heard on speakers — separate bug)

=== Test cap seule ===
Recording WAVE '/tmp/cap_test.wav' : Signed 32 bit Little Endian, Rate 48000 Hz, Channels 8
real    0m5.242s    ← OK
md5: 3f0406c725a0efc42ec12e271a43edfa  /tmp/cap_test.wav (7.68MB)

=== Test loopback-c ===
in=46592 (+46592 f/s) out=3840  (+3840 f/s) xrun cap=0 play=16 ring_drop=150
in=93696 (+47104 f/s) out=7936  (+4096 f/s) xrun cap=0 play=32 ring_drop=302
... (linéaire +4k/sec, KO)

=== dmesg ===
[10.430] sof: Firmware info: version 2:10:0-0580b
[10.504] sof: tplg: config SAI7 fmt 0x4004 mclk 12288000 width 32 slots 8
(no errors, DSP stable)
```

## TEST D'ISOLATION B5 (2026-05-03) — CAUSE STRUCTURELLE CONFIRMÉE

Sur proposition GLM (job critic_research `183689a7-904a-40cc-85cc-b838463fcaf5`, durée 1441s) : retirer le SectionGraph cross-pipeline B5 (PIPE 1 mics tap → matrix_2x8 source 1) pour déterminer empiriquement si la dépendance cross-pipeline est la cause des échecs.

### Variantes créées

| Fichier | Description |
|---|---|
| `tools/topology/topology1/sof/pipe-matrix2x8-playback.m4` | Variante pipe play : matrix_2x8 connecté à B0 SEUL (pas de cross-pipeline) |
| `tools/topology/topology1/sof-imx8mp-tac5212-V5.4.1-E6b-test-no-b5.m4` | Topology de test : pipe cap simple (sans tee_1to2/B5) + pipe play matrix B0-seul, PAS de SectionGraph cross-pipeline |

Firmware INCHANGÉ (md5 `1d69f647`, code matrix_2x8 NAÏF, 0 patch). Seule la topology change.

### Résultats COLD-BOOT (firmware NAÏF md5 1d69f647 + tplg test-no-b5 md5 1d0c15ed)

| # | Test | Baseline E6b-pivot (avec B5) | Test no-B5 (sans B5) |
|---|---|---|---|
| 1 | Play seul siren 8ch 5s | ❌ 0.666s + I/O error | ✅ **5.235s PASS** |
| 2 | Capture seule 5s | ✅ 5.245s | ✅ **5.247s PASS** |
| 3 | arecord \| aplay full-duplex pipe Unix 5s | ❌ 0.724s + I/O error | ✅ **5.330s PASS** |
| 4 | loopback-c full-duplex (snd_pcm_link) | ❌ out=4096 fps (12× sous-débit) | ✅ **out=46848 fps PASS** (×11.4) |
| **Score** | **1/4 PASS** | **4/4 PASS** |

### Conclusion empirique

**La cause structurelle est définitivement la dépendance cross-pipeline B5** (PIPE 1 mics tap → matrix_2x8 source 1). Avec B5 retiré :
- `pipeline_lock_branched` ne détecte plus matrix_2x8 comme multi-source → plus de lock channels=1 sur source buffers
- Plus de B5 vide à consommer → plus de phantom frames / corruption circular buffer

Cela confirme l'analyse GLM (Bugs #1 + #2 dans `pipeline-graph.c:303-331` et `module_adapter.c:649-688`).

### Fiche utilisateur (test isolation)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | NON (test automatisé Claude) |
| Type de test | regression_full.sh sur board cold-boot |
| Résultat | 4/4 PASS — débit 48k confirmé sur tous les chemins |

⚠️ **Important** : ce test n'utilise PAS la matrix 16×8 complète (mics + ASIO play). Il prouve seulement que sans le tap mics cross-pipeline, le système marche. Le défi reste de **réintégrer le tap mics proprement** sans casser le mixer.

### Options architecturales pour la réintégration mics → matrix (à étudier)

1. **Topology autostart PIPE 1** : forcer PIPE 1 toujours active même sans capture user (B5 toujours initialisé/triggered).
2. **Fix framework `module_adapter.c`** : skip cross-pipeline sources dont le pipeline source n'est pas actif (`source[i]->source->state != dev->state`). Recommandation GLM.
3. **Repenser l'architecture** : pas de cross-pipeline. Tap mics via mécanisme alternatif (deux PIPE play séparés, comp dédié intra-pipeline, etc).

À investiguer en multi-worker avant tout code.

## TEST OPTION B (kimi) — module_adapter.c skip cross-pipeline inactive — ÉCHEC EMPIRIQUE

**Date** : 2026-05-03 (suite au test isolation B5 confirmé)

**Investigation** : job critic_research `37ee64a1-3077-40f8-ab97-3b1b77d278e6` (6 workers : kimi/minimax/qwen/glm/claude-code/deepseek). Choix utilisateur : pragmatique court = Option B + Option E.

**Patch appliqué** : `sof/src/audio/module_adapter/module_adapter.c`, fonction `module_single_sink_setup` — ajout d'un skip pour les sources cross-pipeline dont `source->state != COMP_STATE_ACTIVE`. Option E (kimi auto-prime tee_1to2) **non appliquée** car analyse révisée du code montre `underrun_permitted=false` sur B5 (pas de phantom-avail mécanisme — tee_1to2 pose `overrun_permitted` pas `underrun_permitted`).

**Build** : firmware md5 `bf37d5c6` (vs naïf `1d69f647`). Tplg restauré E6b-pivot original `aab14e0a` (B5 reconnecté).

### Résultats COLD-BOOT (firmware fix-B + tplg E6b-pivot avec B5)

| # | Test | Naïf (1d69f647) | Fix-B (bf37d5c6) |
|---|---|---|---|
| 1 | Play seul siren 8ch 5s | 0.666s ❌ | 0.647s ❌ |
| 2 | Capture seule 5s | 5.245s ✅ | 5.124s ✅ |
| 3 | arecord \| aplay 5s | 0.724s ❌ | 0.746s ❌ |
| 4 | loopback-c full-duplex 5s | 4096 fps ❌ | **0 fps stuck** ❌ (régression) |
| **Score** | **1/4 PASS** | **1/4 PASS** |

### Conclusion empirique

**Option B SEULE = ÉCHEC**. Pire : loopback-c passe de 4096 fps à 0 fps stuck (out=256 stable, +0 fps), avec xrun play=1. La logique de skip cross-pipeline est fausse OU `source[i]->source->state` ne reflète pas l'activité réelle du buffer même en loopback-c (PIPE 1 et PIPE 2 toutes deux ACTIVE).

Hypothèse : le check `source[i]->source->state != COMP_STATE_ACTIVE` est asynchrone par rapport à l'état du buffer B5. Pendant les premiers ticks de loopback-c, tee_1to2 (source de B5) n'est pas encore en ACTIVE alors que la capture commence à arriver — le skip incorrect bloque la production matrix.

### Revert immédiat (mandat user)

- Code source local : reverté (`module_single_sink_setup` original)
- Firmware board : reverté à `1d69f647` (backup `.bak-pre-fixB` restauré)
- Tplg board : reste `aab14e0a` E6b-pivot (B5 reconnecté) pour préparer la prochaine option

### Prochaine option à tester

Option B + D (fix Bug #1 dans `pipeline_lock_branched` pour exclure cross-pipeline du lock channels=1) — recommandée par minimax/glm/deepseek/qwen/claude-code (5/6 workers).

Alternativement, Option F (claude-code) : migrer `matrix_2x8` vers l'API SOURCE_SINK (`source_get_data_frames_available`) au lieu du module_adapter legacy. À évaluer.

## Conclusion

**État E6.b avec B5 (pivot d'origine)** : 1/4 PASS — NOK majoritaire.
**État E6.b SANS B5 (test isolation)** : **4/4 PASS** — cause structurelle B5 cross-pipeline isolée et confirmée.
**État E6.b avec Fix-B (skip cross-pipeline inactive seul)** : 1/4 PASS — fix insuffisant, reverté. Investigation Option D ou F.

## TEST OPTION G (sched_comp partagé) — ÉCHEC EMPIRIQUE 0/4

**Date** : 2026-05-03

**Patches appliqués** :
- Création `sof/tools/topology/topology1/sof/pipe-dai-sched-capture.m4` (analogue de pipe-dai-sched-playback.m4 pour capture, utilise SCHED_COMP au lieu de N_DAI_IN dans le W_PIPELINE)
- Modification `sof-imx8mp-tac5212-V5.4.1-E6b-pivot.m4` : inversion ordre PIPE 2 puis PIPE 1, PIPE 1 utilise `DAI_ADD_SCHED` avec sched_comp = `PIPELINE_PLAYBACK_SCHED_COMP_2` (= SAI7 TX DAI de PIPE 2)
- Firmware clean (sans diag, md5 5c159e42), tplg Option G md5 0cc7d579

**Résultat** : **0/4 PASS — pire que baseline 1/4 PASS**.

| Test | Naïf 1/4 | Option G 0/4 |
|---|---|---|
| Play seul | 0.666s ❌ | 0.627s ❌ |
| Cap seule | 5.245s ✅ | 0.634s I/O error ❌ (régression cap) |
| arecord\|aplay | 0.724s ❌ | 0.799s ❌ |
| loopback-c | 4096 fps ❌ | 0 fps stuck ❌ |

**Cause de l'échec** : avec PIPE 1 capture utilisant SAI7 TX comme sched_comp, PIPE 1 task ne tourne plus quand SAI7 TX inactive (cap seule sans aplay → SAI TX cb ne fire pas → PIPE 1 task jamais wake → cap fail). Le sched_comp partagé crée une dépendance unidirectionnelle nuisible.

**Revert immédiat (mandat)** : code source m4 reverté via `git checkout`, fichier pipe-dai-sched-capture.m4 supprimé. Firmware clean conservé (sans diag) — utilisable pour les prochains tests. Tplg E6b-pivot original restauré sur board (md5 aab14e0a, retour 1/4 PASS confirmé).

## TEST OPTION B + D combiné — ÉCHEC EMPIRIQUE

**Date** : 2026-05-03

**Patches appliqués** :
- D : `sof/src/audio/pipeline/pipeline-graph.c` `pipeline_lock_branched` — exclure les buffers cross-pipeline du lock channels=1 (variante glm)
- B : `sof/src/audio/module_adapter/module_adapter.c` `module_single_sink_setup` — skip cross-pipeline inactives (variante précédente)

**Build** : firmware md5 `bc10ce57`. Tplg E6b-pivot inchangé `aab14e0a` (B5 connecté).

### Résultats

| # | Test | Naïf | B seul (échec) | **B+D (échec)** |
|---|---|---|---|---|
| 1 | Play seul | 0.666s ❌ | 0.647s ❌ | **0.626s ❌** |
| 2 | Cap seule | 5.245s ✅ | 5.124s ✅ | **5.238s ✅** |
| 3 | arecord\|aplay | 0.724s ❌ | 0.746s ❌ | **0.736s ❌** |
| 4 | loopback-c | 4096 fps ❌ | 0 fps stuck | **0 fps stuck** |
| **Score** | 1/4 | 1/4 | **1/4** |

### Conclusion empirique

**B + D = échec, identique à B seul.** Loopback-c reste stuck à `out=256, +0 fps` après 1 chunk produit. La différence avec naïf : naïf produisait 4096 fps continus (sous-débit mais constant) ; avec B/B+D matrix produit 1 chunk puis bloque définitivement.

Hypothèse révisée : Bug #1 (channels=1 lock) n'était PAS le mécanisme principal de l'échec loopback-c. Le vrai bug est ailleurs — probablement dans la logique de skip ou dans une race state à la transition PIPE 1 PRE_START → ACTIVE. Le check `source[i]->source->state != COMP_STATE_ACTIVE` se déclenche au mauvais moment.

### Revert (mandat)

- Source local : reverts D + B
- Firmware board : `1d69f647` restauré (depuis `.bak-pre-fixB`)
- Tplg : reste `aab14e0a` (B5 connecté)

### Options encore non testées

- **F (claude)** : Migrer matrix_2x8 vers API SOURCE_SINK (mixer16-style) — évite complètement la chaîne `module_single_sink_setup`. Refactoring matrix_2x8.c.
- **H (claude)** : Comp matrix lit hw_params source distante dans prepare (init B5 indépendamment du timing PIPE 1).
- **D variante claude** : Lire `source->channels` au lieu de hardcoder 1 dans pipeline_lock_branched.
- **Investigation supplémentaire** : ajouter logs trace pour voir pourquoi le skip s'enclenche en loopback-c (ce qui ne devrait pas arriver puisque PIPE 1 ACTIVE).

**Découvertes empiriques (COLD-BOOT) avec firmware iter10 md5 7c1653** :
1. matrix_2x8 a un comportement DÉPENDANT de l'état précédent du DSP (post-loopback-c vs cold-boot frais).
2. iter10 RESTE nécessaire — sans iter10, loopback-c bloque totalement (out=0 stuck après 1 chunk).
3. Play-seul cold-boot échoue MÊME AVEC iter10 (I/O error 0.666s).
4. **PIPE 1 cap doit être amorcée** pour que matrix_2x8 puisse produire en play-seul. C'est une dépendance cachée.

## Hypothèse user — propagation paramètres B5 cross-pipeline

User : "Sans le mixer tout fonctionne, l'ajout du mixer a cassé la sortie. Le mixer est DIRECTEMENT lié à la sortie. Tous les patches sont sans analyse réelle du code du mixer et de son impact dans la chaîne de propagation des blocs."

**Hypothèse forte** : le buffer B5 (pont mics-vers-mixer, créé dans PIPE 1) n'est PAS initialisé en cold-boot play-seul tant que PIPE 1 cap n'est pas triggered. Ses params (rate=48k, channels=8, format=S32, frame_bytes=32) sont mis par `comp_verify_params` quand PIPE 1 est triggered. Si PIPE 1 jamais triggered :
- B5 garde params init defaults (channels=0, rate=0, frame_bytes=undefined)
- matrix's `module_single_sink_setup` lit `input_buffers[1]` (B5) avec une stream non-initialisée
- `audio_stream_avail_frames_aligned` divise par frame_bytes=0 ou retourne valeurs anormales
- matrix process choke → ALSA xrun → I/O error

Post-loopback-c : PIPE 1 a été triggered au moins 1 fois → B5 params persistent dans struct comp_buffer même après stream stop → matrix process OK.

**Pistes d'investigation** :
1. Le mixer ne propage peut-être pas les params à ses 2 sources (set_channels seulement, pas rate/format)
2. La SectionGraph cross-pipeline ne crée peut-être pas le binding params suffisamment tôt
3. La chaîne pipeline_params walk pour PIPE 2 ne touche pas B5 (qui appartient à PIPE 1)
4. matrix_2x8 doit peut-être faire l'init B5 lui-même via audio_stream_set_rate/set_frm_fmt

**Direction prochaine session** :
1. Investigation focalisée propagation paramètres mixer → sink B100 + sources B0/B5
2. Comparer pas-à-pas avec sans-mixer (E5.e baseline) où ça marchait
3. **NE PAS rajouter de patches sans analyse code du mixer**. Investigation full code review du mixer.
4. **Lessons learned** : NE PAS faire confiance à un test "OK" sans reboot frais. Toujours valider cold-boot.

## Session 2026-05-04 — Instrumentation lourde + tests Alt #5 + prepare-guard + isolation B5 reconfirmée

**Objectif session** : trouver la cause exacte de TEST 1 PLAY SEUL cold-boot FAIL via instrumentation mailbox du driver SOF (sai.c, sdma.c, dai-legacy.c, pipeline-params.c, matrix_2x8.c, pipeline-schedule.c, zephyr_dma_domain.c).

### Instrumentation phase 1/2/3 — counters mailbox ajoutés

| Offset | Champ | Source instrumentée |
|---|---|---|
| 0x100/104 | matrix_2x8 tick | matrix_2x8.c:194 |
| 0x570/574 | PIPE1/PIPE2 task counter | pipeline-schedule.c |
| 0x580/588/58C | DMA IRQ entry/p1/p2 | zephyr_dma_domain.c |
| 0x5A0-5A8 | sdma_start entry/idx/status | sdma.c sdma_start |
| 0x5B0/5B4 | HSTART count/lastIdx | sdma.c sdma_start |
| 0x600-60C | sdma_copy entry/idx/bytes/chan6 | sdma.c sdma_copy |
| 0x610-624 | dai_dma_cb / dai_common_copy par direction | dai-legacy.c |
| 0x680-69C | sdma_copy par chan_index 0..7 | sdma.c sdma_copy |
| 0x6A0 | sdma_chan_type pour chan 3 | sdma.c |
| 0x6A4/6A8 | dai_dma_copy_legacy par direction | dai-legacy.c |
| 0x6B0-6CC | sdma_start par chan_index 0..7 | sdma.c sdma_start |
| 0x6D0/6D4 | sdma_chan_type / hw_event au start | sdma.c |
| 0x700-758 | SAI : set_cfg count, TCSR, RCSR, MCTL, sai_start_TX/RX, sai_stop, sai_trigger | sai.c |
| 0x780-7C0 | sdma_set_config par chan + direction | sdma.c sdma_set_config |

### Tests cette session (firmware md5 + tplg md5 référencés)

| # | Firmware | Tplg | Modif | TEST 1 PLAY | TEST 2 CAP | TEST 3 dup | TEST 3.5 PLAY après cap | TEST 4 loopback-c | Score |
|---|---|---|---|---|---|---|---|---|---|
| 1 | b2f907b1 | aab14e0a (B5) | Baseline pré-session | FAIL 0.65s | PASS 5.12s | PASS 5.69s | — | non observé | 2/3 |
| 2 | b2f907b1 | aab14e0a (B5) | Retest baseline | FAIL 0.65s | PASS 5.12s | PASS 6.61s | — | non observé | 2/3 |
| 3 | a3165e1a | aab14e0a (B5) | Alt #5 underrun_permitted matrix_2x8_trigger | FAIL 0.65s | PASS 5.12s | **FAIL 1.18s** | — | FAIL 512 fps stuck | **1/4** |
| 4 | 955428708c | aab14e0a (B5) | Revert Alt #5 | FAIL 0.62s | PASS 5.23s | PASS 5.29s | — | FAIL 26.6k dégradé | 2/4 |
| 5 | 955428708c | aab14e0a (B5) | Manuel TEST 3 puis PLAY | — | — | FAIL 1.20s | **PASS 5.23s** | — | diag |
| 6 | 955428708c | aab14e0a (B5) | Cold-boot avec TEST 3.5 inclus | FAIL 0.62s | PASS 5.23s | PASS 5.25s | **PASS 5.20s** | FAIL 256/512 stuck | 3/5 |
| 7 | 955428708c | aab14e0a (B5) | TEST 4 buffer=2048 10s | — | — | — | — | FAIL recover_fail puis 0 fps | diag |
| 8 | 4818db44 | aab14e0a (B5) | Phase 1 SAI instrumentation | FAIL 0.63s | — | — | — | — | diag |
| 9 | ec939a23 | aab14e0a (B5) | Phase 2 per-chan sdma counters | FAIL 0.63s | PASS | — | PASS | — | diag |
| 10 | 5295ad9d | aab14e0a (B5) | Phase 3 sdma_set_config trace | FAIL 0.65s | — | — | — | — | diag |
| 11 | 5295ad9d | aab14e0a (B5) | Variabilité 3× cold-boot | 3× FAIL (0.63/0.64/0.64s) | — | — | — | — | 0/3 |
| 12 | 5295ad9d | aab14e0a (B5) | Reboot + sleep 30s + TEST 1 | FAIL 0.63s | — | — | — | — | 0/1 |
| 13 | 5295ad9d | aab14e0a (B5) | Reboot + dmesg -c + TEST 1 | FAIL 0.67s | — | — | — | — | 0/1 |
| 14 | 5295ad9d | aab14e0a (B5) | Reboot + mailbox warmup + TEST 1 | FAIL 0.63s | — | — | — | — | 0/1 |
| 15 | 5295ad9d | aab14e0a (B5) | rmmod imx_audio_tap | FAIL 0.63s | — | — | — | — | 0/1 |
| 16 | 5295ad9d | **1d0c15ed (NO-B5)** | Diagnostic isolation B5 | **PASS 5.23s** | — | — | — | — | 1/1 |
| 17 | 5295ad9d | **1d0c15ed (NO-B5)** | Régression complète NO-B5 | **PASS 5.10s** | **PASS 5.12s** | **PASS 5.21s** | — | **PASS 47872 fps** | **🎯 4/4** |
| 18 | 669a9f14 | aab14e0a (B5) | Fix prepare-guard COMP_STATE_ACTIVE | FAIL 0.70s | PASS 5.24s | PASS 5.33s | — | FAIL "no such device" | 2/4 |
| 19 | 5295ad9d | aab14e0a (B5) | Re-revert (interrompu) | non testé | — | — | — | — | — |

### Lectures empiriques clés via instrumentation mailbox

**Cold-boot AVANT tout test (firmware 4818db44, B5)** :
```
0x700 sai_set_cfg_tag       = 0xCAFE5A17
0x704 sai_set_cfg_count     = 1                     ← sai_set_config a tourné au boot
0x708 TCSR                  = 0x90010000            ← TX TE=1, BCE=1, MCLK déjà actif
0x70C RCSR                  = 0x00000000
0x710 MCTL                  = 0xc0000000            ← MCLK_EN=1
0x720 sai_start_TX          = 0xffffffff (uninit)   ← sai_start TX jamais appelé
0x730 sai_start_RX          = 1                     ← RX a été start au boot
0x744 sai_stop_RX           = 1                     ← puis stoppé
0x574 PIPE2_task            = 0xffffffff (jamais)
0x600 sdma_copy total       = 0x2051 (8273 calls)   ← SDMA tourne déjà
0x5B0 HSTART_cnt            = 1                     ← chan 4 hardware-started au boot
```

**Conclusion #1** : Le clock SAI TX EST déjà actif au cold-boot (sai_set_config tourne au load topology). L'hypothèse user "le clock TX ne se génère pas au play" est **infirmée**.

**APRÈS TEST 1 PLAY SEUL FAIL (firmware 5295ad9d, B5)** :
```
0x574 PIPE2_task            = 0x00000001            ← wake 1 SEULE fois
0x570 PIPE1_task            = 0x000005e1 (1505)     ← inchangé (PIPE 1 STOP complet pendant TEST 1 !)
0x600 sdma_copy total       = +16 calls seulement   ← DMA quasi-figé
0x60C chan6_copy            = 0xffffffff (jamais)   ← chan 6 inactif
0x690 chan4_copy            = +2 calls              ← chan 4 quasi-figé
0x68C chan3_copy            = +15 calls             ← chan 3 quasi-figé
0x6BC chan3_start           = 2 (was 1)             ← chan 3 RE-STARTED
0x6C0 chan4_start           = 2 (was 1)             ← chan 4 RE-STARTED
0x6D0 last_chan_type_at_start = 4 (MCU2SHP=TX !)    ← dernier start fut TX
0x790 chan4_setcfg          = 2 (DEUX set_configs !) ← collision
0x7B0 chan4_dir             = 0x8 (MEM_TO_DEV=TX)   ← chan 4 reconfiguré RX→TX
0x7AC chan3_dir             = 0x2 (HMEM_TO_LMEM)    ← chan 3 reconfiguré aussi
0x720 sai_start_TX          = 1                     ← sai_start TX appelé
0x728 TCSR_after_TX         = 0x90170001            ← FRDE=1 set OK
```

**Conclusion #2** : Pendant TEST 1 PLAY SEUL FAIL :
- chan 4 (qui servait SAI RX cap auto-running) est reconfiguré en SAI TX play
- chan 3 (cross-pipeline AP2AP) est reconfiguré HMEM_TO_LMEM (play host data)
- PIPE 1 cap **freeze complètement** (0 ticks supplémentaires)
- PIPE 2 task wake une seule fois puis stop
- dai_common_copy_play = 2 calls puis stop

**APRÈS TEST 1 NO-B5 PASS (firmware 5295ad9d, tplg 1d0c15ed)** :
```
chan4_setcfg            = 1 (UN seul set_config)    ← pas de double-config
chan4_dir               = 0x8 (TX)                  ← idem mais une fois
PIPE2_task              = 2513 (5s × 500/s)        ← fire normalement
dai_common_copy_play    = 2521                      ← 5s × 500/s normal
HSTART_cnt              = 1                         ← un seul HSTART
```

**Différence empirique avec/sans B5 = 1 set_config supplémentaire sur chan 4 quand B5 est dans la topology**.

### Comportement reproductible 100%

- Cold-boot direct TEST 1 PLAY SEUL : 5/5 FAIL avec B5 (durées 0.620-0.695s)
- TEST 1-bis PLAY SEUL après TEST 3 : 2/2 PASS (5.221s, 5.230s)
- TEST 3.5 PLAY SEUL après TEST 2 cap : 1/1 PASS (5.205s, 5.230s)
- Sans B5 (tplg 1d0c15ed) : 4/4 PASS

### Tentative de fix Phase 3 — ÉCHEC

**Patch testé** : ajout du guard `if (current->state == COMP_STATE_ACTIVE) return 0;` dans `pipeline_comp_prepare` (sof/src/audio/pipeline/pipeline-params.c:264-296), miroir du guard existant dans `pipeline_comp_params:94`.

**Justification a priori** : empêcher re-prepare destructif d'un comp ACTIVE quand walk cross-pipeline B5 atteint un comp PIPE 1 actif.

**Résultat empirique (firmware 669a9f14, tplg avec-B5)** :
| Test | Avant fix | Après fix |
|---|---|---|
| TEST 1 PLAY SEUL | FAIL 0.62s | FAIL 0.70s |
| TEST 2 CAP | PASS | PASS 5.24s |
| TEST 3 duplex | PASS 5.29s | PASS 5.33s |
| TEST 4 loopback | FAIL | FAIL "no such device" |

→ 2/4 PASS, **fix inefficace**. Le guard `COMP_STATE_ACTIVE` n'a pas changé le comportement — la double set_config sur chan 4 vient d'**ailleurs** que de pipeline_comp_prepare.

**Action** : revert immédiat (per mandate "no fallback"). Source local revert OK, firmware 5295ad9d redéployé.

### Conclusion session 2026-05-04

1. **Instrumentation lourde validée** : 50+ counters mailbox actifs. Lectures empiriques fiables.
2. **Cause racine ÉTABLIE empiriquement** : avec B5 cross-pipeline, chan 4 SDMA reçoit un set_config supplémentaire qui le reconfigure de SAI RX vers SAI TX au cold-boot du 1er PLAY → casse PIPE 1 + ne donne pas de DMA TX exploitable.
3. **L'extra set_config n'est PAS dans pipeline_comp_prepare** (fix testé inefficace). Reste à identifier précisément quel code path déclenche ce 2e set_config quand B5 est présent.
4. **6 workers convergent dans investigations passées** sur la propagation de params (frame_fmt/rate) entre pipelines via cross-pipeline B5. Hypothèse : matrix_2x8_prepare ne propage pas frame_fmt/rate, et tee_1to2_prepare ne propage pas vers cross-pipeline sinks.
5. **Aucune solution validée à ce jour**. Investigation pure proposée pour identifier le code path exact.

## Référence (autres docs/spec liées)

- Spec : N/A (tactique)
- Investigations critic : jobs `2566539a`, `0222b73e`, `a54df037`, `daeb077a`, `4c58e837`, `fc862f10`, `fc1bab9b`, `bc580f54`, `183689a7-904a-40cc-85cc-b838463fcaf5` (le dernier = analyse GLM 79.5KB qui a proposé le test d'isolation décisif), `96559baf-379a-48ab-8c56-19128460bef6`, `ff02050a-0a37-4d6e-b10d-feba0b0cc09e`, `70e7b496-7f56-4d9b-914f-22a7c709d044`
- Fiche précédente : `TESTS_V5.4.1_E6.a.md` (matrix mixer16 alt, abandonnée pour matrix_2x8 pivot)
- Spec V5 : 8 strips IN + matrix 16×8 + 8 strips OUT (project_v5_architecture.md)
- Script régression v3 : 9 tests (2× round PLAY/CAP/duplex + 3 loopback buffer 2048/512/256)
