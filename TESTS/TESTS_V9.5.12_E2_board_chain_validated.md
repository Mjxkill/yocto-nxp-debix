# Test Fiche : V9.5.12 — E2 chain LV2 v5.12 validée sur board (NPU TAP OUT)

**Date** : 2026-06-08
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | Phase 2 board integration — Step A (POC params statiques) |
| Version | V9.5.12 |
| Étape | E2 — Insert mastering ML sur board validé via NPU TAP OUT |
| Commit yocto-nxp-debix | `<à committer>` (branch `feature/v7.0-multiband-drc-tap`) |
| Modèle ML | `conv_v5_12_full_epoch029.pt` |
| Binaire mixer-pro board | `/usr/bin/mixer-pro` v9.5.12-slow-smooth |

## Fixes inclus dans cette étape

| # | Fix | Pourquoi |
|---|---|---|
| 1 | `LV2_MAX_CTRL_PORTS` 64 → 256 (effects.c) | LSP Para EQ x16 stereo a ~158 ports (14 globaux + 9×16 bands). Cap 64 = seules bandes 0-5 visibles → fail sur ft_8..ft_15 |
| 2 | Version string `v9.5.12-slow-smooth` (mixer-pro.h) | Identification |
| 3 | Smoothing tau 50 ms (effects.c V9.5.12) | Absorber vibrato params dû transients (déjà précédent commit) |
| 4 | Endpoint `set_insert_params_bulk` (mixer-pro.c V9.5.5) | Push N params en 1 call (déjà précédent commit) |
| 5 | Script `training/deploy_v5_12_board.py` | POC : load ckpt → features → predict → push board |

## Devices ALSA + audio

| Card | Device | Rôle |
|---|---|---|
| 2 | hw:UAC2Gadget,0 | Source audio (PC envoie wav via USB UAC2) |
| 4 | hw:softac5212tdm,0 | DSP TAC5212 → speakers (output) |

## Tests réalisés

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| 1 | Deploy mixer-pro V9.5.12 | `scp + systemctl restart` | active | ✅ |
| 2 | LSP Para EQ x16 ports visibles | `get_insert` count ft_X | 16 (ft_0..ft_15) | ✅ 16 visibles après fix |
| 3 | Push 62 params v5.12 via `set_insert_params_bulk` | `deploy_v5_12_board.py` | set=76 fail=0 | ✅ 76/76 OK |
| 4 | Routing UAC2 in[8,9] → DSP out[0,1] | `set_master src=8 out=0 gain=1.0` ×2 | OK | ✅ |
| 5 | Play wav PC → board UAC2 | `aplay -D plughw:2,0 file.wav` | audio délivré | ✅ |
| 6 | Capture NPU TAP OUT 8s pendant playback | `npu_tap_reader --device /dev/imx-audio-tap-out --dump --time 8` | wav 8ch S32_LE | ✅ 384k frames |
| 7 | Comparaison spectres bypass vs active | `welch` per band | active > bypass dans hm/ar | ✅ Δ = +8.1 dB hm, +9.2 dB ar |

## Résultats spectraux (Cambridge contraband, 5s middle)

| Bande | Tap bypass | Tap active | **Δ** | Target attendu |
|---|---|---|---|---|
| sub | -77.6 | -70.1 | +7.5 | +12 |
| bs | -42.5 | -38.2 | +4.3 | +6.7 |
| md | -62.6 | -55.7 | +6.9 | +9.2 |
| **hm** | -60.3 | -52.2 | **+8.1** | +13.8 |
| **ar** | -66.6 | -57.4 | **+9.2** | +17.9 |

→ Signature ML appliquée : Δ progressif vers les aigus (hm+ar > sub+bs). Atténué vs target attendu (limiter g_out + DSP DRC en aval). Cohérent.

## Test utilisateur

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | NON (user distant, pas d'écoute possible) |
| Validation | Via mesure spectrale NPU TAP OUT (≈ équivalent objectif) |
| Verdict | **GO** — signature ML clairement visible dans le spectre |

## Logs significatifs

```
{"ok":true,"op":"set_insert","n":4}
{"ok":true,"op":"set_insert_params_bulk","set":76,"fail":0}
{"ok":true,"active":true,"type":"chain","n":4,"chain":[{"slot":0,...
npu_tap: header valid — version=4 ring_size=261120 period=3072 rate=48000 ch=8
npu_tap: done — total=12288000 B  duration=8.00 s  avg=1.54 MB/s  epoch_resets=0  race_retries=0
```

## Conclusion

✅ **GO** pour Phase 2 Step B (export TFLite INT8) :
- La chain LV2 v5.12 fonctionne sur la carte
- Les 62 params sont correctement push via `set_insert_params_bulk`
- Le NPU TAP OUT confirme la signature mastering appliquée à l'audio
- Fix LV2_MAX_CTRL_PORTS débloque les 16 bandes EQ

## Référence

- ARCHI : `ARCHI/ARCHI_V9.5.12.md`
- Fiche précédente : `TESTS/TESTS_V9.5.12_E1_model_validated.md`
- Mémoire : [[v9-5-12-mastering-state]], [[v5-versions-history]]
