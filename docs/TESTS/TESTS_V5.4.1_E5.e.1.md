# Test Fiche : V5.4.1 — E5.e.1

**Date** : 2026-05-01
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E5.e.1 — 1 strip IN ch1 (eq_iir + drc + pga_L + pga_R) + bypass ch2..8 |
| Commit SOF | `59e3d3c1e` (branch `feature/audio-platform-v2`, github fork Mjxkill/sof) |
| Commit yocto-nxp-debix | `81559179c` (branch `feature/audio-platform-v2`) |
| Topologie de référence | `sof/tools/topology/topology1/sof-imx8mp-tac5212-V5.4.1-E5e1.m4` |
| Pipe inclus | `sof/tools/topology/topology1/sof/pipe-deint-strip1-interleave-capture.m4` |
| Firmware sof-imx8m.ri md5 board | `358080e8669c6bc82f5eb77d4cc18a8d` |
| Topology .tplg md5 board | `7ac58ad8eb78ccdc02c6ff3a6637ac5f` (E5.e.1) |
| Kernel Image md5 board | inchangé depuis V3.2.2 baseline |
| DTB md5 board | inchangé depuis V3.2.2 baseline |

## Architecture validée

```
SAI7 RX 8ch ──► B0 ──► deinterleave_8 ──► B1 ──► EQ_IIR ──► B10 ──► DRC ──► B11
                                                                                │
                                                                                ▼
                                              ┌── pga_L ◄── B12 ◄── pga_R ◄── (B11)
                                              │ (FL ch)              (FR ch)
                                              ▼
                                              B13 ─────────────────────┐
                       deinterleave ──► B2..B8 (bypass) ──────────────►│
                                                                       ▼
                                                                interleave_8 ──► B9 ──► host PCM 0 (ASIO IN 8ch)

host PCM 1 ──► volume ──► SAI7 TX 8ch (ASIO OUT 8ch identique V3.2.2)
```

Mono buffers protégés via auto-detection `pipeline_complete` walk #2 (Option F++) :
- B1..B8 : sinks de deinterleave_8 → channels=1 + preserve_channels=true
- B10..B13 : intra-strip (eq_iir→drc→pga_L→pga_R→interleave_8) → propagation linéaire amont
- B0 (8ch DAI side) et B9 (8ch host side) NON lockés — restent à 8ch

## Devices ALSA + audio

| Device | Card | Rôle | Format |
|---|---|---|---|
| `hw:softac5212tdm,0` cap | 2 | ASIO IN 8ch (PCM 0 capture) | S32_LE 48 kHz 8 ch |
| `hw:softac5212tdm,1` play | 2 | ASIO OUT 8ch (PCM 1 playback) | S32_LE 48 kHz 8 ch |

DMA scheduling **2 ms NON-NÉGOCIABLE** (period=2000us, SCHEDULE_TIME_DOMAIN_DMA).

## Tests réalisés (Claude — automatisés sur board)

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| 1 | Boot firmware F++ | `dmesg \| grep sof` | "DT DSP detected", "Firmware: ABI 3:29:0", pas d'`invalid FW header` | OK |
| 2 | Topology load | `dmesg \| grep tplg` | "config SAI7 fmt 0x4004 mclk 12288000 width 32 slots 8" | OK |
| 3 | TAC reset | `/usr/bin/tac-reset` | "All TACs reset done (analog)" | OK |
| 4 | arecord 8ch S32_LE 3s | `arecord -D hw:softac5212tdm,0 -c 8 -r 48000 -f S32_LE /tmp/test_E5e1.wav` | fichier ~4.2 MB, no -ENOMEM, no -EINVAL | **4 227 116 octets** ✓ |
| 5 | Régression E5.d (passthrough) sur firmware F++ | redéploy E5.d.tplg, arecord 3s | fichier ~4.4 MB, no error | **4 358 188 octets** ✓ |
| 6 | dmesg post-arecord | `dmesg \| grep -iE 'enomem\|tx error\|hw params'` | aucune erreur | aucune erreur ✓ |

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | OUI |
| Type de test | loopback-c temps réel (capture 8ch + playback 8ch simultanés via ring buffer 384 frames) |
| Résultat | « très bon » — chaîne neutre identité audible, full duplex stable |
| Commentaires | eq_iir blob = pass identité, drc default neutre, pga_L/R = 0 dB → strip ch1 doit être bit-perfect au passage. Ch2..8 bypass identique à E5.d. NPU tap V3.2.2 préservé (dai_dma_cb hook inchangé). |

## Logs significatifs

Boot firmware F++ E5.e.1 (extrait dmesg) :
```
[   10.467550] sof-audio-of-imx8m 3b6e8000.dsp: DT DSP detected
[   10.505481] sof-audio-of-imx8m 3b6e8000.dsp: unknown sof_ext_man header type 3 size 0x30
[   10.505531] sof-audio-of-imx8m 3b6e8000.dsp: Firmware info: version 2:10:0-469cf
[   10.505538] sof-audio-of-imx8m 3b6e8000.dsp: Firmware: ABI 3:29:0 Kernel ABI 3:23:0
[   10.576000] sof-audio-of-imx8m 3b6e8000.dsp: tplg: config SAI7 fmt 0x4004 mclk 12288000 width 32 slots 8 mclk id 0
[   10.645093] tac5212 3-0050: TAC5212 initialized (I2C 0x50, slots 0-1)
```

arecord OK :
```
Recording WAVE '/tmp/test_E5e1.wav' : Signed 32 bit Little Endian, Rate 48000 Hz, Channels 8
Aborted by signal Terminated...
-rw-r--r-- 1 root root 4227116 May  1 15:48 /tmp/test_E5e1.wav
```

## Conclusion

V5.4.1 E5.e.1 **GO**. La nouvelle approche Option F++ (auto-detection du chemin branché dans `pipeline_complete` + skip-channels pour buffers lockés dans `pipeline_update_buffer_pcm_params` + save/restore dans `buffer_set_params`) résout définitivement le problème de propagation des channels par `pipeline_comp_params_neg` et `comp_verify_params` sur les pipelines branched intra-pipeline.

Cette solution scale automatiquement aux étapes suivantes (E5.e.2 = 8 strips identiques) sans modification firmware additionnelle attendue : il suffit de répliquer le pattern strip ch1 dans le m4. Les comps multi-sink/multi-source (`deinterleave_8` / `interleave_8`) sont détectés automatiquement par la 2ᵉ walk de `pipeline_complete`.

## Référence (autres docs/spec liées)

- Investigation critic : job `0e5ce10a-0eda-4b6f-9952-c741e0f211de` (Option A KO) + `e4c4c526-6710-46e7-b368-7918130c6390` (5 workers, recommandation Option F++ par minimax/claude-code)
- critic_decision : `25dac03a-0fda-425f-b0e4-dda9a690ce6d`
- Fiches précédentes : `TESTS_V5.4.1_E4.deinterleave.md`, `TESTS_V5.4.1_E5.a.md`, `TESTS_V5.4.1_E5.b.1.md`, `TESTS_V5.4.1_E5.c.1.md`, `TESTS_V5.4.1_E5.d.md`
- Mémoire : `sof_e5e1_optionFpp_validated.md`
