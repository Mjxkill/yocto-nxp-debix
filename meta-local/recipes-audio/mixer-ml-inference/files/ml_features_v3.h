/* SPDX-License-Identifier: GPL-2.0-or-later
 * V9.5.20 — ml_features v3 (Mel+MFCC+BF+ΔMel, parité training/features_v3.py)
 */
#ifndef ML_FEATURES_V3_H
#define ML_FEATURES_V3_H

#include "ml_features_v3_tables.h"

#define MLF3_N_WINDOW 10     /* fenêtre modèle : 10 trames de 10 ms */
#define MLF3_LONG_DECIM 2    /* V9.5.21 : FFT 8192 calculée 1 cycle/2 (50 Hz) */

typedef struct {
    /* ring audio 100 ms pour la FFT longue */
    float ring100[MLF3_WIN_LONG];
    int   ring_filled;
    /* mel précédent pour delta */
    float prev_mel[MLF3_N_MEL_SHORT];
    int   has_prev_mel;
    /* V9.5.21 — cache des 16 bandes BF : la FFT 8192 (fenêtre 100 ms) est
     * décimée à 1 cycle/2 (un estimé basses sur 100 ms n'a pas besoin de
     * 100 Hz). Réduit le CPU/trafic mémoire du daemon → moins de stalls
     * capture. La valeur cachée est réutilisée le cycle sauté. */
    float cached_bf[MLF3_N_BF];
    int   has_cached_bf;
    int   long_phase;
    /* ring de 10 trames de features (fenêtre modèle) */
    float window[MLF3_N_WINDOW * MLF3_N_FEATURES];
    int   n_frames;
    float last_features[MLF3_N_FEATURES];
} mlf3_state_t;

void mlf3_init(mlf3_state_t *st);
void mlf3_push_frame(mlf3_state_t *st, const float *audio480);
void mlf3_fill_tensor(const mlf3_state_t *st, float *tensor /* 125*10 */);

#endif
