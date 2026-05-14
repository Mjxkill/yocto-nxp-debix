/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V8.2 — Async sample rate converter (cubic Lagrange 4-tap + PID).
 * Voir asrc.h pour la philosophie.
 */

#include "asrc.h"
#include <math.h>
#include <string.h>

/* Update rate is one call per audio_thread iteration = 500 Hz (period 2 ms).
 * EMA tau = 1.0 s → alpha = 1 - exp(-2ms/1s) ≈ 0.002 */
#define ASRC_EMA_ALPHA   0.002f

/* PID gains. Empirical tuning :
 *   Kp = 1.5e-6 : avec err=384 (= ring full vs setpoint), proportional
 *                  contribution = 5.76e-4 ≈ RATIO_MAX (atteint le clamp
 *                  sur saturation totale, convergence en ~1 sec)
 *   Ki = 1e-9   : intégrale corrige le residual steady-state, monte
 *                  doucement sur erreur soutenue (anti-windup à 1e6).
 */
#define ASRC_KP          (1.5e-6f)
#define ASRC_KI          (1e-9f)

void asrc_init(asrc_t *a, int channels, float setpoint_frames)
{
	if (channels > ASRC_MAX_CH) channels = ASRC_MAX_CH;
	memset(a, 0, sizeof(*a));
	a->channels    = channels;
	a->setpoint    = setpoint_frames;
	a->kp          = ASRC_KP;
	a->ki          = ASRC_KI;
	a->integral_max = 1000000.0f;   /* anti-windup, permet ki d'atteindre ~500 ppm */
	a->ratio       = 1.0f;
	a->ratio_ema   = 1.0f;
	a->ema_alpha   = ASRC_EMA_ALPHA;
	a->pos         = 0.0f;
	a->fade_remaining = 0;
	atomic_store(&a->status, 0);
}

void asrc_reset(asrc_t *a)
{
	for (int ch = 0; ch < a->channels; ch++)
		for (int i = 0; i < 4; i++)
			a->hist[ch][i] = 0.0f;
	a->pos = 0.0f;
	a->fade_remaining = ASRC_FADE_FRAMES;
	/* Keep ratio + integral : continuity in correction across replug */
}

/* Cubic Lagrange 4-tap interpolation.
 * t ∈ [0, 1), samples x0,x1,x2,x3 with x1=present, x2=next.
 *
 *   L0(t) = -t(t-1)(t-2)/6
 *   L1(t) =  (t+1)(t-1)(t-2)/2
 *   L2(t) = -(t+1)t(t-2)/2
 *   L3(t) =  (t+1)t(t-1)/6
 *
 * y(t) = x0*L0 + x1*L1 + x2*L2 + x3*L3
 *
 * For t=0, returns x1 exactly. For t→1, returns x2 exactly.
 * Smooth C¹ between samples.
 */
static inline float cubic_lagrange(float x0, float x1, float x2, float x3, float t)
{
	float tm1 = t - 1.0f;
	float tm2 = t - 2.0f;
	float tp1 = t + 1.0f;

	float L0 = -(t * tm1 * tm2) / 6.0f;
	float L1 =  (tp1 * tm1 * tm2) / 2.0f;
	float L2 = -(tp1 * t   * tm2) / 2.0f;
	float L3 =  (tp1 * t   * tm1) / 6.0f;

	return x0 * L0 + x1 * L1 + x2 * L2 + x3 * L3;
}

int asrc_run(asrc_t *a, const float *in, int in_frames,
             float *out, int out_frames)
{
	int in_consumed = 0;
	float pos = a->pos;
	float step = a->ratio;          /* input frames per output frame */
	int   chs = a->channels;

	for (int o = 0; o < out_frames; o++) {
		/* Advance hist while pos >= 1.0 (= new input sample consumed) */
		while (pos >= 1.0f) {
			if (in_consumed >= in_frames) {
				/* Underflow : input exhausted. Output silence rest. */
				for (int rem = o; rem < out_frames; rem++)
					for (int ch = 0; ch < chs; ch++)
						out[rem * chs + ch] = 0.0f;
				a->pos = pos;
				atomic_fetch_add(&a->frames_in_total, in_consumed);
				atomic_fetch_add(&a->frames_out_total, o);
				return in_consumed;
			}
			/* Shift history : x0 <- x1, x1 <- x2, x2 <- x3, x3 <- new */
			for (int ch = 0; ch < chs; ch++) {
				a->hist[ch][0] = a->hist[ch][1];
				a->hist[ch][1] = a->hist[ch][2];
				a->hist[ch][2] = a->hist[ch][3];
				a->hist[ch][3] = in[in_consumed * chs + ch];
			}
			in_consumed++;
			pos -= 1.0f;
		}

		/* Interpolate at fractional position `pos` between x[1] and x[2] */
		for (int ch = 0; ch < chs; ch++) {
			float y = cubic_lagrange(a->hist[ch][0], a->hist[ch][1],
			                         a->hist[ch][2], a->hist[ch][3], pos);

			/* Crossfade after reset to mask discontinuity */
			if (a->fade_remaining > 0) {
				int n = ASRC_FADE_FRAMES - a->fade_remaining;
				float fade = (float)n / (float)ASRC_FADE_FRAMES;
				y *= fade;
			}
			out[o * chs + ch] = y;
		}
		if (a->fade_remaining > 0) a->fade_remaining--;

		pos += step;
	}

	a->pos = pos;
	atomic_fetch_add(&a->frames_in_total, in_consumed);
	atomic_fetch_add(&a->frames_out_total, out_frames);
	return in_consumed;
}

void asrc_update_pid(asrc_t *a, int ring_fill_frames)
{
	/* Error = setpoint - actual.
	 * If ring too full (host faster than DSP) → err < 0 → ratio > 1
	 *   → consume more input per output → drain ring → equilibrium.
	 * If ring too empty → err > 0 → ratio < 1 → consume less → fill ring.
	 */
	float err = a->setpoint - (float)ring_fill_frames;

	/* Integral with anti-windup */
	a->integral += err;
	if (a->integral >  a->integral_max) a->integral =  a->integral_max;
	if (a->integral < -a->integral_max) a->integral = -a->integral_max;

	/* PI : ratio adjustment is INVERSE of error sign.
	 *   err > 0 (ring underfull) → input too slow → output less per input
	 *                            → ratio < 1 (consume fewer input frames per output)
	 *   err < 0 (ring overfull)  → input too fast → output more per input
	 *                            → ratio > 1
	 */
	float new_ratio = 1.0f - (a->kp * err + a->ki * a->integral);
	int status = 0;
	if (new_ratio < ASRC_RATIO_MIN) { new_ratio = ASRC_RATIO_MIN; status = 2; }
	if (new_ratio > ASRC_RATIO_MAX) { new_ratio = ASRC_RATIO_MAX; status = 2; }
	a->ratio = new_ratio;

	/* EMA for display */
	a->ratio_ema += a->ema_alpha * (new_ratio - a->ratio_ema);

	/* Status if not already clamped */
	if (status == 0) {
		float dppm = fabsf(a->ratio_ema - 1.0f) * 1e6f;
		if (dppm > 100.0f) status = 1;
	}
	atomic_store(&a->status, status);
}

float asrc_drift_ppm(const asrc_t *a)
{
	return (a->ratio_ema - 1.0f) * 1e6f;
}

float asrc_ratio(const asrc_t *a)
{
	return a->ratio;
}
