// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fx_space — reverb Schroeder (4 combs + 2 allpass) + delay stéréo.
 * Code déplacé tel quel depuis effects.c (V14.0 étape 5, extraction pure).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effects.h"
#include "fx_internal.h"

/* ========================================================================
 *   2. REVERB Schroeder — 4 comb filters parallèles + 2 allpass série
 * ====================================================================== */

/* Délais de comb et allpass (samples @ 48 kHz, primes pour réduire artifacts). */
#define COMB_N   4
#define AP_N     2

static const int comb_lens[COMB_N] = { 1557, 1617, 1491, 1422 };  /* ~30 ms */
static const int ap_lens[AP_N]     = {  225,   91 };              /* ~2-5 ms */
#define COMB_MAX 1620
#define AP_MAX   230

struct reverb_state {
	float sr;
	/* params */
	float room_size;    /* 0..1, → feedback comb */
	float damping;      /* 0..1, → lowpass cutoff dans la boucle comb */
	float wet;          /* 0..1, niveau output dry/wet (wet pur) */
	/* state */
	float comb_buf[2][COMB_N][COMB_MAX];   /* L et R */
	int   comb_idx[2][COMB_N];
	float comb_filt[2][COMB_N];            /* état lowpass dans la boucle */
	float ap_buf[2][AP_N][AP_MAX];
	int   ap_idx[2][AP_N];
};

/* V9.3 : process_block. Le reverb a des feedback loops avec dépendances
 * sample-par-sample (impossible à vectoriser), donc la loop interne reste
 * sample. Gain : amortir overhead vtable (1 call vs N) + locality cache. */
static void reverb_process_block(fx_engine_t *fx,
				 const float *in_l, const float *in_r,
				 float *out_l, float *out_r,
				 uint32_t N)
{
	struct reverb_state *r = fx->state;
	const float feedback = 0.28f + r->room_size * 0.7f;
	const float damp1 = r->damping * 0.4f;
	const float damp2 = 1.0f - damp1;
	const float wet = r->wet;

	for (uint32_t s = 0; s < N; s++) {
		float xl = in_l[s], xr = in_r[s];
		float comb_out_l = 0.0f, comb_out_r = 0.0f;

		for (int c = 0; c < COMB_N; c++) {
			int n = comb_lens[c];
			int i = r->comb_idx[0][c];
			float v = r->comb_buf[0][c][i];
			r->comb_filt[0][c] = v * damp2 + r->comb_filt[0][c] * damp1;
			r->comb_buf[0][c][i] = xl + r->comb_filt[0][c] * feedback;
			r->comb_idx[0][c] = (i + 1) % n;
			comb_out_l += v;
			i = r->comb_idx[1][c];
			v = r->comb_buf[1][c][i];
			r->comb_filt[1][c] = v * damp2 + r->comb_filt[1][c] * damp1;
			r->comb_buf[1][c][i] = xr + r->comb_filt[1][c] * feedback;
			r->comb_idx[1][c] = (i + 1) % n;
			comb_out_r += v;
		}
		float ap_l = comb_out_l;
		float ap_r = comb_out_r;
		for (int a = 0; a < AP_N; a++) {
			int n = ap_lens[a];
			int i = r->ap_idx[0][a];
			float bufout = r->ap_buf[0][a][i];
			float input  = ap_l;
			r->ap_buf[0][a][i] = input + bufout * 0.5f;
			ap_l = bufout - input;
			r->ap_idx[0][a] = (i + 1) % n;
			i = r->ap_idx[1][a];
			bufout = r->ap_buf[1][a][i];
			input  = ap_r;
			r->ap_buf[1][a][i] = input + bufout * 0.5f;
			ap_r = bufout - input;
			r->ap_idx[1][a] = (i + 1) % n;
		}
		out_l[s] = ap_l * wet;
		out_r[s] = ap_r * wet;
	}
}

static int reverb_set_param(fx_engine_t *fx, const char *name, float value)
{
	struct reverb_state *r = fx->state;
	if (!strcmp(name, "room_size")) r->room_size = CLAMP(value, 0.0f, 1.0f);
	else if (!strcmp(name, "damping")) r->damping = CLAMP(value, 0.0f, 1.0f);
	else if (!strcmp(name, "wet"))     r->wet = CLAMP(value, 0.0f, 1.0f);
	else return -1;
	return 0;
}

static void reverb_reset(fx_engine_t *fx)
{
	struct reverb_state *r = fx->state;
	memset(r->comb_buf,  0, sizeof(r->comb_buf));
	memset(r->ap_buf,    0, sizeof(r->ap_buf));
	memset(r->comb_filt, 0, sizeof(r->comb_filt));
}

