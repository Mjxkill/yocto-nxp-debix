// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * voice — VFOCUS + VOICE SPATIALIZER (voir voice.h).
 * Code déplacé tel quel depuis mixer-pro.c (V14.0 étape 2, extraction pure).
 * Seule adaptation : structs anonymes g_vf/g_vspat nommées (vf_state /
 * vspat_state) pour les externs — initialisations identiques.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>

#include "state.h"       /* g_st (faders, mute, gains composés) */
#include "dsp_bq.h"      /* rbj_peak_core */
#include "dsp_block.h"   /* mac_block_n4 (somme voix NEON) */
#include "automix.h"     /* g_bmx.role (sidechain voix / cibles musique) */
#include "voice.h"
#include "control.h"    /* handlers d'ops (V14.0 étape 4) */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ========= V13-VFOCUS — « place à la voix » (unmasking spectral) =========
 * Dynamic EQ sidechainé : la musique (tranches rôle instrument du
 * bandmix) est creusée UNIQUEMENT dans les bandes où la voix (tranches
 * rôle lead/choir) a de l'énergie, UNIQUEMENT quand elle chante.
 * 5 bandes peaking RBJ fixes (250/500/1k/2k/4k, Q 1,4) : analyse =
 * passe-bande fixes sur le sidechain voix (post-fader) ; application =
 * MÊMES 5 gains pour toutes les tranches musique → coefs recalculés UNE
 * fois par bloc, 5 biquads cascade par tranche (états par tranche×bande).
 * Zéro alloc, zéro transcendante par sample. ARCHI_V13_VOICEFOCUS.md. */

/* L'ancienne « struct vf_bq » (champs identiques) a été fusionnée dans
 * struct eqx_bq (revue 2026-07-28, lot 5b) : UN seul type de biquad RBJ
 * dans tout le moteur. */

/* champs : voir struct vf_state (voice.h) */
struct vf_state g_vf = { .amount = 0.5f, .max_cut_db = 4.5f };

void vf_init(void)
{
	static const float FR[VF_BANDS] = { 250, 500, 1000, 2000, 4000 };
	for (int b = 0; b < VF_BANDS; b++) {
		float w = 2.0f * (float)M_PI * FR[b] / (float)SAMPLE_RATE;
		float sw = sinf(w);
		g_vf.cw[b] = cosf(w);
		g_vf.alpha[b] = sw / (2.0f * 1.4f);   /* Q = 1,4 */
		/* passe-bande RBJ (pic 0 dB) */
		float a0 = 1.0f + g_vf.alpha[b];
		g_vf.ana[b].b0 = g_vf.alpha[b] / a0;
		g_vf.ana[b].b1 = 0.0f;
		g_vf.ana[b].b2 = -g_vf.alpha[b] / a0;
		g_vf.ana[b].a1 = -2.0f * g_vf.cw[b] / a0;
		g_vf.ana[b].a2 = (1.0f - g_vf.alpha[b]) / a0;
		g_vf.cut[b] = (struct eqx_bq){ 1, 0, 0, 0, 0 };   /* neutre */
	}
}

/* peaking RBJ, gain −cut_db — même NOYAU rbj_peak_core que l'eqx (lot 5b),
 * avec cos/alpha précalculés à l'init → qq mults par bloc (chemin RT) */
static inline void vf_peak_coefs(int b, float cut_db)
{
	rbj_peak_core(&g_vf.cut[b], g_vf.cw[b], g_vf.alpha[b],
		      powf(10.0f, -cut_db / 40.0f));
}

static inline int vf_is_voice(int i)
{
	return g_bmx.role[i] == BR_LEAD || g_bmx.role[i] == BR_CHOIR;
}
static inline int vf_is_music(int i)
{
	int r = g_bmx.role[i];
	return r >= BR_KICK && r <= BR_LINE;
}

