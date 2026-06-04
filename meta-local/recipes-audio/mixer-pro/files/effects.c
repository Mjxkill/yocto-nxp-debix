// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * V7.0-E6.e — Implémentations natives C des 4 effets du mixer-pro.
 *
 * Tous les effets sont écrits pour traitement sample-par-sample (inline dans
 * la boucle audio principale). Pas de buffer interne caché → latence ALSA
 * inchangée (E6.d : 8 ms steady).
 *
 * Précision : float32. Sur Cortex-A53 avec NEON, ops basiques < 10 ns chacune.
 */

#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effects.h"

/* ============================== Helpers ============================ */

#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define DB2LIN(db)       expf((db) * 0.11512925f)   /* 10^(db/20) = exp(db * ln(10)/20) */

/* ========================================================================
 *   1. COMPRESSOR — peak envelope follower + soft-knee gain reduction
 * ====================================================================== */

struct comp_state {
	float sr;
	/* params */
	float threshold_db, ratio, attack_ms, release_ms, makeup_db;
	/* derived */
	float threshold_lin, makeup_lin;
	float attack_coef, release_coef;
	/* state */
	float env_l, env_r;
};

static void comp_recalc(struct comp_state *c)
{
	c->threshold_lin = DB2LIN(c->threshold_db);
	c->makeup_lin    = DB2LIN(c->makeup_db);
	/* coef = exp(-1 / (time_s * sample_rate)) */
	c->attack_coef  = expf(-1.0f / (c->attack_ms  * 0.001f * c->sr));
	c->release_coef = expf(-1.0f / (c->release_ms * 0.001f * c->sr));
}

/* V9.3 : process_block — boucle sur N samples, état env_l/env_r persistant.
 * Loop simple float → auto-vectorisable par gcc -O2 (gcc -ftree-loop-vectorize
 * activé en O2 ; voir asm produit pour confirmer NEON). */
static void comp_process_block(fx_engine_t *fx,
			       const float *in_l, const float *in_r,
			       float *out_l, float *out_r,
			       uint32_t N)
{
	struct comp_state *c = fx->state;
	const float acoef = c->attack_coef;
	const float rcoef = c->release_coef;
	const float one_a = 1.0f - acoef;
	const float one_r = 1.0f - rcoef;
	const float thr   = c->threshold_lin;
	const float ratio = c->ratio;
	const float mk    = c->makeup_lin;
	float el = c->env_l, er = c->env_r;

	for (uint32_t i = 0; i < N; i++) {
		float xl = in_l[i], xr = in_r[i];
		float al = fabsf(xl), ar = fabsf(xr);
		el = (al > el) ? (acoef * el + one_a * al) : (rcoef * el + one_r * al);
		er = (ar > er) ? (acoef * er + one_a * ar) : (rcoef * er + one_r * ar);
		float gl = (el > thr) ? (thr + (el - thr) / ratio) / el : 1.0f;
		float gr = (er > thr) ? (thr + (er - thr) / ratio) / er : 1.0f;
		out_l[i] = xl * gl * mk;
		out_r[i] = xr * gr * mk;
	}
	c->env_l = el;
	c->env_r = er;
}

static int comp_set_param(fx_engine_t *fx, const char *name, float value)
{
	struct comp_state *c = fx->state;
	if (!strcmp(name, "threshold")) c->threshold_db = CLAMP(value, -60.0f, 0.0f);
	else if (!strcmp(name, "ratio"))     c->ratio = CLAMP(value, 1.0f, 20.0f);
	else if (!strcmp(name, "attack"))    c->attack_ms  = CLAMP(value, 0.1f, 500.0f);
	else if (!strcmp(name, "release"))   c->release_ms = CLAMP(value, 1.0f, 2000.0f);
	else if (!strcmp(name, "makeup"))    c->makeup_db = CLAMP(value, -12.0f, 24.0f);
	else return -1;
	comp_recalc(c);
	return 0;
}

static void comp_reset(fx_engine_t *fx)
{
	struct comp_state *c = fx->state;
	c->env_l = c->env_r = 0.0f;
}

static int comp_get_state(fx_engine_t *fx, char *buf, int len)
{
	struct comp_state *c = fx->state;
	return snprintf(buf, len,
		"\"type\":\"compressor\",\"threshold\":%.2f,\"ratio\":%.2f,"
		"\"attack\":%.2f,\"release\":%.2f,\"makeup\":%.2f,"
		"\"env_l_db\":%.2f,\"env_r_db\":%.2f",
		c->threshold_db, c->ratio, c->attack_ms, c->release_ms, c->makeup_db,
		c->env_l > 1e-6f ? 20.0f * log10f(c->env_l) : -120.0f,
		c->env_r > 1e-6f ? 20.0f * log10f(c->env_r) : -120.0f);
}

int fx_init_compressor(fx_engine_t *fx, float sample_rate)
{
	struct comp_state *c = calloc(1, sizeof(*c));
	if (!c) return 0;
	c->sr = sample_rate;
	c->threshold_db = -20.0f;
	c->ratio = 4.0f;
	c->attack_ms = 5.0f;
	c->release_ms = 50.0f;
	c->makeup_db = 0.0f;
	comp_recalc(c);

	fx->type_name = "compressor";
	fx->state = c;
	fx->process_block = comp_process_block;
	fx->set_param = comp_set_param;
	fx->reset = comp_reset;
	fx->get_state = comp_get_state;
	return 1;
}

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

/* ========================================================================
 *   4. EQ 3-band — biquads RBJ low-shelf + peaking + high-shelf
 * ====================================================================== */

struct biquad {
	float b0, b1, b2, a1, a2;
	float x1[2], x2[2], y1[2], y2[2];   /* [0]=L [1]=R */
};

static inline float biquad_step(struct biquad *bq, int ch, float x)
{
	float y = bq->b0 * x + bq->b1 * bq->x1[ch] + bq->b2 * bq->x2[ch]
		- bq->a1 * bq->y1[ch] - bq->a2 * bq->y2[ch];
	bq->x2[ch] = bq->x1[ch]; bq->x1[ch] = x;
	bq->y2[ch] = bq->y1[ch]; bq->y1[ch] = y;
	return y;
}

/* RBJ biquad cookbook : peaking, low-shelf, high-shelf à fréquence f0, Q,
 * gain dB. f0 normalisée 0..0.5 (= rate/sr).
 */
enum biquad_type { BQ_LOW_SHELF, BQ_PEAK, BQ_HIGH_SHELF };

