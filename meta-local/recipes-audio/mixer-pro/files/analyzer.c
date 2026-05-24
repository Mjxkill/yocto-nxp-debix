/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V7.0-E7.5 — analyzer : FFT + scope worker thread.
 *
 * Self-contained radix-2 Cooley-Tukey FFT (iterative DIT, in-place) on
 * float32. No external dependency (kissfft was considered but pulled in
 * for a 70-line routine ; we inline it). The window is a precomputed
 * Hann (sym), built once at thread start.
 *
 * Threading :
 *   - audio_thread writes ring samples (analyzer_tap_write, inline).
 *   - analyzer_thread (this file) reads the ready side of each ring,
 *     computes the spectrum + scope, stores under the per-tap mutex.
 *   - control_thread snapshots the per-tap output through the mutex for
 *     the JSON wire (mixer-pro.c op:get_meters).
 */

#define _GNU_SOURCE
#include "analyzer.h"

#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern atomic_int g_running_flag_for_analyzer; /* set by mixer-pro.c */

/* ============================== Windowing + FFT ==================== */

/* Precomputed Hann window (symmetric, length FFT_N). Built once. */
static float s_hann[TAP_FFT_N];

static void build_hann(void)
{
	for (int n = 0; n < TAP_FFT_N; n++) {
		s_hann[n] = 0.5f *
			(1.0f - cosf(2.0f * (float)M_PI * (float)n /
				     (float)(TAP_FFT_N - 1)));
	}
}

/* In-place radix-2 Cooley-Tukey iterative DIT FFT.
 * n MUST be a power of two ; we always call with TAP_FFT_N = 1024. */
