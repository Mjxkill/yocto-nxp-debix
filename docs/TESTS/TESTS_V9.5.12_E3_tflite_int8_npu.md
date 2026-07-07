# Test Fiche : V9.5.12 — E3 export TFLite INT8 + bench NPU validé

**Date** : 2026-06-08
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | Phase 2 board integration — Step B (export INT8 + bench NPU) |
| Version | V9.5.12 |
| Étape | E3 — Modèle v5.12 quantizé INT8 + benchmark NPU |
| Modèle ML source | `conv_v5_12_full_epoch029.pt` |
| Script export | `training/export_tflite_int8_v5_12.py` |
| Outils utilisés | litert-torch (PyTorch → TFLite f32) + ai-edge-quantizer (f32 → INT8) |

## Pipeline d'export

```
PyTorch model (52K params)
  │
  ▼ litert_torch.convert()
TFLite float32  213 KB  (28 f32 + 10 i32 tensors)
  │
  ▼ ai_edge_quantizer.Quantizer + calibration 200 frames features réelles
TFLite INT8     67 KB   (20 f32 + 11 i8 + 13 i32 tensors)
                3.2× plus petit
```

## Tests réalisés

| # | Test | Résultat |
|---|---|---|
| 1 | Conversion PyTorch → TFLite f32 via litert-torch | ✅ 213 KB |
| 2 | Quantization f32 → INT8 via ai-edge-quantizer + calibration sur 200 frames | ✅ 67 KB (3.2× ÷) |
| 3 | Compare PyTorch vs TFLite f32 (200 samples) | ✅ mean Δ = 0, max Δ = 0 (round-trip parfait) |
| 4 | Compare PyTorch vs TFLite INT8 (200 samples) | ✅ mean Δ = 0.28%, max Δ = 14.5% |
| 5 | Deploy INT8 sur board (scp) | ✅ /tmp/mastering_v5_12_int8.tflite |
| 6 | Bench CPU XNNPACK 4 threads f32 | 36.7 ms (trop lent) |
| 7 | Bench NPU VX delegate f32 | 2.005 ms |
| 8 | Bench CPU XNNPACK 4 threads INT8 | 34.8 ms (toujours trop lent) |
| 9 | **Bench NPU VX delegate INT8** | **0.446 ms** ⭐ (4.5× plus rapide que f32) |

## Budget cycle 100 Hz (10 ms) avec INT8

| Étape | Temps |
|---|---|
| Features extraction (FFT 1024 NEON) | ~0.5 ms |
| **NPU TFLite inférence INT8** | **0.45 ms** |
| fx_chain_set_param × 62 (in-process) | <0.1 ms |
| **Total cycle** | **~1.05 ms** |
| Marge | **8.95 ms** |

Possibilité d'aller jusqu'à 500 Hz update (2 ms cycle).

## Précision INT8 vs PyTorch

```
Mean Δparams normalisés : 0.28%
Max  Δparams normalisés : 14.5% (queue de distribution sur quelques params extrêmes)
```

Impact sur params LV2 réels (range typique) :
- gain_db ±12 dB : mean error 0.07 dB, max 3.5 dB sur quelques bandes
- freq_hz 20-20000 : mean error 56 Hz, max 2900 Hz
- exciter.amount [0,1] : mean 0.003, max 0.15

Acceptable pour POC. Les params extrêmes peu utilisés (gain_db saturé ±12 dB) sont les plus erronés.

## Conclusion

✅ **GO** pour Phase 2 Step C (Module C ml_features.c NEON) :
- INT8 fonctionne sur NPU (0.45 ms)
- Pipeline complet PyTorch → INT8 .tflite validé via litert-torch + ai-edge-quantizer
- Budget cycle 100 Hz largement tenu (~1 ms total)
- Précision INT8 mean 0.28% — acceptable

## Référence

- ARCHI : `ARCHI/ARCHI_V9.5.12.md` (à mettre à jour avec budget INT8)
- Fiche précédente : `TESTS/TESTS_V9.5.12_E2_board_chain_validated.md`
- Outils : litert-torch (PyTorch direct), ai-edge-quantizer (Google standalone)