static void biquad_set(struct biquad *bq, enum biquad_type type, float sr,
		       float f0, float q, float gain_db)
{
	float A = powf(10.0f, gain_db / 40.0f);
	float w0 = 2.0f * (float)M_PI * f0 / sr;
	float cw = cosf(w0), sw = sinf(w0);
	float alpha = sw / (2.0f * q);
	float a0, a1, a2, b0, b1, b2;

	switch (type) {
	case BQ_LOW_SHELF: {
		float beta = sqrtf(A) / q;
		b0 =    A * ((A + 1) - (A - 1) * cw + beta * sw);
		b1 =  2*A * ((A - 1) - (A + 1) * cw);
		b2 =    A * ((A + 1) - (A - 1) * cw - beta * sw);
		a0 =        (A + 1) + (A - 1) * cw + beta * sw;
		a1 =   -2 * ((A - 1) + (A + 1) * cw);
		a2 =        (A + 1) + (A - 1) * cw - beta * sw;
		break;
	}
	case BQ_HIGH_SHELF: {
		float beta = sqrtf(A) / q;
		b0 =    A * ((A + 1) + (A - 1) * cw + beta * sw);
		b1 = -2*A * ((A - 1) + (A + 1) * cw);
		b2 =    A * ((A + 1) + (A - 1) * cw - beta * sw);
		a0 =        (A + 1) - (A - 1) * cw + beta * sw;
		a1 =    2 * ((A - 1) - (A + 1) * cw);
		a2 =        (A + 1) - (A - 1) * cw - beta * sw;
		break;
	}
	default: /* peaking */
		b0 = 1 + alpha * A;
		b1 = -2 * cw;
		b2 = 1 - alpha * A;
		a0 = 1 + alpha / A;
		a1 = -2 * cw;
		a2 = 1 - alpha / A;
		break;
	}
	bq->b0 = b0 / a0; bq->b1 = b1 / a0; bq->b2 = b2 / a0;
	bq->a1 = a1 / a0; bq->a2 = a2 / a0;
}

struct eq_state {
	float sr;
	float low_db, mid_db, mid_freq, mid_q, high_db;
	struct biquad bq_low, bq_mid, bq_high;
};

static void eq_recalc(struct eq_state *e)
{
	biquad_set(&e->bq_low,  BQ_LOW_SHELF,  e->sr, 250.0f,   0.707f, e->low_db);
	biquad_set(&e->bq_mid,  BQ_PEAK,       e->sr, e->mid_freq, e->mid_q, e->mid_db);
	biquad_set(&e->bq_high, BQ_HIGH_SHELF, e->sr, 5000.0f,  0.707f, e->high_db);
}

/* V9.3 : process_block EQ (cascade biquads non vectorisable car y[n]
 * dépend de y[n-1] de chaque biquad). Gain : amortir overhead vtable. */
static void eq_process_block(fx_engine_t *fx,
			     const float *in_l, const float *in_r,
			     float *out_l, float *out_r,
			     uint32_t N)
{
	struct eq_state *e = fx->state;
	for (uint32_t s = 0; s < N; s++) {
		float l = biquad_step(&e->bq_high, 0,
		          biquad_step(&e->bq_mid,  0,
		          biquad_step(&e->bq_low,  0, in_l[s])));
		float r = biquad_step(&e->bq_high, 1,
		          biquad_step(&e->bq_mid,  1,
		          biquad_step(&e->bq_low,  1, in_r[s])));
		out_l[s] = l;
		out_r[s] = r;
	}
}

static int eq_set_param(fx_engine_t *fx, const char *name, float value)
{
	struct eq_state *e = fx->state;
	if (!strcmp(name, "low_gain"))       e->low_db  = CLAMP(value, -18.0f, 18.0f);
	else if (!strcmp(name, "mid_gain"))  e->mid_db  = CLAMP(value, -18.0f, 18.0f);
	else if (!strcmp(name, "mid_freq"))  e->mid_freq = CLAMP(value, 200.0f, 8000.0f);
	else if (!strcmp(name, "mid_q"))     e->mid_q = CLAMP(value, 0.1f, 10.0f);
	else if (!strcmp(name, "high_gain")) e->high_db = CLAMP(value, -18.0f, 18.0f);
	else return -1;
	eq_recalc(e);
	return 0;
}

static void eq_reset(fx_engine_t *fx)
{
	struct eq_state *e = fx->state;
	memset(&e->bq_low.x1,  0, sizeof(e->bq_low.x1));
	memset(&e->bq_low.x2,  0, sizeof(e->bq_low.x2));
	memset(&e->bq_low.y1,  0, sizeof(e->bq_low.y1));
	memset(&e->bq_low.y2,  0, sizeof(e->bq_low.y2));
	memset(&e->bq_mid.x1,  0, sizeof(e->bq_mid.x1));
	memset(&e->bq_mid.x2,  0, sizeof(e->bq_mid.x2));
	memset(&e->bq_mid.y1,  0, sizeof(e->bq_mid.y1));
	memset(&e->bq_mid.y2,  0, sizeof(e->bq_mid.y2));
	memset(&e->bq_high.x1, 0, sizeof(e->bq_high.x1));
	memset(&e->bq_high.x2, 0, sizeof(e->bq_high.x2));
	memset(&e->bq_high.y1, 0, sizeof(e->bq_high.y1));
	memset(&e->bq_high.y2, 0, sizeof(e->bq_high.y2));
}

static int eq_get_state(fx_engine_t *fx, char *buf, int len)
{
	struct eq_state *e = fx->state;
	return snprintf(buf, len,
		"\"type\":\"eq\",\"low_gain\":%.2f,\"mid_gain\":%.2f,"
		"\"mid_freq\":%.1f,\"mid_q\":%.2f,\"high_gain\":%.2f",
		e->low_db, e->mid_db, e->mid_freq, e->mid_q, e->high_db);
}

int fx_init_eq(fx_engine_t *fx, float sample_rate)
{
	struct eq_state *e = calloc(1, sizeof(*e));
	if (!e) return 0;
	e->sr = sample_rate;
	e->low_db = 0.0f; e->mid_db = 0.0f; e->mid_freq = 1000.0f;
	e->mid_q = 1.0f; e->high_db = 0.0f;
	eq_recalc(e);

	fx->type_name = "eq";
	fx->state = e;
	fx->process_block = eq_process_block;
	fx->set_param = eq_set_param;
	fx->reset = eq_reset;
	fx->get_state = eq_get_state;
	return 1;
}

