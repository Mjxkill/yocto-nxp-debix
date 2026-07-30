// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fx_dynamics — compressor natif + limiter natif (bus FX / insert).
 * Code déplacé tel quel depuis effects.c (V14.0 étape 5, extraction pure).
 * NOTE : PAS un doublon du compresseur de tranche (strip_dyn.c) —
 * algorithmes distincts à dessein (enveloppe/échantillon + loi linéaire
 * ici ; crête/bloc + loi dB côté tranches).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effects.h"
#include "fx_internal.h"

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

/* NOTE (revue 2026-07-28, lot 5b) : ce compresseur d'INSERT n'est PAS un
 * doublon de cmp_render (mixer-pro.c) — algorithmes distincts à dessein :
 * ici enveloppe PAR ÉCHANTILLON + loi de gain LINÉAIRE (stéréo master,
 * params fixes) ; cmp_render = crête par bloc + loi en dB + rampe anti-
 * zipper (16 tranches mono, seuils adaptatifs). Fusion = changement du son
 * validé des deux. */
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
 *   4e. V9.5.20 — fx_limiter_native : limiter natif (parité surrogate
 *   limiter du training — envelope follower + hard knee + soft ceiling).
 *
 *     x   *= g_in
 *     env  = follower(max(|L|,|R|), attack at, release rt)
 *     gain = env > th ? th / env : 1
 *     y    = (x × gain), soft-clip ceil × tanh(y/ceil), × g_out
 *
 *   Params (mêmes noms/unités que le push daemon, LSP-compatibles) :
 *   th (lin), g_in (lin), g_out (lin), at (ms), rt (ms), ceil (lin).
 *   Coût ≈ 60 µs / période.
 * ====================================================================== */

struct limiter_state {
	float sr;
	int   bypass;                  /* M/A GUI : 1 = traverse sans effet */
	float th, g_in, g_out, at_ms, rt_ms, ceil;
	float env;
	float a_a, a_r;
	float cfg_at, cfg_rt;
};

static void limiter_recalc(struct limiter_state *l)
{
	l->a_a = 1.0f - expf(-1.0f / (CLAMP(l->at_ms, 0.1f, 100.0f) * l->sr / 1000.0f));
	l->a_r = 1.0f - expf(-1.0f / (CLAMP(l->rt_ms, 1.0f, 1000.0f) * l->sr / 1000.0f));
	l->cfg_at = l->at_ms; l->cfg_rt = l->rt_ms;
}

static void limiter_process_block(fx_engine_t *fx,
				   const float *in_l, const float *in_r,
				   float *out_l, float *out_r,
				   uint32_t N)
{
	struct limiter_state *l = fx->state;
	if (l->bypass) {
		if (out_l != in_l) memcpy(out_l, in_l, N * sizeof(float));
		if (out_r != in_r) memcpy(out_r, in_r, N * sizeof(float));
		return;
	}
	if (l->cfg_at != l->at_ms || l->cfg_rt != l->rt_ms)
		limiter_recalc(l);
	const float th = l->th;
	const float gi = l->g_in, go = l->g_out;
	const float cl = l->ceil < 1e-6f ? 1e-6f : l->ceil;
	float env = l->env;
	for (uint32_t s = 0; s < N; s++) {
		const float xl = in_l[s] * gi;
		const float xr = in_r[s] * gi;
		const float al = fabsf(xl), ar = fabsf(xr);
		const float am = al > ar ? al : ar;
		env += (am > env ? l->a_a : l->a_r) * (am - env);
		const float gain = env > th ? th / (env + 1e-12f) : 1.0f;
		const float yl = xl * gain;
		const float yr = xr * gain;
		out_l[s] = cl * tanhf(yl / cl) * go;
		out_r[s] = cl * tanhf(yr / cl) * go;
	}
	l->env = env;
}

static int limiter_set_param(fx_engine_t *fx, const char *name, float value)
{
	struct limiter_state *l = fx->state;
	if      (!strcmp(name, "bypass")) l->bypass = value > 0.5f;
	else if (!strcmp(name, "th"))    l->th    = CLAMP(value, 0.05f, 1.0f);
	else if (!strcmp(name, "g_in"))  l->g_in  = CLAMP(value, 0.1f, 16.0f);
	else if (!strcmp(name, "g_out")) l->g_out = CLAMP(value, 0.1f, 2.0f);
	else if (!strcmp(name, "at"))    l->at_ms = CLAMP(value, 0.1f, 100.0f);
	else if (!strcmp(name, "rt"))    l->rt_ms = CLAMP(value, 1.0f, 1000.0f);
	else if (!strcmp(name, "ceil"))  l->ceil  = CLAMP(value, 0.5f, 1.0f);
	else return -1;
	return 0;
}

static void limiter_reset(fx_engine_t *fx)
{
	struct limiter_state *l = fx->state;
	l->env = 0.0f;
}

static int limiter_get_state(fx_engine_t *fx, char *buf, int len)
{
	struct limiter_state *l = fx->state;
	return snprintf(buf, len,
		"\"type\":\"limiter_native\",\"bypass\":%d,\"th\":%.4f,"
		"\"g_in\":%.4f,\"g_out\":%.4f,\"at\":%.2f,\"rt\":%.1f,\"ceil\":%.4f",
		l->bypass, l->th, l->g_in, l->g_out, l->at_ms, l->rt_ms, l->ceil);
}

int fx_init_limiter_native(fx_engine_t *fx, float sample_rate)
{
	struct limiter_state *l = calloc(1, sizeof(*l));
	if (!l) return 0;
	l->sr = sample_rate;
	l->th = 1.0f; l->g_in = 1.0f; l->g_out = 1.0f;
	l->at_ms = 4.0f; l->rt_ms = 100.0f; l->ceil = 0.99f;
	limiter_recalc(l);
	fx->type_name = "limiter_native";
	fx->state = l;
	fx->process_block = limiter_process_block;
	fx->set_param = limiter_set_param;
	fx->reset = limiter_reset;
	fx->get_state = limiter_get_state;
	return 1;
}

