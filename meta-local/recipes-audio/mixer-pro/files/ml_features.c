/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V9.5.12 — ml_features : implémentation FFT 1024 + 11 features/frame.
 *
 * Port de training/features.py compute_features_mid_only.
 * Référence Python : SR=48000, FRAME_SIZE=1024, HOP_SIZE=512.
 *
 * Bandes (5) :
 *   low      :   20 Hz –   200 Hz
 *   lowmid   :  200 Hz –   800 Hz
 *   mid      :  800 Hz –  2500 Hz
 *   highmid  : 2500 Hz –  8000 Hz
 *   high     : 8000 Hz – 20000 Hz
 *
 * Indices STFT @ N=1024, sr=48000 :
 *   bin_hz = sr / N = 46.875 Hz
 *   freqs[k] = k * bin_hz  (k = 0..512)
 *   Python utilise np.searchsorted(freqs, lo_hz, 'left') et 'right' pour hi.
 */

#include "ml_features.h"

#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define N             ML_FEATURES_FRAME_SIZE     /* 1024 */
#define N_HALF        (N / 2 + 1)                /* 513 */
#define SR            ML_FEATURES_SAMPLE_RATE    /* 48000 */
#define NB            ML_FEATURES_N_BANDS        /* 5 */

/* Bandes Hz (lo, hi) — DOIT correspondre à features.py BAND_HZ. */
static const float band_hz[NB][2] = {
    {   20.0f,   200.0f},
    {  200.0f,   800.0f},
    {  800.0f,  2500.0f},
    { 2500.0f,  8000.0f},
    { 8000.0f, 20000.0f},
};

/* Indices bins par bande, pré-calculés à init.
 * lo : searchsorted left  (premier bin avec freq >= lo)
 * hi : searchsorted right (premier bin avec freq >  hi) [exclusive]
 */
static int band_lo[NB], band_hi[NB];

/* Hann window pré-calculée. */
static float g_hann[N];

/* Buffers FFT — alloués via fftwf_alloc pour alignement SIMD. */
static float          *g_in  = NULL;
static fftwf_complex  *g_out = NULL;
static fftwf_plan      g_plan;

static int g_initialized = 0;

/* searchsorted 'left' : premier index >= value.
 * freqs[k] = k * (sr/N), donc on calcule directement.
 * 'right' = premier index > value.
 */
static int searchsorted_freq_left(float v_hz)
{
    /* freqs[k] >= v ⇔ k >= ceil(v * N / sr) */
    int k = (int)ceilf(v_hz * (float)N / (float)SR);
    if (k < 0) k = 0;
    if (k > N_HALF) k = N_HALF;
    return k;
}
static int searchsorted_freq_right(float v_hz)
{
    /* freqs[k] > v ⇔ k > v * N / sr ⇔ k >= floor(v * N / sr) + 1 */
    int k = (int)floorf(v_hz * (float)N / (float)SR) + 1;
    if (k < 0) k = 0;
    if (k > N_HALF) k = N_HALF;
    return k;
}

int ml_features_init(void)
{
    if (g_initialized)
        return 0;

    /* Hann window : 0.5 * (1 - cos(2π n / (N-1))). */
    for (int n = 0; n < N; n++) {
        g_hann[n] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)n
                                         / (float)(N - 1)));
    }

    /* Indices bandes. */
    for (int b = 0; b < NB; b++) {
        band_lo[b] = searchsorted_freq_left(band_hz[b][0]);
        band_hi[b] = searchsorted_freq_right(band_hz[b][1]);
    }

    /* FFT buffers + plan. FFTW_MEASURE = optimise pour cette taille. */
    g_in  = (float *)fftwf_alloc_real(N);
    g_out = (fftwf_complex *)fftwf_alloc_complex(N_HALF);
    if (!g_in || !g_out) {
        if (g_in)  fftwf_free(g_in);
        if (g_out) fftwf_free(g_out);
        return -1;
    }
    g_plan = fftwf_plan_dft_r2c_1d(N, g_in, g_out, FFTW_MEASURE);
    if (!g_plan) {
        fftwf_free(g_in);
        fftwf_free(g_out);
        return -1;
    }

    g_initialized = 1;
    return 0;
}

