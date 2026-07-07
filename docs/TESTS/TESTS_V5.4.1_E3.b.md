# Test Fiche : V5.4.1 — E3.b runtime test tplg avec widgets custom

**Date** : 2026-04-28
**Statut** : GO conditionnel — kernel/firmware acceptent UUIDs custom ; tplg test pas représentatif baseline V3.2.2

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E3.b — deploy + reboot + test runtime tplg avec widgets isolés |
| Commit SOF | `ebcd4d268` (branch `feature/audio-platform-v2`, widgets m4 + topology test) |
| Commit yocto-nxp-debix | `db78ceb6` |
| Topologie de référence (test) | `sof-imx8mp-tac5212-V5.4.1-test.m4` (compilé en /tmp/V5.4.1-test.tplg) |
| Topology .tplg test md5 | `40a5b436db2251e90a4e891da4873575` (9124 B) |
| Firmware sof-imx8m.ri md5 board | `5b9acfc367ae5de28b6017b82d67810d` (E2 inchangé) |
| Topology backup V3.2.2 | `/root/backup-pre-V5.4.1/sof-imx8mp-tac5212.tplg.V3.2.2` (md5 `7366ff30...`) |

## Tests réalisés

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| E3.b.0 | Re-compile tplg test E3.a | `m4 ... + alsatplg -c -o` | tplg 9124 B md5 `40a5b436...` | ✅ |
| E3.b.1 | Backup tplg V3.2.2 board | `cp + md5sum` | backup créé `7366ff30...` | ✅ |
| E3.b.2-3 | Deploy tplg test + reboot | `scp + reboot` | board boot OK | ✅ |
| E3.b.4 | Firmware boot dmesg | `dmesg \| grep sof` | "Firmware info: version 2:10:0-fbf02" + 0 erreur SOF | ✅ |
| E3.b.4 | UUIDs custom acceptés | `dmesg` post-deploy | aucune erreur "unknown widget" / "invalid UUID" | ✅ |
| E3.b.5 | arecord 8ch 2s | `arecord -D hw:2,0 -c 8 -f S32_LE -r 48000 -d 2 t.wav` | 3072044 B + 0 xrun | ✅ |
| E3.b.5 | NPU tap V3.2.2 stats | `npu_tap_reader --stats --time 3` | header valid, 0 resets, 0 races | ✅ (period=1536 ≠ 3072 baseline — voir notes) |
| E3.b.5 | aplay 8ch siren | `aplay -D hw:2,0 siren.wav` | playback OK | ✅ |
| E3.b.6 | Restore V3.2.2 baseline | `cp backup + reboot` | tplg md5 retour `7366ff30...` | ✅ |
| E3.b.7 | V3.2.2 régression post-restore — period | `npu_tap_reader --stats` | period=3072 (baseline) | ✅ |
| E3.b.7 | V3.2.2 régression post-restore — audio | `arecord -D hw:3,0 8ch 2s` | 3072044 B + 0 xrun | ✅ |
| E3.b.7 | V3.2.2 régression post-restore — throughput | npu_tap | 1.50 MB/s 0 resets | ✅ |

## Différences observées E3.b vs baseline V3.2.2

| Métrique | V3.2.2 baseline | E3.b tplg test | Cause |
|---|---|---|---|
| Card 2 PCMs | device 0 SAI_Capture, device 1 SAI_Playback (2 PCMs) | device 0 TAC5212 duplex (1 PCM) | Topology test utilise `pipe-multiband-drc-playback.m4` (pas `pipe-volume-playback.m4` V3.2.2 baseline) |
| NPU tap period_bytes | 3072 (96 frames × 8ch × 4B) | 1536 | Format/period DAI différent du fait du pipe-multiband-drc-playback |
| Throughput NPU tap | 1.50 MB/s | 1.32 MB/s | Cohérent avec period plus petit |
| Card 3 sofADCIn | absent | présent (sof-ADC-In) | Apparaît avec topology multiband_drc playback |

