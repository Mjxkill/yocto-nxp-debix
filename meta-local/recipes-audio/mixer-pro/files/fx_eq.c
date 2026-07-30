// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fx_eq — EQ 3 bandes + para_eq_x16 (mastering ML) + enveloppe
 * spectrale 64 pts→FIR 256 + exciter natif.
 * Code déplacé tel quel depuis effects.c (V14.0 étape 5, extraction pure).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effects.h"
#include "fx_internal.h"
#include <fftw3.h>

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
 *   4b. V9.5.12 — para_eq_x16 : 16 biquads peak stéréo (mastering ML).
 *
 *   Lock-free, RT-safe. Pas de worker thread (vs LV2 LSP/Calf).
 *   Params : bN_freq, bN_gain_db, bN_q (N=0..15) — atomic write.
 *   Biquad recompute déclenché par set_param (cheap : ~50 FLOPs per band).
 * ====================================================================== */

#define PARA_EQ_N_BANDS 16

struct para_eq_state {
	float sr;
	float freq[PARA_EQ_N_BANDS];
	float gain_db[PARA_EQ_N_BANDS];
	float q[PARA_EQ_N_BANDS];
	struct biquad bq[PARA_EQ_N_BANDS];
};

static void para_eq_recalc_band(struct para_eq_state *e, int b)
{
	biquad_set(&e->bq[b], BQ_PEAK, e->sr, e->freq[b], e->q[b], e->gain_db[b]);
}

static void para_eq_process_block(fx_engine_t *fx,
				   const float *in_l, const float *in_r,
				   float *out_l, float *out_r,
				   uint32_t N)
{
	struct para_eq_state *e = fx->state;
	for (uint32_t s = 0; s < N; s++) {
		float l = in_l[s];
		float r = in_r[s];
		for (int b = 0; b < PARA_EQ_N_BANDS; b++) {
			l = biquad_step(&e->bq[b], 0, l);
			r = biquad_step(&e->bq[b], 1, r);
		}
		out_l[s] = l;
		out_r[s] = r;
	}
}

static int para_eq_set_param(fx_engine_t *fx, const char *name, float value)
{
	struct para_eq_state *e = fx->state;
	/* Format : bN_freq / bN_gain_db / bN_q  (N = 0..15) */
	if (name[0] != 'b') return -1;
	const char *p = name + 1;
	int b = 0;
	while (*p >= '0' && *p <= '9') { b = b * 10 + (*p - '0'); p++; }
	if (b < 0 || b >= PARA_EQ_N_BANDS) return -1;
	if (*p != '_') return -1;
	p++;
	if      (!strcmp(p, "freq"))    e->freq[b]    = CLAMP(value, 20.0f, 20000.0f);
	else if (!strcmp(p, "gain_db")) e->gain_db[b] = CLAMP(value, -18.0f, 18.0f);
	else if (!strcmp(p, "q"))       e->q[b]       = CLAMP(value, 0.1f, 10.0f);
	else return -1;
	para_eq_recalc_band(e, b);
	return 0;
}

static void para_eq_reset(fx_engine_t *fx)
{
	struct para_eq_state *e = fx->state;
	for (int b = 0; b < PARA_EQ_N_BANDS; b++) {
		memset(&e->bq[b].x1, 0, sizeof(e->bq[b].x1));
		memset(&e->bq[b].x2, 0, sizeof(e->bq[b].x2));
		memset(&e->bq[b].y1, 0, sizeof(e->bq[b].y1));
		memset(&e->bq[b].y2, 0, sizeof(e->bq[b].y2));
	}
}

static int para_eq_get_state(fx_engine_t *fx, char *buf, int len)
{
	struct para_eq_state *e = fx->state;
	int n = snprintf(buf, len, "\"type\":\"para_eq_x16\",\"bands\":[");
	for (int b = 0; b < PARA_EQ_N_BANDS && n < len - 80; b++) {
		n += snprintf(buf + n, len - n,
		              "%s{\"f\":%.1f,\"g\":%.2f,\"q\":%.2f}",
		              b == 0 ? "" : ",", e->freq[b], e->gain_db[b], e->q[b]);
	}
	if (n < len - 4) n += snprintf(buf + n, len - n, "]");
	return n;
}