static void fft_radix2(float *re, float *im, int n)
{
	/* Bit-reverse permutation */
	int j = 0;
	for (int i = 1; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) {
			float t;
			t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	/* Butterflies */
	for (int len = 2; len <= n; len <<= 1) {
		float ang = -2.0f * (float)M_PI / (float)len;
		float wlen_re = cosf(ang);
		float wlen_im = sinf(ang);
		int half = len >> 1;
		for (int i = 0; i < n; i += len) {
			float w_re = 1.0f, w_im = 0.0f;
			for (int k = 0; k < half; k++) {
				float u_re = re[i + k];
				float u_im = im[i + k];
				float v_re = re[i + k + half] * w_re - im[i + k + half] * w_im;
				float v_im = re[i + k + half] * w_im + im[i + k + half] * w_re;
				re[i + k]        = u_re + v_re;
				im[i + k]        = u_im + v_im;
				re[i + k + half] = u_re - v_re;
				im[i + k + half] = u_im - v_im;
				float nw_re = w_re * wlen_re - w_im * wlen_im;
				float nw_im = w_re * wlen_im + w_im * wlen_re;
				w_re = nw_re; w_im = nw_im;
			}
		}
	}
}

/* ============================== Tap lifecycle ====================== */

void analyzer_taps_init(mixer_tap_t taps[N_TAPS])
{
	memset(taps, 0, sizeof(mixer_tap_t) * N_TAPS);
	for (int i = 0; i < N_TAPS; i++) {
		atomic_init(&taps[i].kind, TAP_KIND_NONE);
		atomic_init(&taps[i].a, 0);
		atomic_init(&taps[i].b, -1);
		atomic_init(&taps[i].wpos, 0);
		atomic_init(&taps[i].side, 0);
		atomic_init(&taps[i].ready_seq, 0);
		pthread_mutex_init(&taps[i].out_lock, NULL);
		for (int k = 0; k < TAP_BINS_OUT; k++)
			taps[i].out_spec[k] = -120;
	}
}

void analyzer_taps_destroy(mixer_tap_t taps[N_TAPS])
{
	for (int i = 0; i < N_TAPS; i++)
		pthread_mutex_destroy(&taps[i].out_lock);
}

/* ============================== Thread main ======================== */

/* g_taps_for_analyzer is set by mixer-pro.c before pthread_create. */
extern mixer_tap_t *g_taps_for_analyzer;

/* Pull one ready buffer, run FFT, fill out_spec/out_scope/out_rms. */
static void analyze_tap(mixer_tap_t *t)
{
	int kind = atomic_load_explicit(&t->kind, memory_order_acquire);
	if (kind == TAP_KIND_NONE)
		return;

	int seq = atomic_load_explicit(&t->ready_seq, memory_order_acquire);
	if (seq == 0)
		return;             /* no buffer filled yet */

	/* The "ready" side is the OPPOSITE of the side currently being written.
	 * Once we read it, audio_thread may immediately re-use it for the next
	 * fill (after another roll-over), so we make a local copy first. */
	int cur_side  = atomic_load_explicit(&t->side, memory_order_acquire);
	int ready     = cur_side ^ 1;

	static float L[TAP_FFT_N];
	static float R[TAP_FFT_N];
	memcpy(L, t->ring_l[ready], sizeof(L));
	memcpy(R, t->ring_r[ready], sizeof(R));

	/* RMS over the window (before windowing). */
	float sum_sq = 0;
	for (int n = 0; n < TAP_FFT_N; n++) {
		float m = 0.5f * (L[n] + R[n]);
		sum_sq += m * m;
	}
	float rms = sqrtf(sum_sq / (float)TAP_FFT_N);
	float rms_dB = (rms > 1e-7f) ? (20.0f * log10f(rms)) : -120.0f;
	if (rms_dB < -120.0f) rms_dB = -120.0f;
	if (rms_dB >    6.0f) rms_dB =    6.0f;

	/* Real FFT on the (L+R)/2 windowed signal. Imaginary part starts at 0. */
	static float re[TAP_FFT_N];
	static float im[TAP_FFT_N];
	for (int n = 0; n < TAP_FFT_N; n++) {
		float m = 0.5f * (L[n] + R[n]);
		re[n] = m * s_hann[n];
		im[n] = 0.0f;
	}
	fft_radix2(re, im, TAP_FFT_N);

	/* Downsample : 512 useful bins → TAP_BINS_OUT (128) via 4:1 peak hold.
	 * Magnitude → dB, clipped [-120 .. 0]. */
	const int group = (TAP_FFT_N / 2) / TAP_BINS_OUT;   /* 4 */
	const float norm = 2.0f / (float)TAP_FFT_N;          /* one-sided fft scale */
	int8_t out_spec_tmp[TAP_BINS_OUT];
	for (int b = 0; b < TAP_BINS_OUT; b++) {
		float mag_max = 0.0f;
		for (int g = 0; g < group; g++) {
			int k = b * group + g;
			float m = sqrtf(re[k] * re[k] + im[k] * im[k]) * norm;
			if (m > mag_max) mag_max = m;
		}
		float db = (mag_max > 1e-7f) ? (20.0f * log10f(mag_max)) : -120.0f;
		if (db < -120.0f) db = -120.0f;
		if (db >    0.0f) db =    0.0f;
		out_spec_tmp[b] = (int8_t)lrintf(db);
	}

	/* Scope : last TAP_SCOPE_N stereo pairs of the window, quantized S16. */
	int16_t out_scope_tmp[TAP_SCOPE_N * 2];
	int off = TAP_FFT_N - TAP_SCOPE_N;
	for (int s = 0; s < TAP_SCOPE_N; s++) {
		float l = L[off + s];
		float r = R[off + s];
		if (l >  1.0f) l =  1.0f;  if (l < -1.0f) l = -1.0f;
		if (r >  1.0f) r =  1.0f;  if (r < -1.0f) r = -1.0f;
		out_scope_tmp[s * 2]     = (int16_t)lrintf(l * 32767.0f);
		out_scope_tmp[s * 2 + 1] = (int16_t)lrintf(r * 32767.0f);
	}

	pthread_mutex_lock(&t->out_lock);
	memcpy(t->out_spec,  out_spec_tmp,  sizeof(out_spec_tmp));
	memcpy(t->out_scope, out_scope_tmp, sizeof(out_scope_tmp));
	t->out_rms_dB = rms_dB;
	t->out_seq++;
	pthread_mutex_unlock(&t->out_lock);
}

void *analyzer_thread(void *arg)
{
	(void)arg;
	build_hann();

	/* Low-RT priority : run on the same core but never preempt audio. */
	struct sched_param sp = { .sched_priority = RT_PRIO_ANALYZER };
	if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
		fprintf(stderr, "analyzer thread: SCHED_FIFO prio %d failed, "
				"running SCHED_OTHER\n", RT_PRIO_ANALYZER);
	}
	/* V9.0 — pin sur cores 0,1 (non-RT critique, hors des cores audio isolés) */
	{
		cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0, &cs); CPU_SET(1, &cs);
		pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
	}

	while (atomic_load_explicit(&g_running_flag_for_analyzer,
				    memory_order_acquire)) {
		for (int t = 0; t < N_TAPS; t++)
			analyze_tap(&g_taps_for_analyzer[t]);
		usleep(ANALYZER_PERIOD_US);
	}
	return NULL;
}