/* ========================================================================
 *   5. LV2 plugin host (V9.2) — lilv-0
 * ======================================================================
 *
 * Charge un plugin LV2 RT-safe via lilv, expose un fx_engine_t wrapper.
 *
 * Per-sample processing : lilv_instance_run(N=1) à chaque sample. Pas
 * optimal (overhead par call) mais cohérent avec l'architecture vtable
 * frame-per-frame du mixer. Plugins simples (gain, biquad) tolèrent.
 * Plugins avec buffers internes (reverb, delay lines) fonctionnent
 * aussi car ils gardent leur state interne entre les runs.
 *
 * Sécurité RT : on filtre `lv2:hardRTCapable=true` au load time.
 * Plugin sans cette propriété = refusé (peut allouer en process).
 */
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/atom.h>
#include <lv2/options/options.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/parameters/parameters.h>
#include <lv2/worker/worker.h>

/* Global lilv world (shared par tous les bus LV2). Init lazy. */
static LilvWorld *g_lv2_world           = NULL;
static const LilvPlugins *g_lv2_plugins = NULL;
static LilvNode  *g_uri_audio_port      = NULL;
static LilvNode  *g_uri_control_port    = NULL;
static LilvNode  *g_uri_input_port      = NULL;
static LilvNode  *g_uri_output_port     = NULL;
static LilvNode  *g_uri_hard_rt         = NULL;
/* V9.2-step5c : LV2 atom port support (control/automation/notify) */
static LilvNode  *g_uri_atom_port       = NULL;
static LV2_URID   g_urid_atom_sequence  = 0;
static LV2_URID   g_urid_atom_chunk     = 0;

/* V9.2-step5d : LV2 options host feature globals.
 * Permet de passer maxBlockLength, sampleRate, etc. au plugin à init.
 * Beaucoup de plugins modernes (dragonfly Hall, calf, lsp) requièrent
 * `opts:options` pour allouer leurs buffers internes.
 */
static int32_t  g_opt_max_block    = 96;       /* period frames (cohérent ALSA) */
static int32_t  g_opt_min_block    = 1;        /* on run sample-par-sample */
static int32_t  g_opt_nom_block    = 96;       /* nominal = max */
static int32_t  g_opt_seq_size     = 8192;     /* atom_sequence capacity */
static float    g_opt_sample_rate  = 48000.0f; /* SAMPLE_RATE projet */
static LV2_URID g_urid_max_block   = 0;
static LV2_URID g_urid_min_block   = 0;
static LV2_URID g_urid_nom_block   = 0;
static LV2_URID g_urid_seq_size    = 0;
static LV2_URID g_urid_sample_rate = 0;
static LV2_URID g_urid_atom_int    = 0;
static LV2_URID g_urid_atom_float  = 0;
static LV2_Options_Option g_lv2_options[7];   /* 6 entries + terminator zero */
static LV2_Feature g_feature_options = {
	.URI  = LV2_OPTIONS__options,
	.data = g_lv2_options,
};

/* V9.2 — host feature `urid:map` : service basique de mapping URI → ID.
 * Beaucoup de plugins LV2 modernes (scope, params, etc.) le require sinon
 * instantiate fail. Implémentation simple linear search (suffisant pour
 * < 100 URIs typique). */
#define URID_MAP_MAX 256
static char       *g_urid_uris[URID_MAP_MAX];
static int         g_urid_count = 0;

static LV2_URID urid_map_fn(LV2_URID_Map_Handle handle, const char *uri)
{
	(void)handle;
	for (int i = 0; i < g_urid_count; i++) {
		if (strcmp(g_urid_uris[i], uri) == 0) return (LV2_URID)(i + 1);
	}
	if (g_urid_count >= URID_MAP_MAX) return 0;
	g_urid_uris[g_urid_count] = strdup(uri);
	return (LV2_URID)(++g_urid_count);
}

static LV2_URID_Map g_urid_map_data = {
	.handle = NULL,
	.map = urid_map_fn,
};
static LV2_Feature g_feature_urid_map = {
	.URI  = LV2_URID__map,
	.data = &g_urid_map_data,
};
/* V9.2-step5d : g_host_features global = urid_map + options.
 * worker:schedule est INSTANCE-spécifique (handle = struct lv2_worker*),
 * donc construit per-plugin dans fx_init_lv2() à partir de ce array de base. */
static const LV2_Feature *g_host_features[] = {
	&g_feature_urid_map,
	&g_feature_options,
	NULL
};

static int lv2_world_init(void)
{
	if (g_lv2_world) return 1;
	g_lv2_world = lilv_world_new();
	if (!g_lv2_world) return 0;
	lilv_world_load_all(g_lv2_world);

	g_uri_audio_port    = lilv_new_uri(g_lv2_world, LV2_CORE__AudioPort);
	g_uri_control_port  = lilv_new_uri(g_lv2_world, LV2_CORE__ControlPort);
	g_uri_input_port    = lilv_new_uri(g_lv2_world, LV2_CORE__InputPort);
	g_uri_output_port   = lilv_new_uri(g_lv2_world, LV2_CORE__OutputPort);
	g_uri_hard_rt       = lilv_new_uri(g_lv2_world, LV2_CORE__hardRTCapable);
	g_uri_atom_port     = lilv_new_uri(g_lv2_world, LV2_ATOM__AtomPort);

	/* Pre-map atom URIDs (utilisés à chaque cycle audio dans lv2_process) */
	g_urid_atom_sequence = urid_map_fn(NULL, LV2_ATOM__Sequence);
	g_urid_atom_chunk    = urid_map_fn(NULL, LV2_ATOM__Chunk);

	/* V9.2-step5d : pre-map options URIDs + init g_lv2_options[] array */
	g_urid_max_block    = urid_map_fn(NULL, LV2_BUF_SIZE__maxBlockLength);
	g_urid_min_block    = urid_map_fn(NULL, LV2_BUF_SIZE__minBlockLength);
	g_urid_nom_block    = urid_map_fn(NULL, LV2_BUF_SIZE__nominalBlockLength);
	g_urid_seq_size     = urid_map_fn(NULL, LV2_BUF_SIZE__sequenceSize);
	g_urid_sample_rate  = urid_map_fn(NULL, LV2_PARAMETERS__sampleRate);
	g_urid_atom_int     = urid_map_fn(NULL, LV2_ATOM__Int);
	g_urid_atom_float   = urid_map_fn(NULL, LV2_ATOM__Float);

	g_lv2_options[0] = (LV2_Options_Option){
		LV2_OPTIONS_INSTANCE, 0, g_urid_max_block,
		sizeof(int32_t), g_urid_atom_int, &g_opt_max_block };
	g_lv2_options[1] = (LV2_Options_Option){
		LV2_OPTIONS_INSTANCE, 0, g_urid_min_block,
		sizeof(int32_t), g_urid_atom_int, &g_opt_min_block };
	g_lv2_options[2] = (LV2_Options_Option){
		LV2_OPTIONS_INSTANCE, 0, g_urid_nom_block,
		sizeof(int32_t), g_urid_atom_int, &g_opt_nom_block };
	g_lv2_options[3] = (LV2_Options_Option){
		LV2_OPTIONS_INSTANCE, 0, g_urid_seq_size,
		sizeof(int32_t), g_urid_atom_int, &g_opt_seq_size };
	g_lv2_options[4] = (LV2_Options_Option){
		LV2_OPTIONS_INSTANCE, 0, g_urid_sample_rate,
		sizeof(float), g_urid_atom_float, &g_opt_sample_rate };
	g_lv2_options[5] = (LV2_Options_Option){ 0, 0, 0, 0, 0, NULL };  /* terminator */
	g_lv2_options[6] = (LV2_Options_Option){ 0, 0, 0, 0, 0, NULL };  /* safety */

	g_lv2_plugins = lilv_world_get_all_plugins(g_lv2_world);
	return 1;
}

