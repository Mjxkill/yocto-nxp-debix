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
#include "control.h"    /* handlers d'ops (V14.0 étape 4) */
#include <stdio.h>
#include <math.h>
#include <unistd.h>

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

/* V14.0 étape 4 : ops du module — appelées par le dispatcher control.
 * Corps déplacés tels quels depuis handle_cmd (extraction pure) ;
 * retourne 1 si l'op est traitée, 0 sinon. */
int looper_handle_op(int fd, const char *line)
{
	if (json_has_op(line, "looper_track_ctl")) {
		/* V12-LOOP-PRO : {"op":"looper_track_ctl","track":N,
		 * "action":"rec|play|mute|unmute|clear"} */
		int t = -1; char act[16] = "";
		(void)json_get_int(line, "track", &t);
		(void)json_get_str(line, "action", act, sizeof(act));
		if (t < 0 || t >= LOOP_TRACKS) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad track\"}\n");
			return 1;
		}
		struct loop_track *tr = &g_tr[t];
		int st = atomic_load(&tr->state);
		uint32_t mlen = atomic_load(&g_master_len);

		if (!strcmp(act, "rec")) {
			/* V13.2 : re-tap REC sur une piste ARMÉE = désarme */
			if (st == TR_ARMED) {
				atomic_store_explicit(&tr->state, TR_EMPTY,
						      memory_order_release);
				dprintf(fd, "{\"ok\":true,\"track\":%d,"
					    "\"armed\":0}\n", t);
				return 1;
			}
			/* un seul REC ACTIF simultané (l'armement est libre) */
			int busy = 0;
			for (int i = 0; i < LOOP_TRACKS; i++)
				if (atomic_load(&g_tr[i].state) == TR_REC) busy = 1;
			if (st != TR_EMPTY) {
				dprintf(fd, "{\"ok\":false,\"err\":\"not empty\"}\n");
				return 1;
			}
			/* memset de la piste VIDE (non lue par l'audio) → silence
			 * des zones non ré-enregistrées, aucun glitch. */
			memset(tr->buf, 0, (size_t)LOOP_MAX_FRAMES * 2 * sizeof(float));
			tr->rec_head = 0;
			tr->rec_done = 0;
			atomic_store(&tr->rec_start, REC_START_NONE);
			atomic_store(&tr->len, 0);
			atomic_store(&tr->muted, 0);
			if (mlen == 0 && !busy) {
				/* pas encore de boucle maître : REC libre
				 * immédiat (définit la longueur au PLAY) */
				atomic_store_explicit(&tr->state, TR_REC,
						      memory_order_release);
			} else {
				/* V13.2 : boucle maître présente (ou en cours
				 * d'enregistrement) → ARMÉ, départ quantifié
				 * au prochain début de boucle, un tour exact
				 * puis PLAY (loop_render). */
				atomic_store(&g_loop_run, 1);
				atomic_store_explicit(&tr->state, TR_ARMED,
						      memory_order_release);
			}
		} else if (!strcmp(act, "play")) {
			if (st == TR_REC) {
				if (mlen == 0) {
					/* piste MAÎTRE : fige master_len = rec_head */
					uint32_t h = tr->rec_head;
					if (h == 0) {
						dprintf(fd, "{\"ok\":false,\"err\":\"empty rec\"}\n");
						return 1;
					}
					atomic_store_explicit(&tr->len, h, memory_order_release);
					atomic_store(&tr->state, TR_PLAY);
					atomic_store(&g_master_len, h);
					atomic_store(&g_lpos, 0);
					atomic_store(&g_loop_run, 1);
				} else {
					/* piste alignée : fige à mlen (zones non
					 * enregistrées = silence memsetté) */
					atomic_store_explicit(&tr->len, mlen, memory_order_release);
					atomic_store(&tr->state, TR_PLAY);
				}
			}
			/* si déjà PLAY : no-op (transport global via looper_ctl) */
		} else if (!strcmp(act, "mute")) {
			atomic_store(&tr->muted, 1);
		} else if (!strcmp(act, "unmute")) {
			atomic_store(&tr->muted, 0);
		} else if (!strcmp(act, "clear")) {
			atomic_store_explicit(&tr->state, TR_EMPTY, memory_order_release);
			atomic_store(&tr->len, 0);
			atomic_store(&tr->muted, 0);
			atomic_store(&tr->peak, 0);
			tr->rec_head = 0;
			tr->rec_done = 0;
			/* si plus aucune piste n'a de contenu ni n'enregistre →
			 * réinitialise l'horloge maître (nouveau départ). */
			int alive = 0;
			for (int i = 0; i < LOOP_TRACKS; i++) {
				int s = atomic_load(&g_tr[i].state);
				if (s == TR_REC || (s == TR_PLAY && atomic_load(&g_tr[i].len)))
					alive = 1;
			}
			if (!alive) {
				atomic_store(&g_master_len, 0);
				atomic_store(&g_lpos, 0);
				atomic_store(&g_loop_run, 0);
				/* V13.2 : plus de boucle maître → les pistes
				 * ARMÉES n'ont plus de départ possible */
				for (int i = 0; i < LOOP_TRACKS; i++)
					if (atomic_load(&g_tr[i].state) == TR_ARMED)
						atomic_store(&g_tr[i].state, TR_EMPTY);
			}
		} else {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad action\"}\n");
			return 1;
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"looper_track_ctl\",\"track\":%d,"
			"\"state\":\"%s\"}\n", t, TR_NAMES[atomic_load(&tr->state)]);
		return 1;
	}
	if (json_has_op(line, "looper_track_cfg")) {
		/* {"op":"looper_track_cfg","track":N,"src_a":N,"src_b":N|-1,
		 * "gain_db":F} — refusé pendant REC de cette piste */
		int t = -1;
		(void)json_get_int(line, "track", &t);
		if (t < 0 || t >= LOOP_TRACKS) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad track\"}\n");
			return 1;
		}
		struct loop_track *tr = &g_tr[t];
		if (atomic_load(&tr->state) == TR_REC) {
			dprintf(fd, "{\"ok\":false,\"err\":\"busy rec\"}\n");
			return 1;
		}
		int a = -2, b = -2;
		float gdb = 1000.0f;
		(void)json_get_int(line, "src_a", &a);
		(void)json_get_int(line, "src_b", &b);
		(void)json_get_float(line, "gain_db", &gdb);
		if (a >= 0 && a < N_INPUT_REAL) tr->src_a = a;
		if (b >= -1 && b < N_INPUT_REAL) tr->src_b = b;
		if (gdb > -60.0f && gdb <= 12.0f) tr->gain = powf(10.0f, gdb / 20.0f);
		dprintf(fd, "{\"ok\":true,\"op\":\"looper_track_cfg\",\"track\":%d}\n", t);
		return 1;
	}
	if (json_has_op(line, "looper_ctl")) {
		/* transport global : {"op":"looper_ctl","action":"play_all|stop_all|clear_all"} */
		char act[16] = "";
		(void)json_get_str(line, "action", act, sizeof(act));
		if (!strcmp(act, "play_all")) {
			if (atomic_load(&g_master_len)) atomic_store(&g_loop_run, 1);
		} else if (!strcmp(act, "stop_all")) {
			atomic_store(&g_loop_run, 0);
		} else if (!strcmp(act, "clear_all")) {
			for (int i = 0; i < LOOP_TRACKS; i++) {
				atomic_store_explicit(&g_tr[i].state, TR_EMPTY,
						      memory_order_release);
				atomic_store(&g_tr[i].len, 0);
				atomic_store(&g_tr[i].muted, 0);
				atomic_store(&g_tr[i].peak, 0);
				g_tr[i].rec_head = 0;
				g_tr[i].rec_done = 0;
			}
			atomic_store(&g_master_len, 0);
			atomic_store(&g_lpos, 0);
			atomic_store(&g_loop_run, 0);
		} else {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad action\"}\n");
			return 1;
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"looper_ctl\",\"action\":\"%s\"}\n", act);
		return 1;
	}
	if (json_has_op(line, "looper_status")) {
		uint32_t mlen = atomic_load(&g_master_len);
		int n = snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			"{\"ok\":true,\"master_len_s\":%.2f,\"pos_s\":%.2f,"
			"\"run\":%d,\"max_s\":%u,\"master_peak\":%u,\"tracks\":[",
			mlen / 48000.0f, atomic_load(&g_lpos) / 48000.0f,
			atomic_load(&g_loop_run), LOOP_MAX_FRAMES / 48000u,
			atomic_load(&g_loop_mpeak));
		for (int t = 0; t < LOOP_TRACKS; t++) {
			struct loop_track *tr = &g_tr[t];
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n,
				"%s{\"track\":%d,\"state\":\"%s\",\"len_s\":%.2f,"
				"\"muted\":%d,\"src_a\":%d,\"src_b\":%d,"
				"\"gain_db\":%.1f,\"peak\":%u}",
				t ? "," : "", t, TR_NAMES[atomic_load(&tr->state)],
				atomic_load(&tr->len) / 48000.0f,
				atomic_load(&tr->muted), tr->src_a, tr->src_b,
				20.0f * log10f(tr->gain > 1e-6f ? tr->gain : 1e-6f),
				atomic_load(&tr->peak));
		}
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "]}\n");
		write(fd, g_ctl_reply, strlen(g_ctl_reply));
		return 1;
	}
	return 0;
}