int fx_init_para_eq_x16(fx_engine_t *fx, float sample_rate)
{
	struct para_eq_state *e = calloc(1, sizeof(*e));
	if (!e) return 0;
	e->sr = sample_rate;
	/* Default : 16 bandes log-spaced 20-20000 Hz, gain 0 dB, Q 1.0 */
	for (int b = 0; b < PARA_EQ_N_BANDS; b++) {
		float t = (float)b / (float)(PARA_EQ_N_BANDS - 1);   /* 0..1 */
		e->freq[b]    = 20.0f * powf(1000.0f, t);            /* 20..20000 log */
		e->gain_db[b] = 0.0f;
		e->q[b]       = 1.0f;
		para_eq_recalc_band(e, b);
	}
	fx->type_name = "para_eq_x16";
	fx->state = e;
	fx->process_block = para_eq_process_block;
	fx->set_param = para_eq_set_param;
	fx->reset = para_eq_reset;
	fx->get_state = para_eq_get_state;
	return 1;
}



/* ========================================================================
 *   4c. V9.5.20 — fx_spectral_env : enveloppe spectrale 64 pts → FIR 256.
 *
 *   Le modèle ML (daemon mixer-ml-inference) pousse 64 gains dB par canal
 *   (bandes log 20 Hz - 20 kHz). Application par FIR 256 taps phase
 *   linéaire (latence 128 samples = 2.67 ms), reconstruite quand les gains
 *   changent, avec interpolation linéaire des taps entre l'ancienne et la
 *   nouvelle FIR (lissage temporel, pas de clics).
 *
 *   Lock-free : set_param écrit les gains cible (floats), l'audio_thread
 *   détecte le changement (compteur), reconstruit et interpole.
 *
 *   Params : l_g0..l_g63 (canal L), r_g0..r_g63 (canal R), en dB ±12.
 *   Parité avec training/surrogate_spectral_env.py : interp log-fréquence
 *   de l'enveloppe vers |H(f)|, irfft, shift centre, fenêtre Hann.
 * ====================================================================== */

#include <fftw3.h>

#define SENV_N        64
#define SENV_TAPS     256
#define SENV_NBINS    (SENV_TAPS / 2 + 1)
#define SENV_FMIN     20.0f
#define SENV_FMAX     20000.0f
#define SENV_XFADE_BLOCKS 5    /* interpolation des taps sur 5 blocs (10 ms) */

struct senv_chan {
	float target_db[SENV_N];      /* écrit par control_thread (set_param) */
	unsigned target_seq;          /* incrémenté à chaque set complet */
	unsigned applied_seq;
	float h_old[SENV_TAPS];
	float h_new[SENV_TAPS];
	float h_cur[SENV_TAPS];
	int   fade_pos;               /* 0..SENV_XFADE_BLOCKS ; >=X = stable */
	float dline[SENV_TAPS - 1];   /* delay line (état conv) */
};

struct senv_state {
	float sr;
	float log_env_f[SENV_N];      /* log des fréqs centrales */
	float bin_w[SENV_NBINS];      /* poids interp précalculés */
	int   bin_i[SENV_NBINS];      /* index bande gauche par bin */
	float win[SENV_TAPS];         /* Hann */
	fftwf_plan plan;              /* c2r SENV_TAPS */
	fftwf_complex *Hbuf;
	float *hbuf;
	struct senv_chan ch[2];
};

static void senv_rebuild_fir(struct senv_state *s, struct senv_chan *c)
{
	/* enveloppe (dB) → |H| par bin (interp log-freq) → irfft → shift+win */
	for (int k = 0; k < SENV_NBINS; k++) {
		const int i = s->bin_i[k];
		const float w = s->bin_w[k];
		const float g_db = c->target_db[i] + (c->target_db[i + 1 < SENV_N ? i + 1 : i]
		                    - c->target_db[i]) * w;
		const float g = powf(10.0f, g_db / 20.0f);
		s->Hbuf[k][0] = g;
		s->Hbuf[k][1] = 0.0f;
	}
	fftwf_execute(s->plan);                        /* → hbuf[SENV_TAPS], phase 0 */
	/* normalisation irfft (FFTW c2r est non normalisée) + shift centre + win */
	const float inv_n = 1.0f / (float)SENV_TAPS;
	memcpy(c->h_old, c->h_cur, sizeof(c->h_old));
	for (int n = 0; n < SENV_TAPS; n++) {
		const int src = (n + SENV_TAPS / 2) % SENV_TAPS;   /* np.roll(h, N/2) */
		c->h_new[n] = s->hbuf[src] * inv_n * s->win[n];
	}
	c->fade_pos = 0;
}

