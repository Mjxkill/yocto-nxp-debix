/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V9.5.12 — ml_features : extracteur features audio pour modèle ML mastering.
 *
 * Port C/NEON de training/features.py (V9.5.3-v2 mid-only, 11 features/frame).
 *
 * Pipeline pour 1 frame de 1024 samples stéréo @ 48 kHz :
 *   1. mid = (L + R) / 2                (combine stéréo)
 *   2. hann_window * mid                (1024 multiplies)
 *   3. FFT 1024-point R2C               (fftw3f)
 *   4. power = |spec|² (513 bins)
 *   5. 5 bandes RMS dB : 20-200, 200-800, 800-2500, 2500-8000, 8000-20000 Hz
 *   6. 5 centroïdes spectraux Hz par bande
 *   7. 1 niveau global mid dB
 *
 * Out : 11 floats (5 + 5 + 1).
 *
 * Frame_size = 1024 samples = 21.3 ms @ 48 kHz.
 * Hop_size  = 512 samples  = 10.7 ms → feature rate 93.75 Hz.
 *
 * Coût compute par frame (estimation Cortex-A53 NEON) :
 *   - 1024 multiplies pour Hann       : ~3 µs
 *   - FFT 1024 r2c fftw3f             : ~5-10 µs
 *   - bandes/centroïdes/log10         : ~5 µs
 *   - Total                           : ~15-20 µs / frame
 *
 * Cycle 100 Hz (10 ms) : 1 frame neuve → ~20 µs (largement OK).
 *
 * Aucune allocation runtime après ml_features_init() — RT-safe.
 */
#ifndef __ML_FEATURES_H__
#define __ML_FEATURES_H__

#include <stdint.h>

#define ML_FEATURES_FRAME_SIZE    1024
#define ML_FEATURES_HOP_SIZE      512
#define ML_FEATURES_SAMPLE_RATE   48000
#define ML_FEATURES_N_BANDS       5
#define ML_FEATURES_N_FEATURES    11   /* 5 bands + 5 centroids + 1 global */

typedef struct {
    float mid_band_db[ML_FEATURES_N_BANDS];      /* dB per band */
    float mid_centroid_hz[ML_FEATURES_N_BANDS];  /* Hz centroid per band */
    float mid_global_db;                          /* dB total mid */
} ml_features_t;

/* Init : alloue Hann window + FFT plan. Une seule fois au démarrage.
 * Returns 0 on success, -1 on error.
 */
int ml_features_init(void);

/* Process 1 frame de 1024 samples stéréo S32_LE.
 * L, R : pointeurs vers 1024 samples chacun (int32, fixed-point Q1.31).
 *        Convertis en float [-1.0, +1.0] via division par INT32_MAX.
 * out  : pointeur vers ml_features_t pour écrire les 11 floats.
 *
 * Aucune allocation. Safe à appeler depuis thread RT.
 */
void ml_features_process_frame(const int32_t *L, const int32_t *R,
                                ml_features_t *out);

/* Variante float : si l'audio est déjà float [-1, +1] (ex: post-mix mixer-pro).
 * L, R : pointeurs vers 1024 floats chacun.
 */
void ml_features_process_frame_f32(const float *L, const float *R,
                                    ml_features_t *out);

/* Free FFT plan + Hann window. À appeler au shutdown. */
void ml_features_cleanup(void);

#endif /* __ML_FEATURES_H__ */