static int reverb_get_state(fx_engine_t *fx, char *buf, int len)
{
	struct reverb_state *r = fx->state;
	return snprintf(buf, len,
		"\"type\":\"reverb\",\"room_size\":%.3f,\"damping\":%.3f,\"wet\":%.3f",
		r->room_size, r->damping, r->wet);
}

int fx_init_reverb(fx_engine_t *fx, float sample_rate)
{
	struct reverb_state *r = calloc(1, sizeof(*r));
	if (!r) return 0;
	r->sr = sample_rate;
	r->room_size = 0.5f;
	r->damping = 0.5f;
	r->wet = 0.5f;

	fx->type_name = "reverb";
	fx->state = r;
	fx->process_block = reverb_process_block;
	fx->set_param = reverb_set_param;
	fx->reset = reverb_reset;
	fx->get_state = reverb_get_state;
	return 1;
}


/* ========================================================================
 *   3. DELAY — ligne à retard stéréo + feedback
 * ====================================================================== */

#define DELAY_MAX_MS    1000.0f
#define DELAY_MAX_SAMP  48000   /* 1 s @ 48 kHz */

struct delay_state {
	float sr;
	float delay_ms;
	float feedback;
	float wet;
	int   delay_samples;
	float buf_l[DELAY_MAX_SAMP];
	float buf_r[DELAY_MAX_SAMP];
	int   widx;
};

static void delay_recalc(struct delay_state *d)
{
	int s = (int)(d->delay_ms * 0.001f * d->sr);
	if (s < 1) s = 1;
	if (s >= DELAY_MAX_SAMP) s = DELAY_MAX_SAMP - 1;
	d->delay_samples = s;
}

/* V9.3 : process_block delay (ring buffer non vectorisable due au feedback) */
static void delay_process_block(fx_engine_t *fx,
				const float *in_l, const float *in_r,
				float *out_l, float *out_r,
				uint32_t N)
{
	struct delay_state *d = fx->state;
	const float fb = d->feedback;
	const float wet = d->wet;
	const int dsamp = d->delay_samples;
	int widx = d->widx;
	for (uint32_t s = 0; s < N; s++) {
		int ridx = widx - dsamp;
		if (ridx < 0) ridx += DELAY_MAX_SAMP;
		float dl = d->buf_l[ridx];
		float dr = d->buf_r[ridx];
		d->buf_l[widx] = in_l[s] + dl * fb;
		d->buf_r[widx] = in_r[s] + dr * fb;
		widx++;
		if (widx >= DELAY_MAX_SAMP) widx = 0;
		out_l[s] = dl * wet;
		out_r[s] = dr * wet;
	}
	d->widx = widx;
}

static int delay_set_param(fx_engine_t *fx, const char *name, float value)
{
	struct delay_state *d = fx->state;
	if (!strcmp(name, "delay_ms")) {
		d->delay_ms = CLAMP(value, 1.0f, DELAY_MAX_MS);
		delay_recalc(d);
	}
	else if (!strcmp(name, "feedback")) d->feedback = CLAMP(value, 0.0f, 0.95f);
	else if (!strcmp(name, "wet"))      d->wet = CLAMP(value, 0.0f, 1.0f);
	else return -1;
	return 0;
}

static void delay_reset(fx_engine_t *fx)
{
	struct delay_state *d = fx->state;
	memset(d->buf_l, 0, sizeof(d->buf_l));
	memset(d->buf_r, 0, sizeof(d->buf_r));
	d->widx = 0;
}

static int delay_get_state(fx_engine_t *fx, char *buf, int len)
{
	struct delay_state *d = fx->state;
	return snprintf(buf, len,
		"\"type\":\"delay\",\"delay_ms\":%.2f,\"feedback\":%.3f,\"wet\":%.3f",
		d->delay_ms, d->feedback, d->wet);
}

int fx_init_delay(fx_engine_t *fx, float sample_rate)
{
	struct delay_state *d = calloc(1, sizeof(*d));
	if (!d) return 0;
	d->sr = sample_rate;
	d->delay_ms = 250.0f;
	d->feedback = 0.4f;
	d->wet = 0.5f;
	delay_recalc(d);

	fx->type_name = "delay";
	fx->state = d;
	fx->process_block = delay_process_block;
	fx->set_param = delay_set_param;
	fx->reset = delay_reset;
	fx->get_state = delay_get_state;
	return 1;
}