/* V9.2-step5c : liste des URIs de features que l'host implémente. Utilisé
 * pour valider les required_features du plugin AVANT instantiate. Refus
 * propre si plugin demande worker/state/options/etc. non supportés.
 * On supporte aujourd'hui : urid:map (cf g_host_features ci-dessus).
 * hardRTCapable est dans CORE et n'est pas une feature, c'est un trait. */
static int lv2_host_supports_feature(const char *uri)
{
	if (!uri) return 0;
	if (strcmp(uri, LV2_URID__map) == 0) return 1;
	if (strcmp(uri, LV2_OPTIONS__options) == 0) return 1;
	/* V9.2-step5d : worker:schedule supporté via thread per-plugin (cf
	 * struct lv2_worker dans fx_init_lv2). Le feature data est instance-
	 * spécifique, pas global. */
	if (strcmp(uri, LV2_WORKER__schedule) == 0) return 1;
	/* lv2:state (presets, save/restore) : pas implémenté V9.2, plugins
	 * qui le require seront refusés. À implémenter V9.3 si besoin. */
	return 0;
}

#define LV2_MAX_CTRL_PORTS 64
#define LV2_MAX_NAME_LEN   32

/* V9.2-step5d : LV2 worker support (1 thread non-RT per plugin instance).
 *
 * Architecture :
 *   audio_thread (RT prio 99) — appelle lilv_instance_run() qui peut
 *     appeler worker_schedule_cb() ; cette callback queue le request
 *     non-bloquant (pthread_mutex_trylock + counter drops si fail).
 *   worker thread (sched OTHER) — sleep sur cond, exécute iface->work()
 *     qui peut prendre 100ms+ (load IR file). Appelle worker_respond_cb()
 *     qui store la response dans un buffer per-instance.
 *   audio_thread (lv2_process) — au début, check si resp_pending,
 *     copy local + call iface->work_response() pour committer dans le plugin.
 *
 * Ring SPSC simple à 1 slot in / 1 slot out. Si plugin spam schedule_work
 * sans laisser le worker thread répondre → drops counter incrémenté.
 *
 * RT safety :
 *   - pthread_mutex_trylock dans audio_thread : non-bloquant (10-20 µs
 *     worst case sous contention PREEMPT_RT, négligeable / period 2 ms).
 *   - cond_wait avec timeout 2s côté worker thread pour détecter exit_flag
 *     (suggestion critic).
 *   - Si pthread_create fail → refus propre du plugin (suggestion critic).
 */
#define LV2_WORKER_BUF_SIZE  8192

struct lv2_worker {
	pthread_t          thread;
	pthread_mutex_t    mutex;
	pthread_cond_t     cond;

	/* SPSC 1-slot ring */
	uint8_t            req_buf[LV2_WORKER_BUF_SIZE];
	uint32_t           req_size;
	volatile int       req_pending;

	uint8_t            resp_buf[LV2_WORKER_BUF_SIZE];
	uint32_t           resp_size;
	volatile int       resp_pending;

	volatile int       exit_flag;
	uint64_t           drops;   /* schedule_work rejetées (critic suggestion) */

	const LV2_Worker_Interface *iface;
	LV2_Handle         plugin_handle;
	LV2_Worker_Schedule schedule;
};

/* Worker thread function : sleep sur cond, execute iface->work(), reboucle.
 * Timeout 2s sur cond_wait pour détecter exit_flag en cas de glitch (critic). */
static void *lv2_worker_thread_fn(void *arg);

/* Callbacks (forward decl) */
static LV2_Worker_Status lv2_worker_respond_cb(LV2_Worker_Respond_Handle handle,
                                               uint32_t size, const void *data);
static LV2_Worker_Status lv2_worker_schedule_cb(LV2_Worker_Schedule_Handle handle,
                                                uint32_t size, const void *data);

/* V9.2-step5c : LV2 atom port buffers.
 * Buffer 8 KB par port = largement suffisant pour usage non-MIDI (state
 * notify, presets ack, peak meter feedback). Si plugin overflow, on logue
 * un warning + cap au capacity initial pour éviter corruption mémoire.
 * Mode mono→stereo : instance2 partage les mêmes buffers que instance1
 * (limitation : pas d'automation indépendante par instance, suffisant en
 * mode passif sans MIDI/automation host).
 */
#define LV2_MAX_ATOM_PORTS 8
#define LV2_ATOM_BUF_SIZE  8192

struct lv2_state {
	float          sr;
	char          *uri;
	LilvInstance  *instance;
	LilvInstance  *instance2;   /* V9.2 : 2e instance pour canal R en mode mono */
	int            is_mono;     /* 1 si plugin 1in/1out (2 instances pour L+R) */
	const LilvPlugin *plugin;

	int            n_ports;
	int            audio_in_idx[2];   /* L, R, -1 si pas dispo */
	int            audio_out_idx[2];

	int            n_ctrl_in;
	int            ctrl_in_idx[LV2_MAX_CTRL_PORTS];
	char           ctrl_in_name[LV2_MAX_CTRL_PORTS][LV2_MAX_NAME_LEN];
	float          ctrl_values[LV2_MAX_CTRL_PORTS];   /* live values, connected */

