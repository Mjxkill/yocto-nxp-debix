// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * looper — V12-LOOP-PRO : loopstation multipiste (voir looper.h).
 * Code déplacé tel quel depuis mixer-pro.c (V14.0 étape 1, extraction pure).
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "state.h"     /* g_st.input_gain / automix_gain / keeper_gain */
#include "util.h"      /* mlog */
#include "looper.h"

const char *const TR_NAMES[] = { "empty", "rec", "play", "armed" };

struct loop_track g_tr[LOOP_TRACKS];
_Atomic uint32_t  g_master_len;  /* 0 tant qu'aucune piste posée */
_Atomic uint32_t  g_lpos;        /* position globale (frames) */
_Atomic int       g_loop_run;    /* transport global (0=stop, 1=play) */
_Atomic uint32_t  g_loop_mpeak;  /* crête master (somme des pistes) */

/* V12-LOOP-PRO : buffers loopstation — LOOP_TRACKS × 40 s stéréo
 * alloués au démarrage (main, AVANT les threads — jamais en RT). */
void loop_init(void)
{
	for (int t = 0; t < LOOP_TRACKS; t++) {
		g_tr[t].buf   = calloc((size_t)LOOP_MAX_FRAMES * 2, sizeof(float));
		g_tr[t].src_a = t < N_INPUT_MICS ? t : 0;
		g_tr[t].src_b = -1;
		g_tr[t].gain  = 1.0f;
		atomic_store(&g_tr[t].rec_start, REC_START_NONE);
		if (!g_tr[t].buf)
			mlog("loop: alloc piste %d ÉCHEC — looper dégradé", t);
	}
}