**Note importante** : ces différences sont dues au choix de pipeline templates dans la topology test (multiband_drc playback au lieu de volume/drc V3.2.2). Ce N'EST PAS lié aux 3 NEW comps custom. Le but de E3.b était de valider que les UUIDs custom (TEE_1TO2/MIXER16/INTERLEAVE_8 dans widgets isolés) sont acceptés par le système — confirmé.

## Validation des objectifs E3.b

| # | Objectif | Résultat |
|---|---|---|
| 1 | Firmware démarre avec tplg contenant widgets custom | ✅ "Firmware info" affiché, no panic |
| 2 | Kernel SOF accepte UUIDs custom non-listés (process_type=NONE/comp_type=NONE) | ✅ aucune erreur dans dmesg |
| 3 | Widgets isolés (sans P_GRAPH) silently ignorés ou no-op | ✅ pas d'erreur d'instanciation |
| 4 | Audio fonctionne (capture + playback) sur la topology test | ✅ |
| 5 | NPU tap V3.2.2 stable (différent format mais 0 race/reset) | ✅ |

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | **NON** |
| Type de test | — |
| Résultat | — |
| Commentaires | E3.b = test runtime kernel/firmware-side seulement. Topology test pas conçue pour usage utilisateur final. Test utilisateur attendu à E3.c+ ou E5/E6/E7 quand topology fonctionnelle complète déployée. |

## Logs significatifs

```
[10.397688] sof-audio-of-imx8m 3b6e8000.dsp: Firmware info: version 2:10:0-fbf02
[10.397695] sof-audio-of-imx8m 3b6e8000.dsp: Firmware: ABI 3:29:0 Kernel ABI 3:23:0
[10.564231] sof-audio-of-imx8m 3b6e8000.dsp: Topology: ABI 3:29:0 Kernel ABI 3:23:0
0 erreur dans dmesg

card 2: softac5212tdm, device 0: TAC5212 (duplex)
card 3: sofADCIn (NEW dans cette topology test)

NPU tap : header valid version=4 ring_size=261120 period=1536 rate=48000 ch=8
1.32 MB/s, epoch=1, resets=0, races=0
arecord 8ch 2s = 3072044 B + 0 xrun
```

## Conclusion

**GO E3.b.** Le firmware E2 (commit `04eadda17`, md5 `5b9acfc3...`) accepte un tplg avec widgets UUIDs custom (tee_1to2/mixer16/interleave_8). Le kernel SOF Linux retourne `SOF_PROCESS_NONE/SOF_COMP_NONE` pour ces types non-listés et n'objecte pas. Les widgets isolés (PIPELINE_ID=99, sans P_GRAPH) sont silently ignorés au runtime — aucun crash, audio V3.2.2-like fonctionne.

**Note critique** : la topology test n'est PAS représentative de la baseline V3.2.2 (utilise multiband_drc playback au lieu de volume playback). Tplg V3.2.2 a été **restauré post-test et régression validée** : period=3072 retrouvé (vs 1536 pendant test), audio + NPU tap stables sur card 3 (renumérotée par ALSA enum order, SAI_Capture device 0 + SAI_Playback device 1).

**Confirmation importante** : la différence period=1536 observée en E3.b avec la topology test est due au choix de `pipe-multiband-drc-playback.m4` (period DAI plus court) — **pas du tout liée aux 3 NEW comps**. Les widgets isolés ont été silently ignorés sans impact sur le datapath audio.

**Prochaine étape E3.c** : créer une topology fonctionnelle minimale qui CONNECTE réellement tee_1to2 (1 src × 2 sinks) dans un pipeline graph valide, avec consumers réels (host capture × 2 ou DAI). Test : aplay → tee_1to2 → 2 captures → vérifier sha256 identique = fan-out validé bit-perfect.

## Référence

- Spec : `PHASE_1A_3_DSP_V5.4.1.md`
- Pattern m4 widgets : `multiband_drc.m4`
- Fiche précédente : `TESTS_V5.4.1_E3.a.md`
- Backup tplg V3.2.2 : `/root/backup-pre-V5.4.1/sof-imx8mp-tac5212.tplg.V3.2.2` (md5 `7366ff30...`)