static void senv_process_block(fx_engine_t *fx,
				const float *in_l, const float *in_r,
				float *out_l, float *out_r,
				uint32_t N)
{
	struct senv_state *s = fx->state;
	const float *ins[2] = { in_l, in_r };
	float *outs[2] = { out_l, out_r };

	for (int chn = 0; chn < 2; chn++) {
		struct senv_chan *c = &s->ch[chn];
		/* nouveaux gains ? → rebuild + démarre l'interpolation */
		if (c->target_seq != c->applied_seq) {
			c->applied_seq = c->target_seq;
			senv_rebuild_fir(s, c);
		}
		/* interpolation des taps (lissage 10 ms) */
		if (c->fade_pos < SENV_XFADE_BLOCKS) {
			c->fade_pos++;
			const float w = (float)c->fade_pos / SENV_XFADE_BLOCKS;
			for (int n = 0; n < SENV_TAPS; n++)
				c->h_cur[n] = c->h_old[n] + (c->h_new[n] - c->h_old[n]) * w;
		}
		/* convolution FIR avec delay line (overlap-save) */
		const float *x = ins[chn];
		float *y = outs[chn];
		float seg[SENV_TAPS - 1 + 256];              /* N <= 256 garanti (96) */
		memcpy(seg, c->dline, (SENV_TAPS - 1) * sizeof(float));
		memcpy(seg + SENV_TAPS - 1, x, N * sizeof(float));
		for (uint32_t i = 0; i < N; i++) {
			float acc = 0.0f;
			const float *sp = seg + i;
			const float *hp = c->h_cur;
			for (int t = 0; t < SENV_TAPS; t++)
				acc += sp[t] * hp[SENV_TAPS - 1 - t];
			y[i] = acc;
		}
		memcpy(c->dline, seg + N, (SENV_TAPS - 1) * sizeof(float));
	}
}

static int senv_set_param(fx_engine_t *fx, const char *name, float value)
{
	struct senv_state *s = fx->state;
	/* l_gN / r_gN ; "commit" sur g63 (incrémente seq → rebuild une fois) */
	int chn;
	if (name[0] == 'l' && name[1] == '_') chn = 0;
	else if (name[0] == 'r' && name[1] == '_') chn = 1;
	else return -1;
	if (name[2] != 'g') return -1;
	int b = atoi(name + 3);
	if (b < 0 || b >= SENV_N) return -1;
	struct senv_chan *c = &s->ch[chn];
	c->target_db[b] = value < -12.0f ? -12.0f : (value > 12.0f ? 12.0f : value);
	if (b == SENV_N - 1)
		c->target_seq++;            /* dernier gain du jeu → applique */
	return 0;
}

static void senv_reset(fx_engine_t *fx)
{
	struct senv_state *s = fx->state;
	for (int chn = 0; chn < 2; chn++)
		memset(s->ch[chn].dline, 0, sizeof(s->ch[chn].dline));
}

static int senv_get_state(fx_engine_t *fx, char *buf, int len)
{
	struct senv_state *s = fx->state;
	int n = snprintf(buf, len, "\"type\":\"spectral_env\",\"l\":[");
	for (int b = 0; b < SENV_N && n < len - 16; b++)
		n += snprintf(buf + n, len - n, "%s%.1f", b ? "," : "",
		              s->ch[0].target_db[b]);
	if (n < len - 8) n += snprintf(buf + n, len - n, "],\"r\":[");
	for (int b = 0; b < SENV_N && n < len - 16; b++)
		n += snprintf(buf + n, len - n, "%s%.1f", b ? "," : "",
		              s->ch[1].target_db[b]);
	if (n < len - 4) n += snprintf(buf + n, len - n, "]");
	return n;
}

