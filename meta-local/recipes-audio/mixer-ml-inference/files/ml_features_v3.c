/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V9.5.20 — ml_features v3 : encoder Mel+MFCC+BF+ΔMel pour le modèle
 * mastering enveloppe spectrale (parité avec training/features_v3.py).
 *
 * Par trame de 10 ms (480 samples @ 48 kHz), par canal :
 *   [0..43]   log10 Mel 44 bandes (200 Hz - 20 kHz, FFT 1024 zero-paddée)
 *   [44..63]  20 MFCC (DCT du log Mel)
 *   [64..79]  16 bandes BF log (20 - 630 Hz, FFT 8192 sur les 100 ms passées)
 *   [80..123] 44 delta-Mel (mel[t] − mel[t-1])
 *   [124]     RMS dB de la trame
 * puis normalisation par bande (carto MLF3_NORM, cf diag biais fréquentiels).
 *
 * Coût mesuré (bench FFTW NEON A53, 2026-06-10) : FFT1024 17.6 µs +
 * FFT8192 217 µs + mel/dct ~10 µs ≈ 250 µs / canal / trame → 2 canaux 5 % CPU.
 *
 * Usage :
 *   mlf3_state_t st; mlf3_init(&st);
 *   à chaque bloc de 480 samples : mlf3_push_frame(&st, block) → features
 *   (125 floats) écrites dans st.last_features + ring 10 trames st.window.
 */
#include "ml_features_v3.h"
#include "ml_features_v3_tables.h"

#include <fftw3.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Plans FFTW partagés entre canaux (thread du daemon unique). */
static fftwf_plan g_plan_short = NULL;
static fftwf_plan g_plan_long  = NULL;
static float          *g_in_short, *g_in_long;
static fftwf_complex  *g_out_short, *g_out_long;
static float g_win_short[MLF3_FRAME_SIZE];
static float g_win_long[MLF3_WIN_LONG];
static int g_global_init = 0;

static void mlf3_global_init(void)
{
    if (g_global_init) return;
    g_in_short  = fftwf_alloc_real(MLF3_FFT_SHORT);
    g_out_short = fftwf_alloc_complex(MLF3_FFT_SHORT / 2 + 1);
    g_in_long   = fftwf_alloc_real(MLF3_FFT_LONG);
    g_out_long  = fftwf_alloc_complex(MLF3_FFT_LONG / 2 + 1);
    g_plan_short = fftwf_plan_dft_r2c_1d(MLF3_FFT_SHORT, g_in_short, g_out_short,
                                          FFTW_MEASURE);
    g_plan_long  = fftwf_plan_dft_r2c_1d(MLF3_FFT_LONG, g_in_long, g_out_long,
                                          FFTW_MEASURE);
    /* numpy.hanning(N) = 0.5 - 0.5*cos(2*pi*n/(N-1)) — version symétrique */
    for (int i = 0; i < MLF3_FRAME_SIZE; i++)
        g_win_short[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i
                                             / (MLF3_FRAME_SIZE - 1));
    for (int i = 0; i < MLF3_WIN_LONG; i++)
        g_win_long[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i
                                            / (MLF3_WIN_LONG - 1));
    g_global_init = 1;
}

void mlf3_init(mlf3_state_t *st)
{
    mlf3_global_init();
    memset(st, 0, sizeof(*st));
    st->has_prev_mel = 0;
}

/* Calcule la trame de features depuis les 480 nouveaux samples.
 * audio480 : bloc mono float32 (le canal).
 * Écrit st->last_features[125] (normalisé) et pousse dans le ring 10 trames. */