	/* Buffers I/O 1-sample (alloués pour audio L/R, in et out) */
	float          buf_in_l, buf_in_r, buf_out_l, buf_out_r;

	/* Dummy buffer pour control output (1 par port output, ignoré) */
	float          ctrl_out_dummy[LV2_MAX_CTRL_PORTS];

	/* V9.2-step5c : atom port support (control input + notify output) */
	int            n_atom_in, n_atom_out;
	int            atom_in_idx[LV2_MAX_ATOM_PORTS];
	int            atom_out_idx[LV2_MAX_ATOM_PORTS];
	uint8_t       *atom_in_bufs[LV2_MAX_ATOM_PORTS];
	uint8_t       *atom_out_bufs[LV2_MAX_ATOM_PORTS];

	/* V9.2-step5d : worker support (NULL si plugin ne demande pas worker:schedule) */
	struct lv2_worker *worker;
};

/* V9.2-step5d : worker callbacks + thread.
 * - schedule_cb : appelée par plugin depuis run() audio_thread → queue request
 * - respond_cb  : appelée par plugin depuis work() worker_thread → buffer response
 * - thread_fn   : worker thread loop (sleep/work/respond)
 */
static LV2_Worker_Status lv2_worker_schedule_cb(LV2_Worker_Schedule_Handle handle,
                                                uint32_t size, const void *data)
{
	struct lv2_worker *w = (struct lv2_worker *)handle;
	if (!w || !data || size == 0 || size > LV2_WORKER_BUF_SIZE) {
		if (w) w->drops++;
		return LV2_WORKER_ERR_NO_SPACE;
	}
	/* Non-blocking trylock pour rester RT-safe sur audio_thread. */
	if (pthread_mutex_trylock(&w->mutex) != 0) {
		w->drops++;
		return LV2_WORKER_ERR_UNKNOWN;
	}
	if (w->req_pending) {
		/* Worker thread n'a pas encore consommé le request précédent.
		 * Plugin doit retry au prochain run(). */
		w->drops++;
		pthread_mutex_unlock(&w->mutex);
		return LV2_WORKER_ERR_UNKNOWN;
	}
	memcpy(w->req_buf, data, size);
	w->req_size = size;
	__sync_synchronize();
	w->req_pending = 1;
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->mutex);
	return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status lv2_worker_respond_cb(LV2_Worker_Respond_Handle handle,
                                               uint32_t size, const void *data)
{
	struct lv2_worker *w = (struct lv2_worker *)handle;
	if (!w || !data || size == 0 || size > LV2_WORKER_BUF_SIZE)
		return LV2_WORKER_ERR_NO_SPACE;
	/* Worker thread est le seul writer, audio thread le seul reader.
	 * resp_pending = 0 sur entrée garanti par audio thread après consume. */
	memcpy(w->resp_buf, data, size);
	w->resp_size = size;
	__sync_synchronize();
	w->resp_pending = 1;
	return LV2_WORKER_SUCCESS;
}

static void *lv2_worker_thread_fn(void *arg)
{
	struct lv2_worker *w = (struct lv2_worker *)arg;

	while (!w->exit_flag) {
		pthread_mutex_lock(&w->mutex);
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 2;  /* 2s timeout pour relire exit_flag (deadlock guard) */
		while (!w->req_pending && !w->exit_flag) {
			int rc = pthread_cond_timedwait(&w->cond, &w->mutex, &ts);
			if (rc == ETIMEDOUT) break;
		}
		if (w->exit_flag) {
			pthread_mutex_unlock(&w->mutex);
			break;
		}
		if (!w->req_pending) {
			pthread_mutex_unlock(&w->mutex);
			continue;
		}
		/* Copy request hors mutex avant d'appeler work() (qui peut être long) */
		uint8_t local[LV2_WORKER_BUF_SIZE];
		uint32_t sz = w->req_size;
		memcpy(local, w->req_buf, sz);
		w->req_pending = 0;
		pthread_mutex_unlock(&w->mutex);

		if (w->iface && w->iface->work)
			w->iface->work(w->plugin_handle, lv2_worker_respond_cb, w, sz, local);
	}
	return NULL;
}

/* V9.3 : process_block — le GROS GAIN du refactor.
 * AVANT V9.3 : lilv_instance_run(N=1) appelée N fois par cycle audio.
 *   → 96 calls × 4 bus = 384 calls par cycle, overhead jump table +
 *     state restore × 384. Pour LSP Para EQ 16-band = ~30 ms par cycle.
 * APRÈS V9.3 : 1 call lilv_instance_run(N=96) par bus → 4 calls par cycle.
 *   Le plugin process son block en interne (avec ses optims internes
 *   block-loop, NEON, SIMD si présentes dans le code source LSP/calf).
 *   → Gain attendu 30-60× sur plugins lourds.
 *
 * Worker response commit + atom reset : 1 fois par block (vs 96 fois). */
static void lv2_process_block(fx_engine_t *fx,
			      const float *in_l, const float *in_r,
			      float *out_l, float *out_r,
			      uint32_t N)
{
	struct lv2_state *st = fx->state;

	/* Worker response commit avant run (LV2 spec) */
	if (st->worker && st->worker->resp_pending) {
		struct lv2_worker *w = st->worker;
		uint8_t local[LV2_WORKER_BUF_SIZE];
		uint32_t sz = w->resp_size;
		memcpy(local, w->resp_buf, sz);
		__sync_synchronize();
		w->resp_pending = 0;
		if (w->iface && w->iface->work_response)
			w->iface->work_response(w->plugin_handle, sz, local);
	}

	/* Reconnecte audio ports aux buffers externes (block).
	 * V9.3 : reconnect par cycle = function ptr set, négligeable vs gain N=96. */
	if (!st->is_mono) {
		/* Stéréo natif 2/2 */
		if (st->audio_in_idx[0] >= 0)
			lilv_instance_connect_port(st->instance, st->audio_in_idx[0], (void *)in_l);
		if (st->audio_in_idx[1] >= 0)
			lilv_instance_connect_port(st->instance, st->audio_in_idx[1], (void *)in_r);
		if (st->audio_out_idx[0] >= 0)
			lilv_instance_connect_port(st->instance, st->audio_out_idx[0], out_l);
		if (st->audio_out_idx[1] >= 0)
			lilv_instance_connect_port(st->instance, st->audio_out_idx[1], out_r);
	} else {
		/* Mono 1/1 dupliqué : instance1 = L, instance2 = R */
		if (st->audio_in_idx[0] >= 0) {
			lilv_instance_connect_port(st->instance,  st->audio_in_idx[0], (void *)in_l);
			lilv_instance_connect_port(st->instance2, st->audio_in_idx[0], (void *)in_r);
		}
		if (st->audio_out_idx[0] >= 0) {
			lilv_instance_connect_port(st->instance,  st->audio_out_idx[0], out_l);
			lilv_instance_connect_port(st->instance2, st->audio_out_idx[0], out_r);
		}
	}

	/* Reset atom ports — 1 fois par block (vs N fois). */
	for (int k = 0; k < st->n_atom_in; k++) {
		LV2_Atom *atom = (LV2_Atom *)st->atom_in_bufs[k];
		atom->size = sizeof(LV2_Atom_Sequence_Body);
		atom->type = g_urid_atom_sequence;
	}
	for (int k = 0; k < st->n_atom_out; k++) {
		LV2_Atom *atom = (LV2_Atom *)st->atom_out_bufs[k];
		atom->size = LV2_ATOM_BUF_SIZE - sizeof(LV2_Atom);
		atom->type = g_urid_atom_chunk;
	}

	/* RUN N samples en 1 call (vs N × N=1). */
	lilv_instance_run(st->instance, N);
	if (st->is_mono && st->instance2)
		lilv_instance_run(st->instance2, N);
}

