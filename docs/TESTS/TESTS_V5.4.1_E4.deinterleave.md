# Test Fiche : V5.4.1 — E4 deinterleave_8 NEW comp

**Date** : 2026-04-28
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E4.deinterleave — création comp deinterleave_8 (1×8ch → 8 mono) |
| Commit SOF | `710bb72f5` (branch `feature/audio-platform-v2`) |
| Commit yocto-nxp-debix | `83b636d6` (inchangé, pas de modif yocto) |
| Topologie de référence | `sof-imx8mp-tac5212-drc.m4` (V3.2.2 inchangée — comp deinterleave_8 pas encore instancié) |
| Firmware sof-imx8m.ri md5 board | `997978174217aa0b505e54f601afa5c0` |
| Topology .tplg md5 board | `7366ff3061ee9c6eed57218cd0c0dcbc` (V3.2.2 baseline) |

## Justification : pourquoi deinterleave_8

Architecture Phase 1a.3 figée (mémoire `project_v5_architecture.md`) : **8 strips IN mono parallèles** avec controls ALSA indépendants par voie (NON-NÉGOCIABLE). Or SAI7 RX produit 1 buffer 8ch interleaved, et SOF IPC3 ne fournit pas de comp natif pour split 8ch → 8 mono (mux/demux limités à 4 streams). Solution : créer comp custom `deinterleave_8`, pattern miroir d'`interleave_8` déjà implémenté.

## Composant deinterleave_8

| Champ | Valeur |
|---|---|
| Mode | SOURCE_SINK |
| max_sources | 1 |
| max_sinks | 8 |
| UUID | `f1a2b3c4-d5e6-4789-abcd-ef0123456789` |
| Process | `sink_data[ch][frame] = src_data[frame*8 + ch]` (memcpy stride inverse) |
| Path | `sof/src/audio/deinterleave_8/` |
| LOC | ~120 |

## Fichiers créés

| Fichier | Type |
|---|---|
| `sof/src/audio/deinterleave_8/deinterleave_8.c` | Implementation C |
| `sof/src/audio/deinterleave_8/Kconfig` | CONFIG_COMP_DEINTERLEAVE_8 |
| `sof/src/audio/deinterleave_8/CMakeLists.txt` | add_local_sources |
| `sof/src/audio/Kconfig` | rsource (patch) |
| `sof/src/audio/CMakeLists.txt` | add_subdirectory_ifdef (patch) |
| `sof/zephyr/CMakeLists.txt` | zephyr_library_sources_ifdef (patch) |
| `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` | +CONFIG (patch) |
| `sof/tools/topology/topology1/m4/deinterleave_8.m4` | Widget m4 |

## Tests réalisés

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| E4.d.0 | UUID unicité | `grep -rn deinterleave\|f1a2b3c4 sof/src/audio/` | aucune collision | ✅ |
| E4.d.1 | Build firmware | `west build` | sdram0 stable, static_uuid +48 B | ✅ |
| E4.d.2 | Sign + 4 UUIDs présents | `strings zephyr.ri \| grep -E deinterleave\|tee_1to2\|mixer16\|interleave_8` | 4 strings UUIDs | ✅ |
| E4.d.3 | Deploy + reboot | `scp + reboot` | board boot OK | ✅ |
| E4.d.4 | Firmware md5 board | `md5sum` | `99797817...` | ✅ |
| E4.d.4 | SOF errors | `dmesg \| grep -iE sof.*err\|panic` | 0 erreur | ✅ |
| E4.d.5 | NPU tap V3.2.2 régression | `npu_tap_reader --stats --time 3` | header valid, **period=3072 baseline**, 0 resets | ✅ |
| E4.d.5 | V3.2.2 audio régression | `arecord -D hw:$TAC,0 -c 8 -f S32_LE -d 2` | 3072044 B + 0 xrun | ✅ |
| E4.d.5 | NPU tap device + module | `ls /dev/imx-audio-tap + lsmod` | présent + chargé | ✅ |

## Validation comparative E3.c → E4

| Métrique | E3.c | E4 (deinterleave ajouté) | Delta |
|---|---|---|---|
| Firmware md5 | `5b9acfc3...` | `99797817...` | DIFFERENT (rebuild OK) |
| static_uuid_entries | 2160 B (3 UUIDs) | 2208 B (4 UUIDs) | +48 B = 1 UUID |
| sdram0 | 321024 B | 321024 B | INVARIANT |
| sdram1 | 99.66% | 99.66% | INVARIANT |
| NPU tap période | 1536 (E3.c topology test) → 3072 (post-restore) | 3072 (V3.2.2 baseline) | INVARIANT V3.2.2 |
| Throughput NPU tap | 1.50 MB/s | 1.54 MB/s | équivalent |

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | **NON** |
| Type de test | — |
| Résultat | — |
| Commentaires | E4.deinterleave = ajout comp custom, pas instancié dans topology V3.2.2 actuelle. Test fonctionnel runtime du deinterleave_8 différé à E5+ avec topology Phase 1a.3 (8 strips IN où deinterleave_8 split SAI7 RX 8ch en 8 mono). |

## Logs significatifs

```
[E4 build]
sdram0: 321024 B (3.83%)        # stable vs E3.c
static_uuid_entries: 2208 B     # +48 B vs E3.c (2160 B = 1 UUID supplémentaire)
static_log_entries: 89920 B     # +124 B (nouveaux comp_info strings)

[E4 firmware on board]
md5sum: 997978174217aa0b505e54f601afa5c0
strings: tee_1to2 + mixer16 + interleave_8 + deinterleave_8 (4 UUIDs)

[E4 NPU tap régression]
header valid version=4 ring_size=261120 period=3072 rate=48000 ch=8
1.54 MB/s, epoch=1, resets=0, races=0
arecord 8ch 2s = 3072044 B
```

## Conclusion

**GO E4 deinterleave_8.** Composant custom créé, compilé, UUID enregistré dans firmware, déployé sur board sans régression V3.2.2 (NPU tap period=3072 + audio + 0 erreur). Les 4 NEW comps (tee_1to2 + mixer16 + interleave_8 + deinterleave_8) sont prêts à être instanciés dans la topology Phase 1a.3 complète à E5.

## Prochaine étape : E5

Concevoir et implémenter la **topology m4 Phase 1a.3 progressive** :
```
SAI7 RX 8ch → deinterleave_8 → 8 strips IN mono (eq_iir + drc + 2× volume L/R) → 8 tee_1to2
                                                                                    │
                          ┌─────────────────────────────────────────────────────────┘
                          │
                          ▼
        8 buf_post_in_cap → interleave_8_cap → 1 PCM capture host 8ch (ASIO IN)
                          │
                          ▼
          mixer16 in[0..7] (mics)
          mixer16 in[8..15] ◄── 8 PCM playback hosts mono (ASIO OUT)
                          │
                          ▼ 8 mono sinks
          8 strips OUT (multiband_drc + pga + drc) → interleave_8 → SAI7 TX 8ch
                                                                    │
                                                                    ▼
                                                              NPU tap V3.2.2
                                                                    ▼
                                                              4× TAC5212
```

Effort estimé : ~500-800 lignes de m4 (pipelines + widgets + buffers + P_GRAPH).

## Référence

- Spec : `PHASE_1A_3_DSP_V5.4.1.md`
- Pattern : `interleave_8.c` (E2 implementation, miroir)
- Architecture figée : mémoire `project_v5_architecture.md` (règle 8 = effets indépendants NON-NÉGOCIABLE)
- Fiche précédente : `TESTS_V5.4.1_E3.c.md`
