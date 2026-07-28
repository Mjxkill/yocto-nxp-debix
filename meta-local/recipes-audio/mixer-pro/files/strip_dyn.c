// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * strip_dyn — dynamique par tranche + lien stéréo (voir strip_dyn.h).
 * Code déplacé tel quel depuis mixer-pro.c (V14.0 étape 2, extraction pure).
 */
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>

#include "strip_dyn.h"

/* ============ V13.3 : LIEN STÉRÉO de paires de tranches ============
 * Paires fixes (2k, 2k+1) sur les 16 tranches réelles. Une paire liée :
 * les écritures fader/mute/gate/comp/automix sur UNE tranche s'appliquent
 * aux DEUX (miroir dans les handlers socket — jamais dans l'audio).
 * Les sends ne sont PAS miroirés (pattern stéréo posé par les GUIs).
 * Voir docs/ARCHI/ARCHI_V13.3_STEREO_LINK.md */
_Atomic int g_link[N_LINK_PAIRS];
/* link_partner : inline dans strip_dyn.h */

/* ========= V12-EXP — expandeur/gate par tranche (16 voies réelles) =========
 * Downward expander in-place sur in_block[0..15], AVANT smp/loop/automix/
 * mix : le gate s'applique à tout l'aval (sends, master, looper, automix,
 * tap NPU) — une seule vérité du signal de tranche. P1/P2 exclues.
 * Enveloppe crête par bloc (2 ms), coefs attack/release PRÉCALCULÉS à la
 * config (jamais d'expf en RT), hold anti-chatter (granularité 1 bloc),
 * rampe de gain linéaire intra-bloc (zipper-free), GR publié en atomic
 * pour la GUI. off = zéro coût. ARCHI_V12_EXPANDER.md. */

struct exp_ch g_exp[N_EXP_CH];

/* Précalculs — control thread (handler socket / load state), SOUS
 * target_lock quand le daemon tourne. Valide et clampe les plages. */
void exp_configure(int src, int on, float thr_db, float ratio,
			  float atk_ms, float rel_ms, float range_db,
			  float hold_ms)
{
	if (src < 0 || src >= N_EXP_CH)
		return;
	struct exp_ch *e = &g_exp[src];
	if (thr_db < -80.0f) thr_db = -80.0f;
	if (thr_db > 0.0f)   thr_db = 0.0f;
	if (ratio < 1.0f)    ratio = 1.0f;
	if (ratio > 20.0f)   ratio = 20.0f;
	if (atk_ms < 0.5f)   atk_ms = 0.5f;
	if (atk_ms > 100.0f) atk_ms = 100.0f;
	if (rel_ms < 5.0f)   rel_ms = 5.0f;
	if (rel_ms > 1000.0f) rel_ms = 1000.0f;
	if (range_db < 0.0f)  range_db = 0.0f;
	if (range_db > 80.0f) range_db = 80.0f;
	if (hold_ms < 0.0f)   hold_ms = 0.0f;
	if (hold_ms > 500.0f) hold_ms = 500.0f;
	e->thr_db = thr_db;  e->ratio = ratio;  e->range_db = range_db;
	e->atk_ms = atk_ms;  e->rel_ms = rel_ms; e->hold_ms = hold_ms;
	e->thr_lin = powf(10.0f, thr_db / 20.0f);
	e->ka = 1.0f - expf(-2.0f / atk_ms);
	e->kr = 1.0f - expf(-2.0f / rel_ms);
	e->hold_blocks = (int)(hold_ms / 2.0f);
	e->on = on ? 1 : 0;
	if (!e->on) {   /* off : état neutre, aucun résidu à la réactivation */
		e->env = 0.0f; e->gain = 1.0f; e->hold_cnt = 0;
		atomic_store_explicit(&e->gr_mdb, 0, memory_order_relaxed);
	}
}

/* Rendu (audio_thread, SOUS target_lock, juste après le convert S32→float) */
void exp_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	for (int i = 0; i < N_EXP_CH; i++) {
		struct exp_ch *e = &g_exp[i];
		if (!e->on)
			continue;

		/* 1. crête du bloc */
		float p = 0.0f;
		const float *x = in_block[i];
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			float v = x[f] < 0 ? -x[f] : x[f];
			if (v > p) p = v;
		}
		/* 2. enveloppe asymétrique (coefs précalculés) */
		e->env += (p > e->env ? e->ka : e->kr) * (p - e->env);

		/* 3. hold anti-chatter */
		if (e->env >= e->thr_lin)
			e->hold_cnt = e->hold_blocks;
		else if (e->hold_cnt > 0)
			e->hold_cnt--;

		/* 4. gain cible */
		float g_db = 0.0f;
		if (e->env < e->thr_lin && e->hold_cnt == 0) {
			float env_db = 20.0f * log10f(e->env + 1e-10f);
			g_db = (env_db - e->thr_db) * (e->ratio - 1.0f);
			if (g_db < -e->range_db)
				g_db = -e->range_db;
		}
		float gt = (g_db >= 0.0f) ? 1.0f : powf(10.0f, g_db / 20.0f);
		atomic_store_explicit(&e->gr_mdb,
				      (uint32_t)(-g_db * 1000.0f),
				      memory_order_relaxed);

		/* 5. rampe linéaire gain_prev → gain (zipper-free) */
		float g0 = e->gain;
		float step = (gt - g0) / (float)PERIOD_FRAMES;
		float *y = in_block[i];
		float g = g0;
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			g += step;
			y[f] *= g;
		}
		e->gain = gt;
	}
}