/* Rendu (audio_thread, SOUS target_lock, après cmp_render) */
void duck_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	if (!g_vf.on) {
		if (atomic_load_explicit(&g_vf.active, memory_order_relaxed))
			atomic_store(&g_vf.active, 0);
		return;
	}

	/* 1. sidechain voix = somme post-fader des tranches lead/choir */
	static float sc[PERIOD_FRAMES];
	memset(sc, 0, sizeof(sc));
	int nvoice = 0;
	for (int i = 0; i < N_EXP_CH; i++) {
		if (!vf_is_voice(i))
			continue;
		nvoice++;
		const float g = g_st.input_gain[i];
		for (int f = 0; f < PERIOD_FRAMES; f++)
			sc[f] += in_block[i][f] * g;
	}

	/* 2. enveloppes : large bande + par bande (crête, att 5 ms/rel 180) */
	float pk_wb = 0.0f, pk_b[VF_BANDS] = { 0 };
	if (nvoice) {
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			float v = sc[f] < 0 ? -sc[f] : sc[f];
			if (v > pk_wb) pk_wb = v;
		}
		for (int b = 0; b < VF_BANDS; b++) {
			const struct eqx_bq *q = &g_vf.ana[b];
			float z1 = g_vf.az[b][0], z2 = g_vf.az[b][1];
			float m = 0.0f;
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float x = sc[f];
				float y = q->b0 * x + z1;
				z1 = q->b1 * x - q->a1 * y + z2;
				z2 = q->b2 * x - q->a2 * y;
				float v = y < 0 ? -y : y;
				if (v > m) m = v;
			}
			g_vf.az[b][0] = z1; g_vf.az[b][1] = z2;
			pk_b[b] = m;
		}
	}
	const float ka = 0.33f, kr = 0.011f;   /* 5 ms / 180 ms (blocs 2 ms) */
	g_vf.env_wb += (pk_wb > g_vf.env_wb ? ka : kr) * (pk_wb - g_vf.env_wb);
	float emax = 1e-12f;
	for (int b = 0; b < VF_BANDS; b++) {
		g_vf.env[b] += (pk_b[b] > g_vf.env[b] ? ka : kr)
			       * (pk_b[b] - g_vf.env[b]);
		if (g_vf.env[b] > emax) emax = g_vf.env[b];
	}

	/* 3. cuts cibles : voix active → proportionnel à la bande dominante */
	int act = g_vf.env_wb > 0.005623f;   /* −45 dBFS */
	atomic_store_explicit(&g_vf.active, act, memory_order_relaxed);
	const float kca = 0.18f, kcr = 0.01f;   /* 10 ms / 200 ms */
	int any = 0;
	for (int b = 0; b < VF_BANDS; b++) {
		float tgt = act ? g_vf.max_cut_db * g_vf.amount
				  * (g_vf.env[b] / emax) : 0.0f;
		g_vf.cut_db[b] += (tgt > g_vf.cut_db[b] ? kca : kcr)
				  * (tgt - g_vf.cut_db[b]);
		if (g_vf.cut_db[b] > 0.05f) {
			vf_peak_coefs(b, g_vf.cut_db[b]);
			any = 1;
		}
		atomic_store_explicit(&g_vf.pub_cut[b],
				      (uint32_t)(g_vf.cut_db[b] * 1000.0f),
				      memory_order_relaxed);
	}
	if (!any || !nvoice)
		return;

	/* 4. application : 5 peaking cascade sur les tranches musique */
	for (int i = 0; i < N_EXP_CH; i++) {
		if (!vf_is_music(i))
			continue;
		float *x = in_block[i];
		for (int b = 0; b < VF_BANDS; b++) {
			if (g_vf.cut_db[b] <= 0.05f)
				continue;
			const struct eqx_bq *q = &g_vf.cut[b];
			float z1 = g_vf.st[i][b][0], z2 = g_vf.st[i][b][1];
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float xi = x[f];
				float y = q->b0 * xi + z1;
				z1 = q->b1 * xi - q->a1 * y + z2;
				z2 = q->b2 * xi - q->a2 * y;
				x[f] = y;
			}
			g_vf.st[i][b][0] = z1; g_vf.st[i][b][1] = z2;
		}
	}
}