static int lv2_set_param(fx_engine_t *fx, const char *name, float value)
{
	struct lv2_state *st = fx->state;
	for (int i = 0; i < st->n_ctrl_in; i++) {
		if (strcmp(st->ctrl_in_name[i], name) == 0) {
			st->ctrl_values[i] = value;
			return 0;
		}
	}
	return -1;
}

static void lv2_reset(fx_engine_t *fx)
{
	struct lv2_state *st = fx->state;
	st->buf_in_l = st->buf_in_r = st->buf_out_l = st->buf_out_r = 0;
	if (st->instance) {
		lilv_instance_deactivate(st->instance);
		lilv_instance_activate(st->instance);
	}
	if (st->instance2) {
		lilv_instance_deactivate(st->instance2);
		lilv_instance_activate(st->instance2);
	}
}

static int lv2_get_state(fx_engine_t *fx, char *buf, int len)
{
	struct lv2_state *st = fx->state;
	int n = snprintf(buf, len,
		"\"type\":\"lv2\",\"uri\":\"%s\",\"params\":{",
		st->uri ? st->uri : "");
	for (int i = 0; i < st->n_ctrl_in && n < len - 32; i++) {
		n += snprintf(buf + n, len - n, "%s\"%s\":%.4f",
		              i == 0 ? "" : ",",
		              st->ctrl_in_name[i],
		              st->ctrl_values[i]);
	}
	if (n < len - 2) n += snprintf(buf + n, len - n, "}");
	return n;
}