void ml_features_cleanup(void)
{
    if (!g_initialized)
        return;
    fftwf_destroy_plan(g_plan);
    fftwf_free(g_in);
    fftwf_free(g_out);
    g_in  = NULL;
    g_out = NULL;
    g_initialized = 0;
}

/* Core : windowed FFT → magnitude/power[513] → bandes + centroids + global.
 *
 * Référence Python features.py compute_features() :
 *   sp_m  = |rFFT(mid * hann)|           # magnitudes
 *   pwr   = sp_m²
 *   band_db[b] = 20·log10(sqrt(MEAN(pwr[lo:hi])))    (= 10·log10(mean power))
 *   centroid_hz[b] = (freqs[lo:hi] * sp_m[lo:hi]).sum() / sp_m[lo:hi].sum()
 *   global_db = 20·log10(sqrt(MEAN((mid*hann)²)))     (time-domain RMS)
 */
static void compute_features_from_mid(const float *mid, ml_features_t *out)
{
    /* 1. Apply Hann window. */
    for (int n = 0; n < N; n++) {
        g_in[n] = mid[n] * g_hann[n];
    }

    /* 1b. Time-domain RMS dB (Python global_db = 20·log10(sqrt(mean(frame²)))). */
    float sum_sq_t = 0.0f;
    for (int n = 0; n < N; n++) sum_sq_t += g_in[n] * g_in[n];
    const float rms_t = sqrtf(sum_sq_t / (float)N + 1e-12f);
    out->mid_global_db = 20.0f * log10f(rms_t + 1e-12f);

    /* 2. FFT 1024-point real-to-complex. */
    fftwf_execute(g_plan);

    /* 3. Magnitude + power spectrum (513 bins). */
    static float mag[N_HALF], pwr[N_HALF];   /* static BSS, RT-safe */
    for (int k = 0; k < N_HALF; k++) {
        const float re = g_out[k][0];
        const float im = g_out[k][1];
        pwr[k] = re * re + im * im;
        mag[k] = sqrtf(pwr[k]);
    }

    /* 4. Par bande : energy = sqrt(MEAN(pwr[lo:hi])) → 20·log10
     *               centroid = Σ(freq · mag) / Σ(mag)
     */
    const float bin_hz = (float)SR / (float)N;   /* 46.875 */
    for (int b = 0; b < NB; b++) {
        const int lo = band_lo[b];
        const int hi = band_hi[b];
        const int nb = hi - lo;
        float sum_pwr = 0.0f, sum_mag = 0.0f, sum_w_mag = 0.0f;
        for (int k = lo; k < hi; k++) {
            sum_pwr   += pwr[k];
            sum_mag   += mag[k];
            sum_w_mag += mag[k] * ((float)k * bin_hz);
        }
        const float energy = sqrtf((nb > 0 ? sum_pwr / (float)nb : 0.0f) + 1e-12f);
        out->mid_band_db[b] = 20.0f * log10f(energy + 1e-12f);
        out->mid_centroid_hz[b] = (sum_mag > 1e-20f)
                                    ? (sum_w_mag / sum_mag) : 0.0f;
    }
}

/* S32_LE → float (Q1.31 normalisé [-1, +1]). */
static inline float s32_to_f(int32_t s)
{
    /* INT32_MAX = 2^31 - 1 ≈ 2.147e9 */
    return (float)s * (1.0f / 2147483648.0f);
}

void ml_features_process_frame(const int32_t *L, const int32_t *R,
                                ml_features_t *out)
{
    /* mid = (L + R) / 2, S32 → float. */
    static float mid[N];
    for (int n = 0; n < N; n++) {
        const float l = s32_to_f(L[n]);
        const float r = s32_to_f(R[n]);
        mid[n] = 0.5f * (l + r);
    }
    compute_features_from_mid(mid, out);
}

void ml_features_process_frame_f32(const float *L, const float *R,
                                    ml_features_t *out)
{
    static float mid[N];
    for (int n = 0; n < N; n++) {
        mid[n] = 0.5f * (L[n] + R[n]);
    }
    compute_features_from_mid(mid, out);
}
