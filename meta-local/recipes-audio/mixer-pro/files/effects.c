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

static void comp_process(fx_engine_t *fx, float in_l, float in_r,
			 float *out_l, float *out_r)
{
	struct comp_state *c = fx->state;

	/* Envelope follower : peak suivi par attack/release. */
	float al = fabsf(in_l);
	float ar = fabsf(in_r);
	c->env_l = (al > c->env_l)
		? c->attack_coef  * c->env_l + (1.0f - c->attack_coef)  * al
		: c->release_coef * c->env_l + (1.0f - c->release_coef) * al;
	c->env_r = (ar > c->env_r)
		? c->attack_coef  * c->env_r + (1.0f - c->attack_coef)  * ar
		: c->release_coef * c->env_r + (1.0f - c->release_coef) * ar;

	/* Gain reduction : si env > threshold, on applique 1/ratio sur l'excès.
	 * gain = (threshold + (env - threshold) / ratio) / env  (en dessous = 1.0)
	 */
	float gl = 1.0f, gr = 1.0f;
	if (c->env_l > c->threshold_lin)
		gl = (c->threshold_lin + (c->env_l - c->threshold_lin) / c->ratio) / c->env_l;
	if (c->env_r > c->threshold_lin)
		gr = (c->threshold_lin + (c->env_r - c->threshold_lin) / c->ratio) / c->env_r;

	*out_l = in_l * gl * c->makeup_lin;
	*out_r = in_r * gr * c->makeup_lin;
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
	fx->process = comp_process;
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

static void reverb_process(fx_engine_t *fx, float in_l, float in_r,
			   float *out_l, float *out_r)
{
	struct reverb_state *r = fx->state;
	float feedback = 0.28f + r->room_size * 0.7f;   /* 0.28..0.98 */
	float damp1 = r->damping * 0.4f;
	float damp2 = 1.0f - damp1;

	float comb_out_l = 0.0f, comb_out_r = 0.0f;

	for (int c = 0; c < COMB_N; c++) {
		int n = comb_lens[c];
		/* L */
		int i = r->comb_idx[0][c];
		float v = r->comb_buf[0][c][i];
		r->comb_filt[0][c] = v * damp2 + r->comb_filt[0][c] * damp1;
		r->comb_buf[0][c][i] = in_l + r->comb_filt[0][c] * feedback;
		r->comb_idx[0][c] = (i + 1) % n;
		comb_out_l += v;
		/* R */
		i = r->comb_idx[1][c];
		v = r->comb_buf[1][c][i];
		r->comb_filt[1][c] = v * damp2 + r->comb_filt[1][c] * damp1;
		r->comb_buf[1][c][i] = in_r + r->comb_filt[1][c] * feedback;
		r->comb_idx[1][c] = (i + 1) % n;
		comb_out_r += v;
	}

	/* Allpass série */
	float ap_l = comb_out_l;
	float ap_r = comb_out_r;
	for (int a = 0; a < AP_N; a++) {
		int n = ap_lens[a];
		/* L */
		int i = r->ap_idx[0][a];
		float bufout = r->ap_buf[0][a][i];
		float input  = ap_l;
		r->ap_buf[0][a][i] = input + bufout * 0.5f;
		ap_l = bufout - input;
		r->ap_idx[0][a] = (i + 1) % n;
		/* R */
		i = r->ap_idx[1][a];
		bufout = r->ap_buf[1][a][i];
		input  = ap_r;
		r->ap_buf[1][a][i] = input + bufout * 0.5f;
		ap_r = bufout - input;
		r->ap_idx[1][a] = (i + 1) % n;
	}

	*out_l = ap_l * r->wet;
	*out_r = ap_r * r->wet;
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
	fx->process = reverb_process;
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

static void delay_process(fx_engine_t *fx, float in_l, float in_r,
			  float *out_l, float *out_r)
{
	struct delay_state *d = fx->state;
	int ridx = d->widx - d->delay_samples;
	if (ridx < 0) ridx += DELAY_MAX_SAMP;

	float dl = d->buf_l[ridx];
	float dr = d->buf_r[ridx];

	/* Write : input + feedback du dernier sample lu */
	d->buf_l[d->widx] = in_l + dl * d->feedback;
	d->buf_r[d->widx] = in_r + dr * d->feedback;
	d->widx = (d->widx + 1) % DELAY_MAX_SAMP;

	*out_l = dl * d->wet;
	*out_r = dr * d->wet;
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
	fx->process = delay_process;
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

static void eq_process(fx_engine_t *fx, float in_l, float in_r,
		       float *out_l, float *out_r)
{
	struct eq_state *e = fx->state;
	float l = biquad_step(&e->bq_high, 0,
	          biquad_step(&e->bq_mid,  0,
	          biquad_step(&e->bq_low,  0, in_l)));
	float r = biquad_step(&e->bq_high, 1,
	          biquad_step(&e->bq_mid,  1,
	          biquad_step(&e->bq_low,  1, in_r)));
	*out_l = l;
	*out_r = r;
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
	fx->process = eq_process;
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
#include <lilv/lilv.h>
#include <lv2/core/lv2.h>

/* Global lilv world (shared par tous les bus LV2). Init lazy. */
static LilvWorld *g_lv2_world           = NULL;
static const LilvPlugins *g_lv2_plugins = NULL;
static LilvNode  *g_uri_audio_port      = NULL;
static LilvNode  *g_uri_control_port    = NULL;
static LilvNode  *g_uri_input_port      = NULL;
static LilvNode  *g_uri_output_port     = NULL;
static LilvNode  *g_uri_hard_rt         = NULL;

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

	g_lv2_plugins = lilv_world_get_all_plugins(g_lv2_world);
	return 1;
}

#define LV2_MAX_CTRL_PORTS 64
#define LV2_MAX_NAME_LEN   32

struct lv2_state {
	float          sr;
	char          *uri;
	LilvInstance  *instance;
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
};

static void lv2_process(fx_engine_t *fx, float in_l, float in_r,
                        float *out_l, float *out_r)
{
	struct lv2_state *st = fx->state;
	st->buf_in_l = in_l;
	st->buf_in_r = in_r;
	lilv_instance_run(st->instance, 1);
	*out_l = st->buf_out_l;
	*out_r = st->buf_out_r;
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

	struct lv2_state *st = calloc(1, sizeof(*st));
	if (!st) return 0;
	st->sr  = sample_rate;
	st->uri = strdup(uri);
	st->plugin = plug;
	st->n_ports = (int)lilv_plugin_get_num_ports(plug);
	st->audio_in_idx[0]  = st->audio_in_idx[1]  = -1;
	st->audio_out_idx[0] = st->audio_out_idx[1] = -1;

	/* Instantiate */
	st->instance = lilv_plugin_instantiate(plug, (double)sample_rate, NULL);
	if (!st->instance) {
		fprintf(stderr, "LV2: instantiate failed for %s\n", uri);
		free(st->uri); free(st);
		return 0;
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

	if (audio_in_n != 2 || audio_out_n != 2) {
		fprintf(stderr, "LV2: %s I/O mismatch (in=%d out=%d, want 2/2)\n",
		        uri, audio_in_n, audio_out_n);
		lilv_instance_free(st->instance);
		free(st->uri); free(st);
		return 0;
	}

	lilv_instance_activate(st->instance);

	fx->type_name = "lv2";
	fx->state     = st;
	fx->process   = lv2_process;
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
		if (st->instance) {
			lilv_instance_deactivate(st->instance);
			lilv_instance_free(st->instance);
		}
		free(st->uri);
	}
	free(fx->state);
	fx->state = NULL;
}