int fx_init_spectral_env(fx_engine_t *fx, float sample_rate)
{
	struct senv_state *s = calloc(1, sizeof(*s));
	if (!s) return 0;
	s->sr = sample_rate;
	/* fréqs centrales log + précalcul interp par bin */
	float env_f[SENV_N];
	for (int b = 0; b < SENV_N; b++) {
		const float t = (float)b / (SENV_N - 1);
		env_f[b] = SENV_FMIN * powf(SENV_FMAX / SENV_FMIN, t);
		s->log_env_f[b] = logf(env_f[b]);
	}
	for (int k = 0; k < SENV_NBINS; k++) {
		const float f = (float)k * sample_rate / 2.0f / (SENV_NBINS - 1);
		const float lf = logf(f < 1.0f ? 1.0f : f);
		if (lf <= s->log_env_f[0]) { s->bin_i[k] = 0; s->bin_w[k] = 0.0f; }
		else if (lf >= s->log_env_f[SENV_N - 1]) {
			s->bin_i[k] = SENV_N - 1; s->bin_w[k] = 0.0f;
		} else {
			int i = 0;
			while (i < SENV_N - 2 && s->log_env_f[i + 1] < lf) i++;
			s->bin_i[k] = i;
			s->bin_w[k] = (lf - s->log_env_f[i])
			            / (s->log_env_f[i + 1] - s->log_env_f[i]);
		}
	}
	for (int n = 0; n < SENV_TAPS; n++)
		s->win[n] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * n / (SENV_TAPS - 1));
	s->Hbuf = fftwf_alloc_complex(SENV_NBINS);
	s->hbuf = fftwf_alloc_real(SENV_TAPS);
	s->plan = fftwf_plan_dft_c2r_1d(SENV_TAPS, s->Hbuf, s->hbuf, FFTW_MEASURE);
	/* gains 0 dB → FIR identité initiale, pour les 2 canaux */
	for (int chn = 0; chn < 2; chn++) {
		senv_rebuild_fir(s, &s->ch[chn]);
		memcpy(s->ch[chn].h_cur, s->ch[chn].h_new, sizeof(s->ch[chn].h_cur));
		s->ch[chn].fade_pos = SENV_XFADE_BLOCKS;
	}
	fx->type_name = "spectral_env";
	fx->state = s;
	fx->process_block = senv_process_block;
	fx->set_param = senv_set_param;
	fx->reset = senv_reset;
	fx->get_state = senv_get_state;
	return 1;
}



/* ========================================================================
 *   4d. V9.5.20 — fx_exciter_native : exciter natif (parité surrogate
 *   calibré sur le vrai Calf — fit 2026-06-10, erreur 2.15 dB).
 *
 *     high = HPF2(x, freq × 1.05, Q 0.707)     (2 biquads RBJ cascadés)
 *     wet  = tanh(drive × high)
 *     y    = x + amount × β(drive) × wet,  β = 1.15 / (1 + 0.7·drive)
 *     y    = ceil × tanh(y / ceil)
 *
 *   Params (mêmes noms que Calf pour compat daemon) : amount, drive,
 *   freq, ceil. Lock-free : floats écrits par le control_thread, biquads
 *   recalculés dans process_block sur changement de freq.
 *   Coût ≈ 30 µs / période (96 frames × 2 ch).
 * ====================================================================== */

#define EXC_CAL_FREQ_MULT 1.05f
#define EXC_CAL_B0        1.15f
#define EXC_CAL_B1        0.70f

struct exciter_state {
	float sr;
	int   bypass;                  /* M/A GUI : 1 = traverse sans effet */
	float amount, drive, freq, ceil;
	float cfg_freq;                 /* freq des biquads courants */
	struct biquad hp1, hp2;        /* HPF ordre 2 (état stéréo dans biquad) */
};

