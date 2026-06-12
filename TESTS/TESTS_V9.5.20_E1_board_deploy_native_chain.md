# TESTS V9.5.20 E1 — Déploiement board mastering ML enveloppe spectrale + chaîne native

Date : 2026-06-11 / 2026-06-12

## Versions

| Artefact | Référence |
|---|---|
| Modèle | conv_v5_20_full_epoch029.pt → mastering_v5_20_int8.tflite (INLINE) md5 `ae1f3ad7feb006932dbc7b4d687b1fee` |
| mixer-pro | md5 `2732d1ac6ba45fa3399594867f0d0443` |
| mixer-ml-inference | md5 `d7434d05dbfa2a2319c2841ab00cca63` |
| mixer-gui-http | md5 `2c43e0743b5a4b4140cc9503d428c85d` |
| Firmware SOF | md5 `ce59a902038ee729034a5259da95b0ac` (version dmesg 2:10:0-4bea8, + DSP load SW REGs 0xE0/0xE4) |
| Topology | inchangée (sof-imx8mp-tac5212.tplg, DMA 2 ms) |

## Architecture déployée

```
USB IN 8/9 → SHM tap → daemon mixer-ml-inference (cores 0-1, nice 10) :
  features v3 ×2 canaux (FFT 1024 + 8192, parité C/Python max 4e-5)
  2 invocations NPU INT8 par cycle 10 ms (0.81 ms/invoke, 16 % NPU)
  gate silences RMS < -65 dB (slew ~50 ms) ; passthrough → params neutres
  moyenne glissante 250 ms sur les 6 params du limiteur
  push bulk 100 Hz (137 params, ~3.5 KB, socket Unix)
→ chaîne insert 100 % NATIVE (audio_thread core 2 RT99) :
  slot 0 spectral_env  : FIR 256 phase linéaire par canal, reconstruite des 64
                         gains (l_gN/r_gN), interpolation taps 10 ms
  slot 1 exciter_native : parité surrogate calibré Calf (HPF2 ×1.05, β(drive))
  slot 2 limiter_native : parité surrogate training (env follower + tanh ceiling)
  → bypass M/A par slot (GUI), jamais poussé par le daemon
```

## Problèmes rencontrés et résolus

1. **TFLite C-API "Input tensor N lacks data"** : ai-edge-quantizer ≥ 256 KB
   sérialise les buffers en format offset-based illisible par le build C
   2.16.2 (Python OK). Fix : resérialisation inline via
   `ai_edge_litert.tools.flatbuffer_utils.write_model` (diff sortie = 0.0).
2. **Xruns chaîne LV2 (24/30 s)** : LSP Limiter ~1 ms + recalcul interne par
   set_param. Fix : moteurs natifs → prof_mix 1500 → ~370 µs, 0 xrun @ 100 Hz.
3. **Gate coupait la musique calme** : seuil -50 dB > intro mesurée -56 dB.
   Fix : -65 dB.
4. **Passthrough laissait le dernier mastering figé** : push params neutres
   sur transition (g_source init -1 pour forcer la 1re détection).
5. **Glitchs analyzer FFT 4096** : worker RT60 bursts 4 FFT sur cores 0-1.
   Fix : SCHED_OTHER nice 10 core 0 + round-robin 1 tap/passe.
6. **Engine "passthrough" des bus FX send inexistant** (la GUI le proposait
   depuis V9.2 mais l'init échouait) : fx_init_passthrough ajouté.

## GUI (panel Assistant + topbar)

- Courbe enveloppe spectrale live L (vert) / R (orange), ±14 dB, 5 Hz
- FFT IN (in8/9, zone bleu-gris) + FFT OUT (out0/1 post-insert, ligne ambre)
  superposées — analyzer FFT 4096 (fenêtre 85 ms ≈ fenêtre modèle), 128 bins
  LOG 20 Hz-20 kHz (le sub est résolu)
- Dials exciter + limiter (th/g_in aussi en dB) ; boutons M/A par effet
- Taps analyzer 2/3 auto on/off à l'ouverture/fermeture du panel
- Topbar : charge CPU0-3 (/proc/stat), NPU + GPU (galcore gc/load),
  **DSP (SW REG 0xE0 fw SOF, heartbeat 0xE4 anti-stale)** — poll 1 Hz

## Mesures

| Métrique | Valeur |
|---|---|
| prof_mix chaîne native | 360-405 µs (vs ~1500 µs LV2) |
| xruns pendant lecture (40 s) | 0 |
| NPU | 0.81 ms/invoke ; ~8-16 % @ 100 Hz ×2 canaux |
| Daemon CPU (cores 0-1) | ~18-23 % |
| **DSP HiFi4** | **~72 %** (matrix 16×8 + 8 strips multiband DRC) |
| Parité features C/Python | max 4e-5 (100 % OK) |
| Parité TFLite INT8 vs PyTorch | mean 0.018, p95 0.071 (enveloppe p95 1.73 dB) |

## Test utilisateur : OUI

- v5.20 ep29 éval PC : « pas mal du tout… j'ai été surpris de retrouver de
  l'air et une bonne qualité de son » → GO déploiement
- Board : limiter « limite plus qu'avant — il fonctionne vraiment maintenant »
  (le natif est fidèle au surrogate d'entraînement, le LSP ne l'était pas)
- Lissage limiteur 250 ms validé à l'écoute
- « plus de glitch » après fix analyzer

## Reste à faire (sprints suivants)

- v5.21 : loss spectrale loudness-normalisée (sub + sur-compression)
- Offload insert chain core 3 (TODO mémorisé) — marge audio_thread
- DSP à 72 % : surveiller avant tout nouvel effet DSP