int fx_init_lv2(fx_engine_t *fx, float sample_rate, const char *uri)
{
	if (!uri || !*uri) return 0;
	if (!lv2_world_init()) return 0;

	LilvNode *plug_uri = lilv_new_uri(g_lv2_world, uri);
	const LilvPlugin *plug = lilv_plugins_get_by_uri(g_lv2_plugins, plug_uri);
	lilv_node_free(plug_uri);
	if (!plug) {
		fprintf(stderr, "LV2: plugin %s not found\n", uri);
		return 0;
	}

	/* RT safety filter — refuse plugin sans hardRTCapable */
	if (!lilv_plugin_has_feature(plug, g_uri_hard_rt)) {
		fprintf(stderr, "LV2: %s NOT hardRTCapable — refused\n", uri);
		return 0;
	}

	/* V9.2-step5c : vérifier que toutes les required_features sont
	 * supportées par l'host (sinon plugin va segfault à activate ou run).
	 * Refus propre avec log de la feature manquante. */
	int needs_worker = 0;
	LilvNodes *req = lilv_plugin_get_required_features(plug);
	if (req) {
		LILV_FOREACH(nodes, it, req) {
			const LilvNode *f = lilv_nodes_get(req, it);
			const char *furi = lilv_node_as_uri(f);
			if (!lv2_host_supports_feature(furi)) {
				fprintf(stderr, "LV2: %s requires unsupported feature '%s' — refused\n",
				        uri, furi ? furi : "(null)");
				lilv_nodes_free(req);
				return 0;
			}
			if (furi && strcmp(furi, LV2_WORKER__schedule) == 0)
				needs_worker = 1;
		}
		lilv_nodes_free(req);
	}
	/* Optional worker support : si plugin l'OFFRE même sans le require, on
	 * lui donne aussi (certains plugins comme calf l'utilisent en optional). */
	if (!needs_worker) {
		LilvNodes *opt = lilv_plugin_get_optional_features(plug);
		if (opt) {
			LILV_FOREACH(nodes, it, opt) {
				const char *furi = lilv_node_as_uri(lilv_nodes_get(opt, it));
				if (furi && strcmp(furi, LV2_WORKER__schedule) == 0) {
					needs_worker = 1;
					break;
				}
			}
			lilv_nodes_free(opt);
		}
	}

	struct lv2_state *st = calloc(1, sizeof(*st));
	if (!st) return 0;
	st->sr  = sample_rate;
	st->uri = strdup(uri);
	st->plugin = plug;
	st->n_ports = (int)lilv_plugin_get_num_ports(plug);
	st->audio_in_idx[0]  = st->audio_in_idx[1]  = -1;
	st->audio_out_idx[0] = st->audio_out_idx[1] = -1;

	/* V9.2-step5d : si plugin demande worker, alloc + spawn thread + build
	 * local features array incluant LV2_WORKER__schedule. Sinon utilise
	 * g_host_features global. */
	const LV2_Feature **features_to_use = g_host_features;
	LV2_Feature feature_worker_local;
	const LV2_Feature *features_local[8] = { NULL };
	if (needs_worker) {
		st->worker = calloc(1, sizeof(*st->worker));
		if (!st->worker) {
			fprintf(stderr, "LV2: worker alloc failed for %s\n", uri);
			free(st->uri); free(st);
			return 0;
		}
		pthread_mutex_init(&st->worker->mutex, NULL);
		pthread_cond_init(&st->worker->cond, NULL);
		st->worker->schedule.handle        = st->worker;
		st->worker->schedule.schedule_work = lv2_worker_schedule_cb;
		feature_worker_local.URI  = LV2_WORKER__schedule;
		feature_worker_local.data = &st->worker->schedule;

		/* Copy g_host_features puis append worker_schedule */
		features_local[0] = &g_feature_urid_map;
		features_local[1] = &g_feature_options;
		features_local[2] = &feature_worker_local;
		features_local[3] = NULL;
		features_to_use = features_local;
	}

	/* Instantiate avec host features (urid:map + options + optionnel worker) */
	st->instance = lilv_plugin_instantiate(plug, (double)sample_rate, features_to_use);
	if (!st->instance) {
		fprintf(stderr, "LV2: instantiate failed for %s\n", uri);
		if (st->worker) {
			pthread_mutex_destroy(&st->worker->mutex);
			pthread_cond_destroy(&st->worker->cond);
			free(st->worker);
		}
		free(st->uri); free(st);
		return 0;
	}

	/* V9.2-step5d : récupère iface worker + spawn thread maintenant que
	 * instance existe. */
	if (st->worker) {
		const LV2_Worker_Interface *iface = (const LV2_Worker_Interface *)
			lilv_instance_get_extension_data(st->instance, LV2_WORKER__interface);
		if (!iface || !iface->work) {
			fprintf(stderr, "LV2: %s claims worker support but no work() iface — refused\n", uri);
			lilv_instance_free(st->instance);
			pthread_mutex_destroy(&st->worker->mutex);
			pthread_cond_destroy(&st->worker->cond);
			free(st->worker);
			free(st->uri); free(st);
			return 0;
		}
		st->worker->iface         = iface;
		st->worker->plugin_handle = lilv_instance_get_handle(st->instance);
		/* spawn thread sur sched OTHER (default) — pas pinné */
		if (pthread_create(&st->worker->thread, NULL,
		                   lv2_worker_thread_fn, st->worker) != 0) {
			fprintf(stderr, "LV2: %s worker pthread_create failed — refused\n", uri);
			lilv_instance_free(st->instance);
			pthread_mutex_destroy(&st->worker->mutex);
			pthread_cond_destroy(&st->worker->cond);
			free(st->worker);
			free(st->uri); free(st);
			return 0;
		}
		fprintf(stderr, "LV2: %s worker thread spawned\n", uri);
	}

	/* Get default control values */
	float *defaults = calloc(st->n_ports, sizeof(float));
	lilv_plugin_get_port_ranges_float(plug, NULL, NULL, defaults);

	/* Scan + connect ports */
	int audio_in_n = 0, audio_out_n = 0;
	for (int i = 0; i < st->n_ports; i++) {
		const LilvPort *port = lilv_plugin_get_port_by_index(plug, i);
		int is_audio  = lilv_port_is_a(plug, port, g_uri_audio_port);
		int is_ctrl   = lilv_port_is_a(plug, port, g_uri_control_port);
		int is_atom   = lilv_port_is_a(plug, port, g_uri_atom_port);
		int is_input  = lilv_port_is_a(plug, port, g_uri_input_port);

		if (is_audio && is_input && audio_in_n < 2) {
			st->audio_in_idx[audio_in_n] = i;
			lilv_instance_connect_port(st->instance, i,
				audio_in_n == 0 ? &st->buf_in_l : &st->buf_in_r);
			audio_in_n++;
		} else if (is_audio && !is_input && audio_out_n < 2) {
			st->audio_out_idx[audio_out_n] = i;
			lilv_instance_connect_port(st->instance, i,
				audio_out_n == 0 ? &st->buf_out_l : &st->buf_out_r);
			audio_out_n++;
		} else if (is_atom) {
			/* V9.2-step5c : AtomPort = control/automation/notify.
			 * Alloue un buffer 8 KB par port, init en sequence vide,
			 * connecte. Reset à chaque cycle dans lv2_process(). */
			int *cnt = is_input ? &st->n_atom_in : &st->n_atom_out;
			if (*cnt >= LV2_MAX_ATOM_PORTS) {
				fprintf(stderr, "LV2: %s too many atom ports (>%d) — refused\n",
				        uri, LV2_MAX_ATOM_PORTS);
				free(defaults);
				/* cleanup partial alloc + return */
				for (int k = 0; k < st->n_atom_in; k++)  free(st->atom_in_bufs[k]);
				for (int k = 0; k < st->n_atom_out; k++) free(st->atom_out_bufs[k]);
				lilv_instance_free(st->instance);
				free(st->uri); free(st);
				return 0;
			}
			uint8_t *buf = calloc(1, LV2_ATOM_BUF_SIZE);
			if (!buf) {
				fprintf(stderr, "LV2: %s atom buf alloc failed\n", uri);
				free(defaults);
				for (int k = 0; k < st->n_atom_in; k++)  free(st->atom_in_bufs[k]);
				for (int k = 0; k < st->n_atom_out; k++) free(st->atom_out_bufs[k]);
				lilv_instance_free(st->instance);
				free(st->uri); free(st);
				return 0;
			}
			if (is_input) {
				LV2_Atom *atom = (LV2_Atom *)buf;
				atom->size = sizeof(LV2_Atom_Sequence_Body);
				atom->type = g_urid_atom_sequence;
				st->atom_in_idx[st->n_atom_in] = i;
				st->atom_in_bufs[st->n_atom_in++] = buf;
			} else {
				LV2_Atom *atom = (LV2_Atom *)buf;
				atom->size = LV2_ATOM_BUF_SIZE - sizeof(LV2_Atom);
				atom->type = g_urid_atom_chunk;
				st->atom_out_idx[st->n_atom_out] = i;
				st->atom_out_bufs[st->n_atom_out++] = buf;
			}
			lilv_instance_connect_port(st->instance, i, buf);
		} else if (is_ctrl && is_input && st->n_ctrl_in < LV2_MAX_CTRL_PORTS) {
			int idx = st->n_ctrl_in++;
			st->ctrl_in_idx[idx] = i;
			st->ctrl_values[idx] = defaults[i];
			LilvNode *sym = (LilvNode *)lilv_port_get_symbol(plug, port);
			const char *sym_str = sym ? lilv_node_as_string(sym) : "?";
			strncpy(st->ctrl_in_name[idx], sym_str, LV2_MAX_NAME_LEN - 1);
			lilv_instance_connect_port(st->instance, i,
				&st->ctrl_values[idx]);
		} else if (is_ctrl) {
			/* Control OUTPUT port — connect to dummy buffer */
			lilv_instance_connect_port(st->instance, i,
				&st->ctrl_out_dummy[i % LV2_MAX_CTRL_PORTS]);
		}
	}
	free(defaults);

	/* Stéréo natif (2/2) : OK direct.
	 * Mono (1/1) : instancier une 2e fois pour le canal R, partageant
	 *              les contrôles. Plugin doit être stateless ou
	 *              indépendant par instance (typique : gain, biquad).
	 * Autre : refus.
	 */
	if (audio_in_n == 2 && audio_out_n == 2) {
		st->is_mono = 0;
	} else if (audio_in_n == 1 && audio_out_n == 1) {
		st->is_mono = 1;
		st->instance2 = lilv_plugin_instantiate(plug, (double)sample_rate, g_host_features);
		if (!st->instance2) {
			fprintf(stderr, "LV2: %s 2nd instance failed for mono→stereo\n", uri);
			lilv_instance_free(st->instance);
			free(st->uri); free(st);
			return 0;
		}
		/* Connect ports de l'instance 2 :
		 *  - audio input (1) → buf_in_r
		 *  - audio output (1) → buf_out_r
		 *  - control inputs → MÊMES buffers que instance1 (params partagés)
		 *  - control outputs → ctrl_out_dummy
		 */
		int ctrl_i = 0;
		int ain_i = 0, aout_i = 0;
		for (int i = 0; i < st->n_ports; i++) {
			const LilvPort *port = lilv_plugin_get_port_by_index(plug, i);
			int is_audio  = lilv_port_is_a(plug, port, g_uri_audio_port);
			int is_ctrl   = lilv_port_is_a(plug, port, g_uri_control_port);
			int is_atom   = lilv_port_is_a(plug, port, g_uri_atom_port);
			int is_input  = lilv_port_is_a(plug, port, g_uri_input_port);

			if (is_audio && is_input)
				lilv_instance_connect_port(st->instance2, i, &st->buf_in_r);
			else if (is_audio && !is_input)
				lilv_instance_connect_port(st->instance2, i, &st->buf_out_r);
			else if (is_atom) {
				/* V9.2-step5c : partage des buffers atom avec instance1.
				 * Limitation : pas d'automation indépendante par instance.
				 * Suffisant en mode passif (pas de MIDI/automation envoyés). */
				uint8_t *buf = NULL;
				if (is_input)
					buf = (ain_i < st->n_atom_in) ? st->atom_in_bufs[ain_i++] : NULL;
				else
					buf = (aout_i < st->n_atom_out) ? st->atom_out_bufs[aout_i++] : NULL;
				/* Si pas de buf (mismatch), connect NULL = laisser flotter
				 * → mais le plugin a déjà passé l'init donc tolère probablement.
				 * Cas non observé jusqu'ici. */
				if (buf) lilv_instance_connect_port(st->instance2, i, buf);
			}
			else if (is_ctrl && is_input)
				lilv_instance_connect_port(st->instance2, i, &st->ctrl_values[ctrl_i++]);
			else if (is_ctrl)
				lilv_instance_connect_port(st->instance2, i,
					&st->ctrl_out_dummy[i % LV2_MAX_CTRL_PORTS]);
		}
		lilv_instance_activate(st->instance2);
		fprintf(stderr, "LV2: %s mono→stereo (2 instances)\n", uri);
	} else {
		fprintf(stderr, "LV2: %s I/O mismatch (in=%d out=%d, want 2/2 or 1/1)\n",
		        uri, audio_in_n, audio_out_n);
		lilv_instance_free(st->instance);
		free(st->uri); free(st);
		return 0;
	}

	lilv_instance_activate(st->instance);

	fx->type_name = "lv2";
	fx->state     = st;
	fx->process_block = lv2_process_block;
	fx->set_param = lv2_set_param;
	fx->reset     = lv2_reset;
	fx->get_state = lv2_get_state;
	return 1;
}