/* Rendu (audio_thread, SOUS target_lock, après le convert S32→float) */
void loop_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	const int P = N_INPUT_MICS + N_INPUT_STEMS;   /* P1 = 16 */
	uint32_t mlen = atomic_load_explicit(&g_master_len, memory_order_acquire);
	uint32_t lpos = atomic_load_explicit(&g_lpos, memory_order_relaxed);
	int run = atomic_load_explicit(&g_loop_run, memory_order_relaxed);
	int any_rec = 0, any_play = 0;

	/* V12-VU : les pistes s'additionnent dans sl/sr (BSS), ajoutés en un
	 * passage dans P1/P2 après la boucle → crête MASTER looper mesurable
	 * (somme des pistes seules, pas polluée par sampleur/expandeur). */
	static float sl[PERIOD_FRAMES], sr[PERIOD_FRAMES];
	memset(sl, 0, sizeof(sl));
	memset(sr, 0, sizeof(sr));

	/* V13.2 : premier bloc d'un nouveau tour de boucle (lpos vient de
	 * wrapper → ∈ [0, PERIOD_FRAMES)) : les pistes ARMÉES démarrent ici. */
	const int boundary = (run && mlen && lpos < PERIOD_FRAMES);

	for (int t = 0; t < LOOP_TRACKS; t++) {
		struct loop_track *tr = &g_tr[t];
		int st = atomic_load_explicit(&tr->state, memory_order_acquire);
		if (st == TR_ARMED) {
			if (!boundary)
				continue;
			atomic_store_explicit(&tr->state, TR_REC,
					      memory_order_release);
			st = TR_REC;   /* rec_start capturé ci-dessous (≈0) */
		}
		if (st != TR_REC && st != TR_PLAY)
			continue;
		if (!tr->buf)
			continue;

		const int sa = tr->src_a;
		const int sb = tr->src_b >= 0 ? tr->src_b : tr->src_a;
		const float ga = g_st.input_gain[sa] * g_st.automix_gain[sa]
				 * g_st.keeper_gain[sa];
		const float gb = g_st.input_gain[sb] * g_st.automix_gain[sb]
				 * g_st.keeper_gain[sb];

		if (st == TR_REC) {
			any_rec = 1;
			/* V12-VU : crête de l'ENTRÉE enregistrée (visible en REC) */
			float rpk = 0.0f;
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float a = in_block[sa][f] * ga, b = in_block[sb][f] * gb;
				a = a < 0 ? -a : a; b = b < 0 ? -b : b;
				if (b > a) a = b;
				if (a > rpk) rpk = a;
			}
			atomic_store_explicit(&tr->peak,
					      (uint32_t)(rpk * 2147483647.0f),
					      memory_order_relaxed);
			if (mlen == 0) {
				/* piste MAÎTRE : REC libre → définit master_len */
				uint32_t h = tr->rec_head;
				for (int f = 0; f < PERIOD_FRAMES && h < LOOP_MAX_FRAMES; f++, h++) {
					tr->buf[(size_t)h * 2]     = in_block[sa][f] * ga;
					tr->buf[(size_t)h * 2 + 1] = in_block[sb][f] * gb;
				}
				tr->rec_head = h;
				if (h >= LOOP_MAX_FRAMES) {   /* plafond → fige */
					atomic_store_explicit(&tr->len, h, memory_order_release);
					atomic_store(&tr->state, TR_PLAY);
					atomic_store(&g_master_len, h);
					atomic_store(&g_lpos, 0);
					atomic_store(&g_loop_run, 1);
					mlen = h; lpos = 0; run = 1;
				}
			} else {
				/* piste ALIGNÉE : écrit à (rec_start+rec_done)%mlen,
				 * un tour complet puis PLAY. rec_start capturé ICI
				 * (audio) au 1er bloc → pas de décalage socket. */
				uint32_t rs = atomic_load_explicit(&tr->rec_start,
								   memory_order_relaxed);
				if (rs == REC_START_NONE) {
					rs = lpos;
					atomic_store_explicit(&tr->rec_start, rs,
							      memory_order_relaxed);
				}
				uint32_t d = tr->rec_done;
				for (int f = 0; f < PERIOD_FRAMES && d < mlen; f++, d++) {
					uint32_t idx = (rs + d) % mlen;
					tr->buf[(size_t)idx * 2]     = in_block[sa][f] * ga;
					tr->buf[(size_t)idx * 2 + 1] = in_block[sb][f] * gb;
				}
				tr->rec_done = d;
				if (d >= mlen) {   /* tour complet → couche posée */
					atomic_store_explicit(&tr->len, mlen,
							      memory_order_release);
					atomic_store(&tr->state, TR_PLAY);
				}
			}
			continue;   /* une piste en REC ne se relit pas ce bloc */
		}

		/* TR_PLAY : lecture additionnée dans sl/sr si non-mutée */
		uint32_t len = atomic_load_explicit(&tr->len, memory_order_relaxed);
		if (!len || !run || atomic_load_explicit(&tr->muted, memory_order_relaxed)) {
			atomic_store_explicit(&tr->peak, 0, memory_order_relaxed);
			continue;
		}
		any_play = 1;
		const float g = tr->gain;
		uint32_t pk = 0;
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			uint32_t idx = (lpos + f) % len;
			float l = tr->buf[(size_t)idx * 2];
			float r = tr->buf[(size_t)idx * 2 + 1];
			sl[f] += l * g;
			sr[f] += r * g;
			float a = l < 0 ? -l : l, b = r < 0 ? -r : r;
			if (a > b) b = a;
			uint32_t v = (uint32_t)(b * g * 2147483647.0f);
			if (v > pk) pk = v;
		}
		atomic_store_explicit(&tr->peak, pk, memory_order_relaxed);
	}

	/* V12-VU : ajout de la somme dans P1/P2 + crête MASTER looper */
	if (any_play) {
		float mpk = 0.0f;
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			in_block[P][f]     += sl[f];
			in_block[P + 1][f] += sr[f];
			float a = sl[f] < 0 ? -sl[f] : sl[f];
			float b = sr[f] < 0 ? -sr[f] : sr[f];
			if (b > a) a = b;
			if (a > mpk) mpk = a;
		}
		atomic_store_explicit(&g_loop_mpeak,
				      (uint32_t)(mpk * 2147483647.0f),
				      memory_order_relaxed);
	} else {
		atomic_store_explicit(&g_loop_mpeak, 0, memory_order_relaxed);
	}

	/* Avance g_lpos UNE fois par bloc (partagée par toutes les pistes) */
	if (run && mlen) {
		atomic_store_explicit(&g_lpos, (lpos + PERIOD_FRAMES) % mlen,
				      memory_order_relaxed);
	} else if (mlen == 0 && any_rec) {
		/* piste maître en cours d'enreg : rien à avancer (rec_head local) */
	}
}