static void exciter_recalc(struct exciter_state *e)
{
	/* RBJ highpass, freq × calibration, Q 0.707 */
	const float f = e->freq * EXC_CAL_FREQ_MULT;
	const float w = 2.0f * (float)M_PI * f / e->sr;
	const float cw = cosf(w), sw = sinf(w);
	const float alpha = sw / (2.0f * 0.707f);
	const float b0 = (1.0f + cw) / 2.0f, b1 = -(1.0f + cw), b2 = (1.0f + cw) / 2.0f;
	const float a0 = 1.0f + alpha, a1 = -2.0f * cw, a2 = 1.0f - alpha;
	e->hp1.b0 = b0 / a0; e->hp1.b1 = b1 / a0; e->hp1.b2 = b2 / a0;
	e->hp1.a1 = a1 / a0; e->hp1.a2 = a2 / a0;
	e->hp2.b0 = e->hp1.b0; e->hp2.b1 = e->hp1.b1; e->hp2.b2 = e->hp1.b2;
	e->hp2.a1 = e->hp1.a1; e->hp2.a2 = e->hp1.a2;
	e->cfg_freq = e->freq;
}

static void exciter_process_block(fx_engine_t *fx,
				   const float *in_l, const float *in_r,
				   float *out_l, float *out_r,
				   uint32_t N)
{
	struct exciter_state *e = fx->state;
	if (e->bypass) {
		if (out_l != in_l) memcpy(out_l, in_l, N * sizeof(float));
		if (out_r != in_r) memcpy(out_r, in_r, N * sizeof(float));
		return;
	}
	if (e->cfg_freq != e->freq)
		exciter_recalc(e);
	const float a = e->amount;
	const float d = e->drive;
	const float beta = EXC_CAL_B0 / (1.0f + EXC_CAL_B1 * d);
	const float cl = e->ceil < 1e-6f ? 1e-6f : e->ceil;
	const float ab = a * beta;
	for (uint32_t s = 0; s < N; s++) {
		const float xl = in_l[s], xr = in_r[s];
		float hl = biquad_step(&e->hp2, 0, biquad_step(&e->hp1, 0, xl));
		float hr = biquad_step(&e->hp2, 1, biquad_step(&e->hp1, 1, xr));
		const float yl = xl + ab * tanhf(d * hl);
		const float yr = xr + ab * tanhf(d * hr);
		out_l[s] = cl * tanhf(yl / cl);
		out_r[s] = cl * tanhf(yr / cl);
	}
}

static int exciter_set_param(fx_engine_t *fx, const char *name, float value)
{
	struct exciter_state *e = fx->state;
	if      (!strcmp(name, "bypass")) e->bypass = value > 0.5f;
	else if (!strcmp(name, "amount")) e->amount = CLAMP(value, 0.0f, 1.0f);
	else if (!strcmp(name, "drive"))  e->drive  = CLAMP(value, 0.1f, 10.0f);
	else if (!strcmp(name, "freq"))   e->freq   = CLAMP(value, 1000.0f, 16000.0f);
	else if (!strcmp(name, "ceil"))   e->ceil   = CLAMP(value, 0.1f, 1.0f);
	else return -1;
	return 0;
}

static void exciter_reset(fx_engine_t *fx)
{
	struct exciter_state *e = fx->state;
	memset(&e->hp1.x1, 0, 8 * sizeof(float));
	memset(&e->hp2.x1, 0, 8 * sizeof(float));
}

static int exciter_get_state(fx_engine_t *fx, char *buf, int len)
{
	struct exciter_state *e = fx->state;
	return snprintf(buf, len,
		"\"type\":\"exciter_native\",\"bypass\":%d,\"amount\":%.4f,"
		"\"drive\":%.2f,\"freq\":%.1f,\"ceil\":%.4f",
		e->bypass, e->amount, e->drive, e->freq, e->ceil);
}

int fx_init_exciter_native(fx_engine_t *fx, float sample_rate)
{
	struct exciter_state *e = calloc(1, sizeof(*e));
	if (!e) return 0;
	e->sr = sample_rate;
	e->amount = 0.0f; e->drive = 1.0f; e->freq = 8000.0f; e->ceil = 1.0f;
	exciter_recalc(e);
	fx->type_name = "exciter_native";
	fx->state = e;
	fx->process_block = exciter_process_block;
	fx->set_param = exciter_set_param;
	fx->reset = exciter_reset;
	fx->get_state = exciter_get_state;
	return 1;
}

