# Test Fiche : V3.2.2 — baseline NPU tap + topology drc

**Date** : 2026-04-26 (rétro-fill 2026-04-28)
**Statut** : GO — référence baseline (V3.2.2 production figée sur board)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.2 (NPU tap) |
| Version de référence | V3.2.2 |
| Étape | Baseline (déjà déployée) |
| Commit SOF | `61fcb7d...` (branch `feature/sdma-ap2ap-phase1` SOF Mjxkill, hook NPU tap) |
| Commit SOF V3.2.2.1 défensif | `9f6f70a21` (guard period_bytes==0 dans NPU tap init) |
| Commit yocto-nxp-debix | `<pre-V5.4.x>` (avant E0.5) |
| Topologie de référence | `meta-local/recipes-kernel/linux/files/sof-imx8mp-tac5212-drc.m4` |
| Firmware sof-imx8m.ri md5 board pré-E0.5 | `4061adcdda6768421c7c6abd4d42635c` |
| Topology .tplg md5 board | `7366ff3061ee9c6eed57218cd0c0dcbc` (sof-imx8mp-tac5212.tplg) |

## Devices ALSA + audio

| Device | Card | Rôle | Format |
|---|---|---|---|
| `hw:0,0` | audiohdmi | HDMI playback | i.MX HDMI i2s-hifi-0 |
| `hw:1,0` | es8316audio | ES8316 codec | HiFi ES8316 |
| `hw:2,0` | softac5212tdm | SAI_Capture | 8ch S32_LE 48 kHz TDM |
| `hw:2,1` | softac5212tdm | SAI_Playback | 8ch S32_LE 48 kHz TDM |
| `/dev/imx-audio-tap` | char dev 10,122 | NPU tap ring buffer | mmap-only, magic NPAT |

## Tests réalisés

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| T1 | Capture 8ch | `arecord -D hw:2,0 -c 8 -f S32_LE -r 48000 -d 5 t1.wav` | 3072044 B/sec, samples non-NULL | ✅ OK |
| T2 | Playback 8ch siren | `aplay -D hw:2,1 siren.wav` (5s sine 300→800 Hz) | audible 8 sorties TAC | ✅ OK |
| T3 | Duplex playback + capture | T1 + T2 simultanés | 0 xrun, audio cohérent | ✅ OK |
| T4 | Latence loopback 5 ms | `loopback-lowlat.sh` (alsaloop -E 64 -B 256 -t 5000) | latence < 5 ms 8ch full duplex | ✅ OK |
| T5 | NPU tap header | open `/dev/imx-audio-tap` + mmap + read magic | magic = `0x5441504E` (NPAT), version=4, ring_size=261120 | ✅ OK |
| T6 | NPU tap dump pendant aplay | `npu_tap_reader --stats --time 3` | 1.5 MB/s, 0 epoch_resets, 0 race_retries | ✅ OK |
| T7 | NPU tap dump WAV | `npu_tap_reader --dump out.wav --time 2` | 8ch S32_LE 48 kHz, samples post-effets | ✅ OK |

## Caractéristiques topology V3.2.2 (sof-imx8mp-tac5212-drc.m4)

```
PCM 0 (capture)  : SAI7 RX 8ch ──► multiband_drc + pga + drc ──► host
PCM 1 (playback) : host ──► multiband_drc + pga + drc ──► SAI7 TX 8ch
```

- **Pipelines** : 2 en `SCHEDULE_TIME_DOMAIN_TIMER`
- **DAI** : SAI7 TDM 8 slots × 32 bits, mclk/bclk 12.288 MHz codec_consumer, fsync 48 kHz, **ASYNC**
- **Period nominal** : 1 ms
- **Buffer périodique** : 2 periods côté DAI

## Devices ALSA exposés

- 1× capture 8ch (hw:2,0)
- 1× playback 8ch (hw:2,1)
- 1× /dev/imx-audio-tap (NPU)

## Logs significatifs

```
[10.937762] sof-audio-of-imx8m 3b6e8000.dsp: Firmware info: version 2:10:0-61fcb
imx_audio_tap          12288  0
crw------- 1 root root 10, 122 /dev/imx-audio-tap
npu_tap: header valid — version=4 ring_size=261120 period=3072 rate=48000 ch=8
npu_tap: 1.50 MB/s (epoch=1 resets=0 races=0)
```

## Conclusion

**V3.2.2 = référence baseline V5.4.x.** Tous les tests T1-T7 PASS sur board. Cette config (firmware NPU tap V3.2.2 + topology drc.m4 + 2 pipelines TIMER + duplex 5 ms) est la **référence absolue** : toute évolution V5.4+ doit préserver T1-T7 sans régression.

V4.2 = étiquette historique du master chain (multiband_drc + pga + drc) embarqué dans cette topology drc.m4 — à ne plus utiliser comme version distincte. Référence officielle = V3.2.2.

## Référence

- Spec : `PHASE_1A_2_NPU_TAP_V3.2.2.md`
- Mémoire : `sof_v322_baseline.md`
- NPU tap reader : `/root/tests/npu_tap_reader.c` + binary
- Loopback test : `/root/tests/loopback-lowlat.sh`
- SOF test script : `/root/tests/sof-test.sh`