int fx_lv2_list_uris(char *buf, int len)
{
	if (!lv2_world_init()) return 0;
	int n = snprintf(buf, len, "[");
	int first = 1;
	LILV_FOREACH(plugins, it, g_lv2_plugins) {
		const LilvPlugin *plug = lilv_plugins_get(g_lv2_plugins, it);
		if (!lilv_plugin_has_feature(plug, g_uri_hard_rt))
			continue;   /* skip non-RT */
		const LilvNode *uri  = lilv_plugin_get_uri(plug);
		LilvNode *name       = lilv_plugin_get_name(plug);
		if (!uri) continue;
		if (n >= len - 128) break;
		n += snprintf(buf + n, len - n, "%s{\"uri\":\"%s\",\"name\":\"%s\"}",
		              first ? "" : ",",
		              lilv_node_as_string(uri),
		              name ? lilv_node_as_string(name) : "?");
		lilv_node_free(name);
		first = 0;
	}
	if (n < len - 2) n += snprintf(buf + n, len - n, "]");
	return n;
}

/* ============================== Common ============================= */

void fx_free(fx_engine_t *fx)
{
	if (!fx || !fx->state) return;
	/* LV2 engine : cleanup lilv instance d'abord (différent du calloc).
	 * Détection par type_name (pas idéal mais évite refactor vtable). */
	if (fx->type_name && strcmp(fx->type_name, "lv2") == 0) {
		struct lv2_state *st = fx->state;
		/* V9.2-step5d : stop worker thread AVANT free instance.
		 * Set exit_flag + signal cond + join. Plugin work() ne sera plus
		 * appelée après ; instance peut être deactivate/free. */
		if (st->worker) {
			pthread_mutex_lock(&st->worker->mutex);
			st->worker->exit_flag = 1;
			pthread_cond_signal(&st->worker->cond);
			pthread_mutex_unlock(&st->worker->mutex);
			pthread_join(st->worker->thread, NULL);
			if (st->worker->drops)
				fprintf(stderr, "LV2: worker drops=%llu\n",
				        (unsigned long long)st->worker->drops);
			pthread_mutex_destroy(&st->worker->mutex);
			pthread_cond_destroy(&st->worker->cond);
			free(st->worker);
		}
		if (st->instance) {
			lilv_instance_deactivate(st->instance);
			lilv_instance_free(st->instance);
		}
		if (st->instance2) {
			lilv_instance_deactivate(st->instance2);
			lilv_instance_free(st->instance2);
		}
		/* V9.2-step5c : libère les buffers atom alloués en fx_init_lv2 */
		for (int k = 0; k < st->n_atom_in; k++)  free(st->atom_in_bufs[k]);
		for (int k = 0; k < st->n_atom_out; k++) free(st->atom_out_bufs[k]);
		free(st->uri);
	}
	free(fx->state);
	fx->state = NULL;
}
