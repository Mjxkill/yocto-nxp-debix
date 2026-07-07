# Test Fiche : V9.5.12 — E4 ml_features.c (FFT 1024 + 11 features) parité Python

**Date** : 2026-06-08
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | Phase 2 board integration — Step C (Module C features) |
| Version | V9.5.12 |
| Étape | E4 — Port features.py → ml_features.c, parité validée |

## Fichiers ajoutés / modifiés

| Fichier | Rôle |
|---|---|
| `meta-local/recipes-audio/mixer-pro/files/ml_features.h` | API du module |
| `meta-local/recipes-audio/mixer-pro/files/ml_features.c` | Implémentation FFT 1024 + 5 bandes + 5 centroïdes + 1 global |
| `meta-local/recipes-audio/mixer-pro/files/ml_features_test.c` | Programme standalone test parité |
| `Makefile` | Ajout `-lfftw3f` + nouvelle target `ml_features_test` |
| `mixer-pro_1.0.bb` | `DEPENDS += "fftw"`, `RDEPENDS += "fftw"`, install `ml_features_test` |

## Algorithme (parité Python `features.py compute_features_mid_only`)

```
audio stéréo (B, 2, N) → mid = (L+R)/2
  ↓ par frame (hop 512) :
mid × hann_window(1024)
  ↓ rFFT 1024 → spec[513] complex
mag[k] = |spec[k]|, pwr[k] = mag[k]²
  ↓
per band (5) :
  band_db[b] = 20·log10(sqrt(MEAN(pwr[lo:hi])))
  centroid_hz[b] = Σ(freq · mag) / Σ(mag)
global_db = 20·log10(sqrt(MEAN((mid·hann)²)))   ← time-domain RMS
```

## Tests réalisés

| # | Test | Résultat |
|---|---|---|
| 1 | Build mixer-pro avec ml_features.c + fftw3f link | ✅ compile OK |
| 2 | Build ml_features_test (standalone) | ✅ |
| 3 | Deploy sur board | ✅ |
| 4 | Run sur wav contraband 10s (16-bit 48kHz, 936 frames) | ✅ |
| 5 | **Compare CSV C ↔ CSV Python (936 × 11 features)** | ✅ PARITÉ |
| 6 | Bench timing 5 runs | 0.22 s total / 936 frames |

## Résultats parité Python ↔ C

| Feature | mean Δ | max Δ | p99 Δ |
|---|---|---|---|
| mid_b0_db | **0.0000 dB** | 0.0000 | 0.0000 |
| mid_b1_db | **0.0000 dB** | 0.0000 | 0.0000 |
| mid_b2_db | **0.0000 dB** | 0.0000 | 0.0000 |
| mid_b3_db | **0.0000 dB** | 0.0000 | 0.0000 |
| mid_b4_db | **0.0000 dB** | 0.0000 | 0.0000 |
| mid_b0_centroid_hz | 0.0000 Hz | 0.0000 | 0.0000 |
| mid_b1_centroid_hz | 0.0000 Hz | 0.0002 | 0.0001 |
| mid_b2_centroid_hz | 0.0002 Hz | 0.0007 | 0.0005 |
| mid_b3_centroid_hz | 0.0009 Hz | 0.0044 | 0.0034 |
| mid_b4_centroid_hz | 0.0043 Hz | 0.0234 | 0.0143 |
| mid_global_db | **0.0000 dB** | 0.0000 | 0.0000 |

→ Différences sur centroids (max 0.023 Hz @ 8-20 kHz) = rounding float32 négligeable.

## Bench timing

| Mesure | Valeur |
|---|---|
| Total run (936 frames) | 217-220 ms (incl. read WAV + write CSV) |
| Par frame (incl. I/O) | ~238 µs/frame |
| Pure compute estim (sans I/O) | ~50-100 µs/frame |

## Budget cycle 100 Hz avec INT8 + ml_features C

```
Cycle 10 ms :
  ml_features (1 frame compute)  ~0.1 ms
  TFLite INT8 NPU                 0.45 ms
  fx_chain_set_param × 62         <0.1 ms
  ─────────────────────────────────
  Total                          ~0.7 ms
  Marge                           9.3 ms
```

## Erreurs trouvées + corrigées pendant l'implémentation

| Bug | Fix |
|---|---|
| `band_db = 10·log10(SUM power)` au lieu de `10·log10(MEAN power)` | Σ/N puis 10·log10 |
| Centroid pondéré par power | Pondéré par magnitude (= sqrt(power)) |
| Global_db = log de spectre | Global_db = RMS time-domain de `mid·hann` |

## Conclusion

✅ **GO** pour Phase 2 Step D (Thread ml_inference dans mixer-pro) :
- ml_features.c produit des résultats identiques à features.py (différences nulles ou négligeables)
- Compilation Yocto OK avec `fftw` ajouté en DEPENDS/RDEPENDS
- Performance largement compatible cycle 100 Hz (~0.1 ms / frame compute)

## Référence

- ARCHI : `ARCHI/ARCHI_V9.5.12.md`
- Fiches précédentes : `TESTS_V9.5.12_E1_model_validated.md`, `E2_board_chain_validated.md`, `E3_tflite_int8_npu.md`
