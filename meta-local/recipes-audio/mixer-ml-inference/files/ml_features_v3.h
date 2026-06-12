/* SPDX-License-Identifier: GPL-2.0-or-later
 * V9.5.20 — ml_features v3 (Mel+MFCC+BF+ΔMel, parité training/features_v3.py)
 */
#ifndef ML_FEATURES_V3_H
#define ML_FEATURES_V3_H

#include "ml_features_v3_tables.h"

#define MLF3_N_WINDOW 10     /* fenêtre modèle : 10 trames de 10 ms */

typedef struct {
    /* ring audio 100 ms pour la FFT longue */
    float ring100[MLF3_WIN_LONG];
    int   ring_filled;
    /* mel précédent pour delta */
    float prev_mel[MLF3_N_MEL_SHORT];
    int   has_prev_mel;
    /* ring de 10 trames de features (fenêtre modèle) */
    float window[MLF3_N_WINDOW * MLF3_N_FEATURES];
    int   n_frames;
    float last_features[MLF3_N_FEATURES];
} mlf3_state_t;

void mlf3_init(mlf3_state_t *st);
void mlf3_push_frame(mlf3_state_t *st, const float *audio480);
void mlf3_fill_tensor(const mlf3_state_t *st, float *tensor /* 125*10 */);

#endif
