# Test Fiche : V5.4.1 — E1 skeleton 3 NEW comps

**Date** : 2026-04-28
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E1 — skeleton tee_1to2 + mixer16 + interleave_8 |
| Commit SOF | `fbf0269b6` (branch `feature/audio-platform-v2`) |
| Commit yocto-nxp-debix | `f5d94dba` (inchangé) |
| Topologie de référence | `sof-imx8mp-tac5212-drc.m4` (V3.2.2 inchangée — comps NEW pas instanciés en E1) |
| Firmware sof-imx8m.ri md5 board | `161923e9ca03042a82920d6fc660a460` |
| Topology .tplg md5 board | `7366ff3061ee9c6eed57218cd0c0dcbc` (inchangée) |

## Composants NEW créés

| Comp | Path | Mode | UUID | Stub LOC |
|---|---|---|---|---|
| tee_1to2 | `sof/src/audio/tee_1to2/` | STREAM (1×2) | `e1ec7700-5e44-4abc-b1f9-3c14e2d2afe1` | 80 |
| mixer16 | `sof/src/audio/mixer16/` | SOURCE_SINK (16×8) | `d2e64a00-e3b6-4dca-bd83-09e8aae6e1f7` | 90 |
| interleave_8 | `sof/src/audio/interleave_8/` | SOURCE_SINK (8×1) | `c8a3b500-5b95-4cb1-a91c-5a72fa4e9c5e` | 80 |

## Patches CMakeLists/Kconfig

| Fichier | Changement |
|---|---|
| `sof/src/audio/Kconfig` | rsource des 3 nouveaux Kconfig |
| `sof/src/audio/CMakeLists.txt` | add_subdirectory conditionnel × 3 (chemin native SOF) |
| `sof/zephyr/CMakeLists.txt` | zephyr_library_sources_ifdef × 3 (chemin Zephyr SOF firmware — actif pour imx8mp_evk) |
| `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` | +CONFIG_COMP_TEE_1TO2/MIXER16/INTERLEAVE_8=y |

## Tests réalisés

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| E1.0 | Lecture pattern volume.c (UUID + module_interface + DECLARE_MODULE_ADAPTER) | Read tool | pattern documenté | ✅ |
| E1.f | Build firmware avec 3 NEW Kconfig=y | `west build` | sdram0 +4 KB, static_uuid_entries +144 B | ✅ |
| E1.f1 | UUIDs enregistrés dans firmware | `strings zephyr.ri \| grep "tee_1to2\|mixer16\|interleave_8"` | 3 UUIDs présents | ✅ |
| E1.g | Firmware md5 sur board | `md5sum /lib/firmware/imx/sof/sof-imx8m.ri` | `161923e9...` | ✅ |
| E1.g | SOF errors | `dmesg \| grep -iE "sof.*err"` | 0 erreur | ✅ |
| E1.g | NPU tap V3.2.2 invariance | `npu_tap_reader --stats --time 3` | 261120/3072/48000/8, 1.50 MB/s, 0 resets | ✅ |
| E1.g | NPU tap dump 2s | `npu_tap_reader --dump --time 2` | 2611244 B (md5 `6b0437e9...`) | ✅ |
| E1.g | V4.2 arecord 8ch 2s | arecord | 3072044 B + 0 xrun | ✅ |
| E1.g | NPU tap device + module | `ls /dev/imx-audio-tap` + `lsmod` | présent + chargé | ✅ |

## Validation comparative E0.7 vs E1

| Métrique | E0.7 baseline | E1 après 3 stubs | Delta |
|---|---|---|---|
| Firmware md5 | `6d703233...` | `161923e9...` | DIFFERENT (rebuild OK) |
| sdram0 (code) | 316928 B | 321024 B | +4 KB (3 stubs) |
| static_uuid_entries | 2016 B | 2160 B | +144 B (3×48 B = 3 UUIDs) |
| sdram1 (heap) | 99.66% | 99.66% | INVARIANT |
| NPU tap V3.2.2 | invariant | invariant | INVARIANT ✅ |

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | **NON** |
| Type de test | — |
| Résultat | — |
| Commentaires | E1 = skeletons compilables uniquement, comps non instanciés. Pas d'effet sur audio runtime → test utilisateur peu informatif à ce stade. |

## Notes importantes

**Découverte E1** : SOF a deux chemins de build pour les comps audio :
- `sof/src/audio/CMakeLists.txt` (chemin natif SOF non utilisé sur imx8mp_evk)
- `sof/zephyr/CMakeLists.txt` (chemin Zephyr SOF firmware — **actif** pour imx8mp_evk)

Le patch initial dans `sof/src/audio/CMakeLists.txt` était insuffisant — il a fallu ajouter `zephyr_library_sources_ifdef` dans `sof/zephyr/CMakeLists.txt` pour que les .c soient compilés.

## Logs significatifs

```
[E1 build report]
sdram0:      321024 B         8 MB      3.83%   (+4 KB vs E0.7)
sdram1:       8144 KB      8172 KB     99.66%   (inchangé)
static_uuid_entries_seg: 2160 B  (vs 2016 B E0.7 = +144 B = 3 UUIDs)

[E1 firmware on board]
md5sum: 161923e9ca03042a82920d6fc660a460
strings: tee_1to2, mixer16, ^interleave_8 (présents)

[NPU tap régression]
header valid — version=4 ring_size=261120 period=3072 rate=48000 ch=8
1.50 MB/s, 0 resets, 0 races
```

## Conclusion

**GO E1.** Les 3 NEW comps SOF (skeletons) compilent + leur UUIDs sont enregistrés dans le firmware. Aucune régression V3.2.2 (audio + NPU tap PASS). Comps non encore instanciés par la topology V3.2.2 (drc.m4) → no-op safe en runtime. Prêt pour E2 (mix loop + bytes blob + interleave process).

## Référence

- Spec : `PHASE_1A_3_DSP_V5.4.1.md`
- Pattern m4 widget custom : `sof/tools/topology/topology1/m4/multiband_drc.m4` (référence pour E3)
- Fiche précédente : `TESTS_V5.4.1_E0.7.md`