/* V12-MIDIX — expandeur MIDI : déplacé dans midix.c/midix.h (V14.0 étape 1). */


/* ===== V13.9 — VOICE SPATIALIZER : widener décorrélé (Lauridsen) =====
 * Élargit la voix (rôles LEAD + CHŒURS) sans la décentrer : on somme la voix
 * telle qu'elle apparaît au master (mono, centre), on en dérive un « side »
 * décorrélé = copie retardée (~18 ms), et on l'injecte ±dans out0/out1 APRÈS
 * mix_block et AVANT l'EQ/limiter master. Mono-compatible (out0+out1 annule le
 * side → repli mono = mix d'origine). amount=0 → strictement transparent.
 * RT : somme voix en NEON (mac_block_n4), boucle retard/inject légère (96 it). */
/* champs : voir struct vspat_state (voice.h) */
struct vspat_state g_vspat = { .amount_mq = 500,
	      .delay_smp = (VSPAT_DLY_DEF * SAMPLE_RATE) / 1000 };

void vspat_render(const float in_block[N_INPUT_REAL][PERIOD_FRAMES],
			 float out_block[N_OUTPUT_TOTAL][PERIOD_FRAMES], int N)
{
	int on = atomic_load_explicit(&g_vspat.on, memory_order_relaxed);
	float tgt = on ? atomic_load_explicit(&g_vspat.amount_mq,
					      memory_order_relaxed) / 1000.0f : 0.0f;
	/* court-circuit total quand inactif ET déjà éteint (transparent) */
	if (tgt == 0.0f && g_vspat.amount_cur < 1e-4f) {
		g_vspat.amount_cur = 0.0f;
		return;
	}
	int dly = atomic_load_explicit(&g_vspat.delay_smp, memory_order_relaxed);
	if (dly < 1) dly = 1;
	if (dly > VSPAT_RING - PERIOD_FRAMES) dly = VSPAT_RING - PERIOD_FRAMES;

	/* voix telle qu'elle sort au master (mono, centre) : somme des voies
	 * LEAD/CHŒURS avec leur gain effectif vers out0 — accumulation NEON. */
	static float vbus[PERIOD_FRAMES];
	memset(vbus, 0, sizeof(float) * N);
	for (int s = 0; s < N_EXP_CH; s++) {
		int r = g_bmx.role[s];
		if (r != BR_LEAD && r != BR_CHOIR) continue;
		if (g_st.mute_mask & (1u << s)) continue;
		float ig = g_st.input_gain[s] * g_st.automix_gain[s]
			 * g_st.keeper_gain[s] * g_st.master_gain[s][0];
		if (ig == 0.0f) continue;
		mac_block_n4(vbus, in_block[s], ig, N);
	}

	/* retard + injection ±side (séquentiel à cause du ring, mais 96 it).
	 * NEUTRE EN LOUDNESS : on réduit la voix sèche de (1−c) tout en ajoutant
	 * le side ±a·vd, avec c=√(1−a²) → puissance de la voix PAR CANAL
	 * constante (c²+a²=1). On gagne la largeur sans monter le niveau voix.
	 * (a=0 → c=1 : strictement transparent). */
	const float ka = 1.0f / (0.02f * SAMPLE_RATE);   /* slew ~20 ms */
	float *o0 = out_block[0], *o1 = out_block[1];
	for (int f = 0; f < N; f++) {
		g_vspat.ring[g_vspat.wpos] = vbus[f];
		int rp = g_vspat.wpos - dly;
		if (rp < 0) rp += VSPAT_RING;
		float vd = g_vspat.ring[rp];
		if (++g_vspat.wpos >= VSPAT_RING) g_vspat.wpos = 0;

		g_vspat.amount_cur += (tgt - g_vspat.amount_cur) * ka;
		float a = g_vspat.amount_cur;
		float c = sqrtf(1.0f - a * a);       /* a∈[0,1] → c∈[1,0] */
		float dry  = (c - 1.0f) * vbus[f];   /* retire (1−c) de la voix sèche */
		float side = a * vd;
		o0[f] += dry + side;
		o1[f] += dry - side;
	}
}


