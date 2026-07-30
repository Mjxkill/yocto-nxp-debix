// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fx_chain — passthrough + cascade V9.4 de N sub-engines (insert
 * mastering) + fx_free commun.
 * Code déplacé tel quel depuis effects.c (V14.0 étape 5, extraction pure).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effects.h"
#include "fx_internal.h"
#include "fx_lv2.h"
#include <pthread.h>

/* ========================================================================
 *   4b2. V9.5.20 — fx_passthrough : engine neutre pour les bus FX send.
 *   La GUI proposait "passthrough" mais aucun init n'existait → erreur.
 * ====================================================================== */

static void passthrough_process_block(fx_engine_t *fx,
				       const float *in_l, const float *in_r,
				       float *out_l, float *out_r,
				       uint32_t N)
{
	(void)fx;
	if (out_l != in_l) memcpy(out_l, in_l, N * sizeof(float));
	if (out_r != in_r) memcpy(out_r, in_r, N * sizeof(float));
}

static int passthrough_set_param(fx_engine_t *fx, const char *name, float value)
{
	(void)fx; (void)name; (void)value;
	return -1;
}

static void passthrough_reset(fx_engine_t *fx) { (void)fx; }

static int passthrough_get_state(fx_engine_t *fx, char *buf, int len)
{
	(void)fx;
	return snprintf(buf, len, "\"type\":\"passthrough\"");
}

int fx_init_passthrough(fx_engine_t *fx, float sample_rate)
{
	(void)sample_rate;
	fx->type_name = "passthrough";
	fx->state = NULL;
	fx->process_block = passthrough_process_block;
	fx->set_param = passthrough_set_param;
	fx->reset = passthrough_reset;
	fx->get_state = passthrough_get_state;
	return 1;
}


/* ========================================================================
 *   V9.4 — fx_chain : cascade de N sub-engines (insert mastering)
 * ====================================================================== */

struct chain_state {
	int          n_plugins;            /* 0..FX_CHAIN_MAX */
	fx_engine_t  plugins[FX_CHAIN_MAX];
};

/* Buffers ping-pong globaux (1 seul thread audio_thread utilise) — BSS 3 KB. */
static float g_chain_tmp_a_l[96];
static float g_chain_tmp_a_r[96];
static float g_chain_tmp_b_l[96];
static float g_chain_tmp_b_r[96];

static void chain_process_block(fx_engine_t *fx,
                                const float *in_l, const float *in_r,
                                float *out_l, float *out_r, uint32_t N)
{
	struct chain_state *c = fx->state;
	int Np = c->n_plugins;

	if (Np == 0) {
		/* Bypass : in → out */
		if (in_l != out_l) memcpy(out_l, in_l, N * sizeof(float));
		if (in_r != out_r) memcpy(out_r, in_r, N * sizeof(float));
		return;
	}
	if (Np == 1) {
		/* Direct in → out (in et out peuvent être les mêmes buffers) */
		c->plugins[0].process_block(&c->plugins[0], in_l, in_r, out_l, out_r, N);
		return;
	}

	/* Plugin 0 : in → tmp_a */
	c->plugins[0].process_block(&c->plugins[0], in_l, in_r,
	                            g_chain_tmp_a_l, g_chain_tmp_a_r, N);

	/* Plugins 1..Np-2 : ping-pong tmp_a ↔ tmp_b */
	int parity = 0;   /* 0 : src=A, dst=B ; 1 : src=B, dst=A */
	for (int i = 1; i < Np - 1; i++) {
		float *src_l = parity ? g_chain_tmp_b_l : g_chain_tmp_a_l;
		float *src_r = parity ? g_chain_tmp_b_r : g_chain_tmp_a_r;
		float *dst_l = parity ? g_chain_tmp_a_l : g_chain_tmp_b_l;
		float *dst_r = parity ? g_chain_tmp_a_r : g_chain_tmp_b_r;
		c->plugins[i].process_block(&c->plugins[i], src_l, src_r, dst_l, dst_r, N);
		parity = !parity;
	}

	/* Plugin Np-1 (dernier) : src → out_l/r */
	float *src_l = parity ? g_chain_tmp_b_l : g_chain_tmp_a_l;
	float *src_r = parity ? g_chain_tmp_b_r : g_chain_tmp_a_r;
	c->plugins[Np - 1].process_block(&c->plugins[Np - 1], src_l, src_r,
	                                 out_l, out_r, N);
}