/* ========= V13-COMP — compresseur natif par tranche (16 voies) =========
 * Downward compressor in-place sur in_block[0..15], APRÈS le gate
 * (exp_render) : ordre console standard gate→comp. Même patron RT que le
 * gate : enveloppe crête/bloc 2 ms, coefs précalculés au set, rampe de
 * gain intra-bloc, GR atomic, off = zéro coût. Prérequis de l'assistant
 * V13-BANDMIX (les tranches USB n'ont pas de DRC DSP).
 * ARCHI_V13_BANDMIX.md. */
struct cmp_ch g_cmp[N_EXP_CH];

void cmp_configure(int src, int on, float thr_db, float ratio,
			  float atk_ms, float rel_ms, float makeup_db)
{
	if (src < 0 || src >= N_EXP_CH)
		return;
	struct cmp_ch *c = &g_cmp[src];
	if (thr_db < -60.0f) thr_db = -60.0f;
	if (thr_db > 0.0f)   thr_db = 0.0f;
	if (ratio < 1.0f)    ratio = 1.0f;
	if (ratio > 20.0f)   ratio = 20.0f;
	if (atk_ms < 0.5f)   atk_ms = 0.5f;
	if (atk_ms > 250.0f) atk_ms = 250.0f;
	if (rel_ms < 5.0f)   rel_ms = 5.0f;
	if (rel_ms > 2000.0f) rel_ms = 2000.0f;
	if (makeup_db < 0.0f)  makeup_db = 0.0f;
	if (makeup_db > 24.0f) makeup_db = 24.0f;
	c->thr_db = thr_db;  c->ratio = ratio;  c->makeup_db = makeup_db;
	c->atk_ms = atk_ms;  c->rel_ms = rel_ms;
	c->thr_lin = powf(10.0f, thr_db / 20.0f);
	c->ka = 1.0f - expf(-2.0f / atk_ms);
	c->kr = 1.0f - expf(-2.0f / rel_ms);
	c->makeup_lin = powf(10.0f, makeup_db / 20.0f);
	c->on = on ? 1 : 0;
	if (!c->on) {
		/* extinction : si le comp jouait (gain réduit), on RAMPE vers 1 via
		 * cmp_render (pas de saut = pas de clic). Si jamais lancé (gain≤0 au
		 * boot) ou déjà à l'unité, on fige direct. */
		if (c->gain <= 0.0f || c->gain == 1.0f) {
			c->env = 0.0f; c->gain = 1.0f; c->releasing = 0;
			atomic_store_explicit(&c->gr_mdb, 0, memory_order_relaxed);
		} else {
			c->releasing = 1;
		}
	} else {
		c->releasing = 0;
	}
}

/* Rendu (audio_thread, SOUS target_lock, juste après exp_render) */
/* NOTE (revue 2026-07-28, lot 5b) : ce compresseur de TRANCHE n'est PAS un
 * doublon de fx_init_compressor (effects.c) — algorithmes distincts à
 * dessein : ici crête PAR BLOC + loi de gain en dB + rampe de gain intra-
 * bloc anti-zipper + gr publié GUI + extinction douce (16 voies mono,
 * seuils adaptatifs bmx) ; effects.c = enveloppe PAR ÉCHANTILLON + loi
 * linéaire (insert stéréo master, params fixes). Les fusionner changerait
 * le son validé des deux. */
void cmp_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	for (int i = 0; i < N_EXP_CH; i++) {
		struct cmp_ch *c = &g_cmp[i];
		if (!c->on && !c->releasing)
			continue;
		float gt;
		if (c->on) {
			float p = 0.0f;
			const float *x = in_block[i];
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float v = x[f] < 0 ? -x[f] : x[f];
				if (v > p) p = v;
			}
			c->env += (p > c->env ? c->ka : c->kr) * (p - c->env);

			float g_db = 0.0f;
			if (c->env > c->thr_lin) {
				float env_db = 20.0f * log10f(c->env + 1e-10f);
				g_db = (c->thr_db - env_db) * (1.0f - 1.0f / c->ratio);
			}
			gt = powf(10.0f, g_db / 20.0f) * c->makeup_lin;
			atomic_store_explicit(&c->gr_mdb,
					      (uint32_t)(-g_db * 1000.0f),
					      memory_order_relaxed);
		} else {
			gt = 1.0f;   /* releasing : cible unité, ramp doux vers 1 */
			atomic_store_explicit(&c->gr_mdb, 0, memory_order_relaxed);
		}

		float g0 = c->gain;
		float step = (gt - g0) / (float)PERIOD_FRAMES;
		float *y = in_block[i];
		float g = g0;
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			g += step;
			y[f] *= g;
		}
		c->gain = gt;
		if (c->releasing && fabsf(gt - 1.0f) < 1e-3f) {
			c->releasing = 0;   /* extinction terminée */
			c->env = 0.0f;
		}
	}
}