/* V14.0 étape 4 : ops du module — appelées par le dispatcher control.
 * Corps déplacés tels quels depuis handle_cmd (extraction pure) ;
 * retourne 1 si l'op est traitée, 0 sinon. */
int voice_handle_op(int fd, const char *line)
{
	if (json_has_op(line, "set_vfocus")) {
		/* V13-VFOCUS : {"op":"set_vfocus", on?, amount?(0-100),
		 * max_cut_db?} — updates partiels */
		int on = g_vf.on;
		float am = -1.0f, mc = -1.0f;
		(void)json_get_int(line, "on", &on);
		(void)json_get_float(line, "amount", &am);
		(void)json_get_float(line, "max_cut_db", &mc);
		pthread_mutex_lock(&g_st.target_lock);
		g_vf.on = on ? 1 : 0;
		if (am >= 0.0f && am <= 100.0f)
			g_vf.amount = am / 100.0f;
		if (mc >= 0.0f && mc <= 12.0f)
			g_vf.max_cut_db = mc;
		pthread_mutex_unlock(&g_st.target_lock);
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_vfocus\",\"on\":%d}\n",
			g_vf.on);
		return 1;
	}
	if (json_has_op(line, "get_vfocus")) {
		int n = snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			"{\"ok\":true,\"on\":%d,\"amount\":%.0f,"
			"\"max_cut_db\":%.1f,\"active\":%d,\"cuts_db\":[",
			g_vf.on, g_vf.amount * 100.0f, g_vf.max_cut_db,
			atomic_load_explicit(&g_vf.active,
					     memory_order_relaxed));
		for (int b = 0; b < VF_BANDS; b++)
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%.2f",
				      b ? "," : "",
				      atomic_load_explicit(&g_vf.pub_cut[b],
							   memory_order_relaxed)
					/ 1000.0f);
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "]}\n");
		write(fd, g_ctl_reply, n);
		return 1;
	}
	if (json_has_op(line, "set_vspatial")) {
		/* V13.9 — spatializer voix (widener Lauridsen LEAD+CHŒURS) :
		 * {"op":"set_vspatial","on":0/1,"amount":0..100,"delay_ms":3..40}
		 * champs absents = inchangés ; toujours renvoie l'état courant. */
		int iv; float v; int chg = 0;
		if (json_get_int(line, "on", &iv) >= 0) {
			atomic_store_explicit(&g_vspat.on, iv ? 1 : 0,
					      memory_order_relaxed);
			chg = 1;
		}
		if (json_get_float(line, "amount", &v) >= 0 && v >= 0.0f && v <= 100.0f) {
			atomic_store_explicit(&g_vspat.amount_mq, (int)(v * 10.0f + 0.5f),
					      memory_order_relaxed);
			chg = 1;
		}
		if (json_get_float(line, "delay_ms", &v) >= 0 && v >= 3.0f && v <= 40.0f) {
			atomic_store_explicit(&g_vspat.delay_smp,
					      (int)(v * SAMPLE_RATE / 1000.0f),
					      memory_order_relaxed);
			chg = 1;
		}
		if (chg)   /* pollé en lecture par la GUI : dirty SEULEMENT si set */
			atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_vspatial\",\"on\":%d,"
			"\"amount\":%.0f,\"delay_ms\":%.1f}\n",
			atomic_load_explicit(&g_vspat.on, memory_order_relaxed),
			atomic_load_explicit(&g_vspat.amount_mq, memory_order_relaxed) / 10.0,
			atomic_load_explicit(&g_vspat.delay_smp, memory_order_relaxed)
				* 1000.0 / SAMPLE_RATE);
		return 1;
	}
	return 0;
}