/* set_param : route "<slot>/<param_name>" → plugins[slot].set_param("<param_name>") */
static int chain_set_param(fx_engine_t *fx, const char *name, float value)
{
	struct chain_state *c = fx->state;
	if (!name) return -1;
	const char *slash = strchr(name, '/');
	if (!slash) return -1;
	int slot = atoi(name);  /* parse leading digits */
	if (slot < 0 || slot >= c->n_plugins) return -1;
	return c->plugins[slot].set_param(&c->plugins[slot], slash + 1, value);
}

static void chain_reset(fx_engine_t *fx)
{
	struct chain_state *c = fx->state;
	for (int i = 0; i < c->n_plugins; i++)
		if (c->plugins[i].reset) c->plugins[i].reset(&c->plugins[i]);
}

static int chain_get_state(fx_engine_t *fx, char *buf, int len)
{
	struct chain_state *c = fx->state;
	int n = snprintf(buf, len, "\"type\":\"chain\",\"n\":%d,\"chain\":[", c->n_plugins);
	for (int i = 0; i < c->n_plugins && n < len - 64; i++) {
		n += snprintf(buf + n, len - n, "%s{\"slot\":%d,", i ? "," : "", i);
		n += c->plugins[i].get_state(&c->plugins[i], buf + n, len - n);
		n += snprintf(buf + n, len - n, "}");
	}
	if (n < len - 2) n += snprintf(buf + n, len - n, "]");
	return n;
}

int fx_init_chain(fx_engine_t *fx, float sample_rate,
                  const struct fx_chain_spec *specs, int n_specs)
{
	if (n_specs < 0 || n_specs > FX_CHAIN_MAX) return 0;

	struct chain_state *c = calloc(1, sizeof(*c));
	if (!c) return 0;
	c->n_plugins = 0;   /* incrémenté au fur et à mesure ; rollback si erreur */

	for (int i = 0; i < n_specs; i++) {
		const struct fx_chain_spec *s = &specs[i];
		int ok = 0;
		if (!s->engine) goto fail;
		if      (!strcmp(s->engine, "compressor")) ok = fx_init_compressor(&c->plugins[i], sample_rate);
		else if (!strcmp(s->engine, "reverb"))     ok = fx_init_reverb    (&c->plugins[i], sample_rate);
		else if (!strcmp(s->engine, "delay"))      ok = fx_init_delay     (&c->plugins[i], sample_rate);
		else if (!strcmp(s->engine, "eq"))         ok = fx_init_eq        (&c->plugins[i], sample_rate);
		else if (!strcmp(s->engine, "para_eq_x16")) ok = fx_init_para_eq_x16(&c->plugins[i], sample_rate);
		else if (!strcmp(s->engine, "spectral_env")) ok = fx_init_spectral_env(&c->plugins[i], sample_rate);
		else if (!strcmp(s->engine, "exciter_native")) ok = fx_init_exciter_native(&c->plugins[i], sample_rate);
		else if (!strcmp(s->engine, "limiter_native")) ok = fx_init_limiter_native(&c->plugins[i], sample_rate);
		else if (!strcmp(s->engine, "lv2") && s->uri && *s->uri)
			ok = fx_init_lv2(&c->plugins[i], sample_rate, s->uri);
		if (!ok) {
			fprintf(stderr, "chain: slot %d init failed (engine=%s uri=%s)\n",
			        i, s->engine, s->uri ? s->uri : "");
			goto fail;
		}
		c->n_plugins++;
	}

	fx->type_name = "chain";
	fx->state = c;
	fx->process_block = chain_process_block;
	fx->set_param = chain_set_param;
	fx->reset = chain_reset;
	fx->get_state = chain_get_state;
	return 1;

fail:
	for (int j = 0; j < c->n_plugins; j++)
		fx_free(&c->plugins[j]);
	free(c);
	return 0;
}


/* ============================== Common ============================= */

void fx_free(fx_engine_t *fx)
{
	if (!fx || !fx->state) return;
	/* V9.4 — chain : free récursif des sub-engines avant free state */
	if (fx->type_name && strcmp(fx->type_name, "chain") == 0) {
		struct chain_state *c = fx->state;
		for (int i = 0; i < c->n_plugins; i++)
			fx_free(&c->plugins[i]);
		free(c);
		fx->state = NULL;
		return;
	}
	if (fx->type_name && strcmp(fx->type_name, "lv2") == 0)
		lv2_free_state(fx);   /* lv2_host.c (V14.0 étape 5) */
	free(fx->state);
	fx->state = NULL;
}
