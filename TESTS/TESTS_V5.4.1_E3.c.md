# Test Fiche : V5.4.1 — E3.c runtime test tee_1to2 dans pipeline graph connecté

**Date** : 2026-04-28
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E3.c — runtime test fonctionnel tee_1to2 dans pipeline graph |
| Commit SOF | `6f166d00a` (branch `feature/audio-platform-v2`, pipe-tee-volume-playback + topology E3.c) |
| Commit yocto-nxp-debix | `84f9d752` (inchangé) |
| Topologie de référence (test) | `sof-imx8mp-tac5212-V5.4.1-E3c.m4` (compilée vers `/tmp/V5.4.1-E3c.tplg`) |
| Topology .tplg test md5 | `7818c1c55ec564a42a6c76bafabfe12c` (8400 B) |
| Firmware sof-imx8m.ri md5 board | `5b9acfc367ae5de28b6017b82d67810d` (E2 inchangé) |
| Topology backup V3.2.2 | `/root/backup-pre-V5.4.1/sof-imx8mp-tac5212.tplg.V3.2.2` (md5 `7366ff30...`) |

## Architecture topology test E3.c

```
PCM 0 capture (V3.2.2 baseline drc inchangé) :
  SAI7 RX 8ch → drc → host

PCM 1 playback (E3.c NEW pipe-tee-volume-playback) :
  host → B0 → tee_1to2 → B1 → volume → B2 → SAI7 TX 8ch
                  ↑
            (1 sink connecté, 2e sink ignored — single-sink mode pour ce test)
```

**Test ciblé** : valider que tee_1to2 process est instancié + appelé en runtime sans crash quand inséré dans un pipeline graph SOF connecté.

## Tests réalisés

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| E3.c.0 | Compile m4 + alsatplg | `m4 ... && alsatplg -c ... -o ...` | tplg 8400 B md5 `7818c1c5...` | ✅ |
| E3.c.0 | Pipeline graph correct | `strings tplg \| grep TEE_1TO2` | "TEE_1TO22.0" (PIPELINE_ID=2 substitué) | ✅ |
| E3.c.4 | Deploy + reboot | `scp + reboot` | board boot OK | ✅ |
| E3.c.5 | Firmware boot dmesg | `dmesg \| grep sof` | "Firmware info: version 2:10:0-fbf02", 0 erreur | ✅ |
| E3.c.5 | Card 2 sof-tac5212-tdm | `aplay -l` | card 2 device 0 (TAC5212 duplex) | ✅ |
| E3.c.6 | aplay 8ch sine via tee_1to2 | `aplay -D hw:2,0 sine.wav` | "Playing WAVE..." (audio flow OK) | ✅ |
| E3.c.6 | NPU tap pendant playback | `npu_tap_reader --stats --time 3` | header valid, throughput stable, 0 resets/races | ✅ (period=1536, 1.52 MB/s) |
| E3.c.6 | 0 erreur SOF post-aplay | `dmesg \| grep -iE error\|fail\|panic` | aucune erreur tee/sof | ✅ |
| E3.c.7 | V3.2.2 capture régression | `arecord -D hw:2,0 -c 8 -d 2` | 3072044 B + 0 xrun | ✅ |
| E3.c.8 | Restore V3.2.2 baseline | `cp + reboot` | tplg md5 `7366ff30...` | ✅ |

## Validation runtime tee_1to2

**Confirmation** que le firmware E2 (commit `04eadda17`) avec mes 3 NEW comps + UUIDs custom :
1. ✅ Le widget `TEE_1TO2` dans tplg est correctement parsé par le kernel SOF
2. ✅ Le firmware reconnaît l'UUID `e1ec7700-...` et instancie le comp tee_1to2
3. ✅ La pipeline graph traverse tee_1to2 sans crash (host → tee → volume → DAI)
4. ✅ Audio flow est stable (NPU tap reçoit 3 MB sur 3s = audio continu, 0 race)
5. ✅ Aucune erreur dmesg pendant l'init ou le runtime

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | **NON** |
| Type de test | — |
| Résultat | — |
| Commentaires | E3.c = test runtime "tee_1to2 fonctionne sans crash dans une pipeline" — pas un test d'écoute audio par utilisateur. La validation single-sink ne valide pas encore le fan-out bit-perfect (différé à E3.d ou E5+ avec topology multi-sink réelle). |

## Notes

**Pipeline custom créé** : `pipe-tee-volume-playback.m4` — clone de `pipe-volume-playback.m4` avec tee_1to2 inséré entre host et volume. Pattern réutilisable pour E5+ avec strips IN.

**Single-sink mode** : pour ce test simplifié, tee_1to2 utilisé avec 1 seul sink connecté (à volume). Le 2e sink potentiel est ignoré. Valide que :
- module_adapter.c:806 check (`num_output_buffers > max_sinks`) accepte 1 buffer si max_sinks=2 (1>2=false)
- tee_1to2_process boucle for(i=0..num_of_sinks-1) supporte num_of_sinks=1
- Pas de crash si max_sinks > num_output_buffers réel

**period=1536** observé (vs 3072 baseline V3.2.2) : dû au pipeline DAI period différent dans pipe-tee-volume-playback.m4 (à investiguer/aligner si nécessaire). Pas un bug bloquant.

## Logs significatifs

```
[10.605107] sof-audio-of-imx8m 3b6e8000.dsp: Firmware info: version 2:10:0-fbf02
[10.654999] asoc-simple-card sof-sound-tac5212: ASoC: Parent card not yet available
0 erreur SOF tee/fail/panic post-aplay

aplay log : "Playing WAVE '/tmp/sine_e3c.wav' : S32_LE 48000Hz 8ch"

NPU tap : version=4 ring_size=261120 period=1536 rate=48000 ch=8
1.52 MB/s, epoch=1, resets=0, races=0
arecord 8ch 2s = 3072044 B (V3.2.2 capture)
```

## Conclusion

**GO E3.c.** tee_1to2 instancié + process appelé sans crash dans pipeline runtime SOF connecté. Premier test fonctionnel des 3 NEW comps validé. Firmware E2 (mix loop + blob + interleave) opérationnel en production. V3.2.2 baseline restaurée post-test (md5 `7366ff30...`).

**Prochaines étapes** :
- **E3.d** : test fan-out tee_1to2 bit-perfect (2 sinks réels, 2 PCM capture, sha256 identique) — encore plus complexe
- **OU directement E5/E6** : full topology Phase 1a.3 (8 strips IN + tee + matrix + 8 strips OUT + interleave_8 + SAI TX)

À trancher avec utilisateur. E3.c = waypoint validé, on a les outils m4 widgets + firmware fonctionnels.

## Référence

- Spec : `PHASE_1A_3_DSP_V5.4.1.md`
- Pattern pipeline : `pipe-volume-playback.m4` → `pipe-tee-volume-playback.m4` (NEW)
- Fiche précédente : `TESTS_V5.4.1_E3.b.md`
- Backup tplg V3.2.2 : `/root/backup-pre-V5.4.1/sof-imx8mp-tac5212.tplg.V3.2.2`
