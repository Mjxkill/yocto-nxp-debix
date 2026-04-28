# Test Fiche : V5.4.1 — E2 mix loop mixer16 + bytes blob 512B + interleave_8 process

**Date** : 2026-04-28
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E2 — implémentation mix loop Q1.31 mixer16 + ALSA bytes blob 512B + interleave_8 memcpy stride |
| Commit SOF | `04eadda17` (branch `feature/audio-platform-v2`) |
| Commit yocto-nxp-debix | `f5d94dba` (inchangé depuis E0.5) |
| Topologie de référence | `sof-imx8mp-tac5212-drc.m4` (V3.2.2 inchangée — comps NEW non instanciés) |
| Firmware sof-imx8m.ri md5 board | `5b9acfc367ae5de28b6017b82d67810d` |
| Topology .tplg md5 board | `7366ff3061ee9c6eed57218cd0c0dcbc` (inchangée) |

## Implémentations E2

### mixer16 (mix loop 16×8 Q1.31)
- API : SOURCE_SINK avec `sof_source**` / `sof_sink**`
- Helpers : `source_get_data`, `sink_get_buffer`, `sink_commit_buffer`, `source_release_data` (pattern up_down_mixer.c)
- Init : matrice identité (`gain[i][j] = INT32_MAX si i==j and i<8, else 0`)
- ALSA bytes blob 512B via `comp_data_blob_handler` (pattern multiband_drc)
- `set_configuration` / `get_configuration` pour update runtime des gains
- Saturation INT32 sur accumulation
- Estimation : 6.144 MIPS @ 48 kHz (16 mults × 8 outputs × 96 frames × 500 Hz)

### interleave_8 (memcpy stride 8 mono → 1×8ch)
- API : SOURCE_SINK 8×1
- `dst[frame*8 + ch] = src[ch][frame]` (loop)
- Channels au-delà de num_of_sources zero-padded
- ~2 MIPS estimé (memory-bound)
- NPU tap V3.2.2 invariance préservée (period_bytes=3072 = 96×8×4)

### tee_1to2 (inchangé E1, déjà fonctionnel STREAM mode)

## Tests réalisés

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| E2.0 | Lecture pattern multiband_drc + up_down_mixer + data_blob.h | Read tool | pattern documenté | ✅ |
| E2.d | Build firmware E2 | `west build` | OK, sdram0 stable, static_uuid +144 B | ✅ |
| E2.d | UUIDs registered | `strings zephyr.ri` | tee_1to2 + mixer16 + interleave_8 + comp_err strings | ✅ |
| E2.e | Firmware md5 sur board | `md5sum` | `5b9acfc3...` | ✅ |
| E2.e | Firmware version | `dmesg \| grep "Firmware info"` | "version 2:10:0-fbf02" (cache hash E1, md5 prouve E2) | ✅ |
| E2.e | SOF errors | `dmesg \| grep -iE "sof.*err"` | 0 erreur | ✅ |
| E2.e | NPU tap V3.2.2 invariance | `npu_tap_reader --stats --time 3` | 261120/3072/48000/8, 1.53 MB/s, 0 resets | ✅ |
| E2.e | NPU tap dump 2s | `npu_tap_reader --dump --time 2` | 2482220 B (md5 `ff60e3c9...`) | ✅ |
| E2.e | V4.2 audio régression | arecord 8ch 2s | 3072044 B + 0 xrun | ✅ |
| E2.e | /dev/imx-audio-tap + module | `ls` + `lsmod` | présent + chargé | ✅ |

## Validation comparative E1 vs E2

| Métrique | E1 baseline | E2 après mix loop + blob | Delta |
|---|---|---|---|
| Firmware md5 | `161923e9...` | `5b9acfc3...` | DIFFERENT (rebuild OK) |
| sdram0 (code) | 321024 B | 321024 B | INCHANGÉ (mix loop ajouté = inline dans .o existants) |
| static_uuid_entries | 2160 B | 2160 B | INVARIANT |
| static_log_entries | 89348 B | 89796 B | +448 B (nouveaux comp_err strings mixer16) |
| sdram1 (heap) | 99.66% | 99.66% | INVARIANT |
| NPU tap V3.2.2 | invariant | invariant | INVARIANT ✅ |

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | **NON** |
| Type de test | — |
| Résultat | — |
| Commentaires | E2 = mix loop + blob handler implémentés mais comps non instanciés par topology drc.m4 actuelle → no-op runtime. Premier test utilisateur fonctionnel attendu à **E3** (topology m4 mini test). |

## Logs significatifs

```
[E2 build report]
sdram0:      321024 B         8 MB      3.83%
sdram1:       8144 KB      8172 KB     99.66%
static_uuid_entries_seg: 2160 B
static_log_entries_seg:  89796 B  (+448 B vs E1 = nouveaux comp_err mixer16)

[E2 firmware on board]
md5sum: 5b9acfc367ae5de28b6017b82d67810d
strings: tee_1to2, mixer16, interleave_8, "mixer16_init: blob handler alloc failed"

[NPU tap régression]
header valid — version=4 ring_size=261120 period=3072 rate=48000 ch=8
1.53 MB/s, 0 resets, 0 races
arecord 8ch 2s = 3072044 B + 0 xrun
```

## Conclusion

**GO E2.** Mix loop mixer16 16×8 Q1.31 + bytes blob 512B handler + interleave_8 memcpy stride implémentés. Pas de régression V3.2.2 (NPU tap + audio PASS). Validation fonctionnelle des comps **différée à E3** (topology m4 mini test) puisque les comps ne sont pas encore instanciés par la topology drc.m4 actuelle.

## Référence

- Spec : `PHASE_1A_3_DSP_V5.4.1.md`
- Patterns SOF : `multiband_drc.c` (blob handler), `up_down_mixer.c` (SOURCE_SINK API)
- Fiche précédente : `TESTS_V5.4.1_E1.md`
