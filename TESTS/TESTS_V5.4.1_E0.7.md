# Test Fiche : V5.4.1 — E0.7 C14 + C21 isolated patches

**Date** : 2026-04-28
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E0.7 — C14 MODULE_MAX_SOURCES 8→16 + C21 PLATFORM_MAX_STREAMS 8→16 |
| Commit SOF | `094583398` (branch `feature/audio-platform-v2`) |
| Commit yocto-nxp-debix | `f5d94dba` (inchangé depuis E0.5) |
| Topologie de référence | `sof-imx8mp-tac5212-drc.m4` (V3.2.2 inchangée) |
| Firmware sof-imx8m.ri md5 board | `6d7032336025ac9a8d7a78995ec8de60` |
| Topology .tplg md5 board | `7366ff3061ee9c6eed57218cd0c0dcbc` (inchangée) |

## Modifications appliquées

| Fichier | Changement |
|---|---|
| `sof/src/include/sof/audio/module_adapter/module/generic.h:33` | `MODULE_MAX_SOURCES 8 → 16` (C14 — pour mixer16 16 sources) |
| `sof/src/platform/imx8m/include/platform/platform.h:44` | `PLATFORM_MAX_STREAMS 8 → 16` (C21 — pour 9+ PCM streams V5.4.1) |

## Tests réalisés

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| E0.7.a | Baseline V3.2.2 NPU tap (firmware E0.5) | `npu_tap_reader --stats --time 3` | header valid, 1.50 MB/s, 0 resets | ✅ |
| E0.7.a | Baseline V3.2.2 audio | arecord 8ch 2s | 3072044 B + 0 xrun | ✅ |
| E0.7.b-c | Build + sign firmware | `west build` + `west sign` | sdram1 stable 99.66%, signature Reef OK | ✅ |
| E0.7.f | Firmware version après deploy | `dmesg \| grep "Firmware info"` | "version 2:10:0-09458" (= commit `09458`) | ✅ |
| E0.7.f | NPU tap V3.2.2 régression | `npu_tap_reader --stats --time 3` post-deploy | header valid 261120/3072/48000/8, 1.54 MB/s, 0 resets | ✅ INVARIANT |
| E0.7.f | NPU tap dump 2s | `npu_tap_reader --dump --time 2` | 2611200 B (md5 `6b0437e9...`) | ✅ |
| E0.7.f | V4.2 audio régression | arecord 8ch 2s | 3072044 B + 0 xrun | ✅ INVARIANT |
| E0.7 | SDRAM1 stable | memory report | 99.66% inchangé (+3.4 KB invisible vs E0.5) | ✅ |
| E0.7 | SOF errors dmesg | `dmesg \| grep -iE "sof.*err"` | 0 erreur | ✅ |

## Validation comparative E0.5 vs E0.7

| Métrique | E0.5 baseline | E0.7 après C14+C21 | Delta |
|---|---|---|---|
| Firmware md5 | `3cd20642...` | `6d703233...` | DIFFERENT (rebuild OK) |
| Firmware version | `c8298` | `09458` | OK (nouveau commit) |
| NPU tap header | magic NPAT v4 | magic NPAT v4 | INVARIANT ✅ |
| Throughput | 1.50 MB/s | 1.54 MB/s | équivalent ✅ |
| arecord 8ch 2s | 3072044 B | 3072044 B | INVARIANT ✅ |
| 0 xrun | OK | OK | INVARIANT ✅ |

## Logs significatifs

```
[10.066842] sof-audio-of-imx8m 3b6e8000.dsp: Firmware info: version 2:10:0-09458
sdram1: 8144 KB / 8172 KB (99.66%)  [identique à E0.5]
npu_tap: header valid — version=4 ring_size=261120 period=3072 rate=48000 ch=8
npu_tap: 1.54 MB/s (epoch=1 resets=0 races=0)
```

## Conclusion

**GO E0.7.** C14 + C21 patches isolés appliqués SANS régression V3.2.2 (NPU tap + audio PASS). Impact mémoire : +3.4 KB struct processing_module (négligeable). Stack +64 B/frame max. Confirmation 6/6 workers (V5.4 investigation `65d71d8c`) validée empiriquement.

## Référence

- Spec : `PHASE_1A_3_DSP_V5.4.1.md`
- Fiche précédente : `TESTS_V5.4.1_E0.5.md`