void mlf3_push_frame(mlf3_state_t *st, const float *audio480)
{
    float feats[MLF3_N_FEATURES];
    memset(feats, 0, sizeof(feats));

    /* --- ring audio 100 ms pour la FFT longue --- */
    memmove(st->ring100, st->ring100 + MLF3_FRAME_SIZE,
            (MLF3_WIN_LONG - MLF3_FRAME_SIZE) * sizeof(float));
    memcpy(st->ring100 + MLF3_WIN_LONG - MLF3_FRAME_SIZE, audio480,
           MLF3_FRAME_SIZE * sizeof(float));
    if (st->ring_filled < MLF3_WIN_LONG)
        st->ring_filled += MLF3_FRAME_SIZE;

    /* --- court terme : FFT 1024 --- */
    for (int i = 0; i < MLF3_FRAME_SIZE; i++)
        g_in_short[i] = audio480[i] * g_win_short[i];
    memset(g_in_short + MLF3_FRAME_SIZE, 0,
           (MLF3_FFT_SHORT - MLF3_FRAME_SIZE) * sizeof(float));
    fftwf_execute(g_plan_short);

    static float pwr_s[MLF3_FFT_SHORT / 2 + 1];
    for (int k = 0; k <= MLF3_FFT_SHORT / 2; k++)
        pwr_s[k] = g_out_short[k][0] * g_out_short[k][0]
                 + g_out_short[k][1] * g_out_short[k][1];

    float mel[MLF3_N_MEL_SHORT];
    for (int b = 0; b < MLF3_N_MEL_SHORT; b++) {
        float acc = 0.0f;
        const int s = MLF3_MEL44_START[b], c = MLF3_MEL44_COUNT[b];
        for (int k = 0; k < c; k++) acc += pwr_s[s + k];
        mel[b] = log10f(acc / (float)(c > 0 ? c : 1) + 1e-10f);
        feats[b] = mel[b];
    }

    /* --- MFCC : DCT 20×44 --- */
    for (int m = 0; m < MLF3_N_MFCC; m++) {
        float acc = 0.0f;
        for (int b = 0; b < MLF3_N_MEL_SHORT; b++)
            acc += MLF3_DCT[m][b] * mel[b];
        feats[MLF3_N_MEL_SHORT + m] = acc;
    }

    /* --- long terme : FFT 8192 sur la fenêtre 100 ms (décimée 1 cycle/2) ---
     * Parité features_v3.py : si le ring n'est pas plein, le segment plus
     * court est fenêtré par la FIN de la fenêtre Hann longue.
     * V9.5.21 : calculée seulement 1 cycle sur 2 (long_phase) ; le cycle
     * sauté réutilise cached_bf. Au 1er cycle (pas de cache) on calcule. */
    st->long_phase = (st->long_phase + 1) % MLF3_LONG_DECIM;
    if (st->long_phase != 0 && st->has_cached_bf) {
        /* cycle sauté : réutilise les BF cachées */
        for (int b = 0; b < MLF3_N_BF; b++)
            feats[64 + b] = st->cached_bf[b];
    } else {
        int avail = st->ring_filled < MLF3_WIN_LONG ? st->ring_filled
                                                     : MLF3_WIN_LONG;
        const float *seg = st->ring100 + (MLF3_WIN_LONG - avail);
        const float *win = g_win_long + (MLF3_WIN_LONG - avail);
        for (int i = 0; i < avail; i++)
            g_in_long[i] = seg[i] * win[i];
        memset(g_in_long + avail, 0,
               (MLF3_FFT_LONG - avail) * sizeof(float));
        fftwf_execute(g_plan_long);
        for (int b = 0; b < MLF3_N_BF; b++) {
            float acc = 0.0f;
            const int s = MLF3_MELBF_START[b], c = MLF3_MELBF_COUNT[b];
            for (int k = 0; k < c; k++) {
                const float re = g_out_long[s + k][0];
                const float im = g_out_long[s + k][1];
                acc += re * re + im * im;
            }
            feats[64 + b] = log10f(acc / (float)(c > 0 ? c : 1) + 1e-10f);
            st->cached_bf[b] = feats[64 + b];
        }
        st->has_cached_bf = 1;
    }

    /* --- delta-Mel --- */
    if (st->has_prev_mel)
        for (int b = 0; b < MLF3_N_MEL_SHORT; b++)
            feats[80 + b] = mel[b] - st->prev_mel[b];
    memcpy(st->prev_mel, mel, sizeof(mel));
    st->has_prev_mel = 1;

    /* --- RMS dB (fenêtré, comme features_v3.py : rms du frame*win) --- */
    {
        float acc = 0.0f;
        for (int i = 0; i < MLF3_FRAME_SIZE; i++) {
            const float v = audio480[i] * g_win_short[i];
            acc += v * v;
        }
        const float rms = sqrtf(acc / MLF3_FRAME_SIZE + 1e-12f);
        feats[MLF3_N_FEATURES - 1] = 20.0f * log10f(rms + 1e-12f);
    }

    /* --- carto de normalisation --- */
    for (int i = 0; i < MLF3_N_FEATURES; i++)
        feats[i] *= MLF3_NORM[i];

    /* --- ring 10 trames (fenêtre du modèle) --- */
    memmove(st->window, st->window + MLF3_N_FEATURES,
            (MLF3_N_WINDOW - 1) * MLF3_N_FEATURES * sizeof(float));
    memcpy(st->window + (MLF3_N_WINDOW - 1) * MLF3_N_FEATURES, feats,
           sizeof(feats));
    if (st->n_frames < MLF3_N_WINDOW) st->n_frames++;
    memcpy(st->last_features, feats, sizeof(feats));
}

/* Remplit le tenseur modèle (125, 10) row-major : tensor[f][t].
 * Si moins de 10 trames vues : padding par répétition de la 1re (parité
 * avec eval_v5_19.py). */
void mlf3_fill_tensor(const mlf3_state_t *st, float *tensor /* 125*10 */)
{
    for (int t = 0; t < MLF3_N_WINDOW; t++) {
        int src_t = t;
        if (st->n_frames < MLF3_N_WINDOW) {
            const int missing = MLF3_N_WINDOW - st->n_frames;
            src_t = t < missing ? MLF3_N_WINDOW - st->n_frames : t;
            /* trames absentes → première trame dispo */
            if (t < missing) src_t = MLF3_N_WINDOW - st->n_frames;
        }
        for (int f = 0; f < MLF3_N_FEATURES; f++)
            tensor[f * MLF3_N_WINDOW + t] =
                st->window[src_t * MLF3_N_FEATURES + f];
    }
}
