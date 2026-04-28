# Test Fiche : V5.4.1 — E0.5 SDRAM2 carve-out + cacheattr + COMP_IIR

**Date** : 2026-04-28
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 (mixer16 + strips) |
| Version de référence | V5.4.1 |
| Étape | E0.5 — pré-requis SDRAM2 8 MB DSP-only @ 0xA0000000 |
| Commit SOF | `c829807b5` (branch `feature/audio-platform-v2`) |
| Commit yocto-nxp-debix | `f5d94dba` (branch `feature/audio-platform-v2`) |
| Topologie de référence | `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212-drc.m4` (V3.2.2 inchangée) |
| Firmware sof-imx8m.ri md5 board | `3cd20642d721088c176d07bcd73295d8` |
| Topology .tplg md5 board | `7366ff3061ee9c6eed57218cd0c0dcbc` (V3.2.2 inchangée) |
| Kernel Image / DTB md5 board | regénéré bitbake post-E0.5.b apply-sdram2-dt.py |

## Modifications appliquées

| Fichier | Changement |
|---|---|
| `sof/src/platform/imx8m/include/platform/lib/memory.h` | +SDRAM2_BASE 0xA0000000, +SDRAM2_SIZE 0x800000, PLATFORM_HEAP_BUFFER 3→4 |
| `sof/src/platform/imx8m/imx8m.x.in` | MEMORY +sof_sdram2 ; cacheattr `0x22212222 → 0x22112222` (region 5 = WT) |
| `sof/src/platform/imx8m/lib/memory.c` | +sdram2_heap_map + buffer[3] caps RAM\|CACHE\|DMA |
| `sof/app/boards/imx8mp_evk_mimx8ml8_adsp.conf` | +CONFIG_COMP_IIR=y |
| `meta-local/recipes-kernel/linux/files/apply-sdram2-dt.py` | NEW — DT carve sdram2_reserved@a0000000 + dsp memory-region |
| `meta-local/recipes-kernel/linux/linux-imx_%.bbappend` | +SRC_URI + appel apply-sdram2-dt.py |

## Tests réalisés

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| E0.5.0 | Lecture code SOF (H1-H5) | Read tool memory.c/h, imx8m.x.in, grep PLATFORM_HEAP_BUFFER | H1-H5 toutes confirmées | ✅ |
| E0.5.a | Pré-check `/proc/iomem` | `ssh root@board cat /proc/iomem \| grep -i a000` | 0xA0000000-0xA7FFFFFF libre | ✅ |
| E0.5.h | SDRAM2 reservation visible | `cat /proc/iomem` | `a0000000-a07fffff : reserved` | ✅ |
| E0.5.h | DT node carve OK | `ls /proc/device-tree/reserved-memory/sdram2_reserved@a0000000/` | reg = 0xA0000000 + 0x800000 (8 MB) | ✅ |
| E0.5.h | MemTotal -8 MB | `grep MemTotal /proc/meminfo` | 3679488 kB (vs 3687668 baseline = -8180 kB) | ✅ |
| E0.5.i | V3.2.2 régression audio | T1 baseline (arecord 8ch 2s) | 3072044 B + 0 xrun | ✅ |
| E0.5.j | V3.2.2 régression NPU tap | T6 baseline (npu_tap_reader stats 3s) | header valid, 1.50 MB/s, 0 resets | ✅ |
| E0.5.g | Firmware boot dmesg propre | `dmesg \| grep -i sof` | "Firmware info: version 2:10:0-c8298" | ✅ |
| E0.5.g | SDRAM1 utilisation stable | west build memory report | 99.66% inchangé | ✅ |

## Logs significatifs

```
[10.937762] sof-audio-of-imx8m 3b6e8000.dsp: Firmware info: version 2:10:0-c8298
=== /proc/iomem zone 0xA0000000 ===
a0000000-a07fffff : reserved
=== /proc/device-tree/reserved-memory/sdram2_reserved ===
reg = 00000000 000000a0 00000000 00008000   # 0xA0000000 + 0x800000 (8 MB)
=== MemTotal ===
MemTotal: 3679488 kB   # baseline 3687668 (-8180 kB ≈ -8 MB ✓)
```

## Conclusion

**GO E0.5.** SDRAM2 carve-out + cacheattr region 5 WT + buffer[3] heap + CONFIG_COMP_IIR appliqués sans régression V3.2.2 (audio + NPU tap PASS). Pré-requis SDRAM2 prêts pour E0.7 (C14 + C21).

## Référence

- Spec : `PHASE_1A_3_DSP_V5.4.1.md` (commit `6713215d`)
- Investigation critic : job `65d71d8c-9d36-4efe-a07a-c8fa5c301390` (V5.4 GO 6/6)
- Fiche précédente : `TESTS_V3.2.2_baseline.md`
