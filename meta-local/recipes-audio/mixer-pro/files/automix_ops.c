// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * automix_ops — ops control de l'AUTOMIX LIVE (V14.0 étape 4).
 * Corps déplacés tels quels depuis handle_cmd (extraction pure). Fichier
 * séparé d'automix.c pour tenir la limite 1000 lignes/fichier — même
 * domaine, même .h (automix.h).
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "state.h"
#include "util.h"
#include "strip_dyn.h"
#include "master.h"
#include "voice.h"
#include "automix.h"
#include "control.h"

/* V14.0 étape 4 : ops du module — appelées par le dispatcher control.
 * Corps déplacés tels quels depuis handle_cmd (extraction pure) ;
 * retourne 1 si l'op est traitée, 0 sinon. */
int automix_handle_op(int fd, const char *line)
{
	if (json_has_op(line, "set_automix")) {
		/* V12-AMX : adhésion + poids par tranche.
		 * {"op":"set_automix","src":N,"on":0|1,"weight_db":F} */
		int src, on = 0;
		float wdb = 0.0f;
		if (json_get_int(line, "src", &src) < 0 ||
		    src < 0 || src >= N_INPUT_REAL) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_automix src\"}\n");
			return 1;
		}
		/* updates PARTIELS : toggler « A » sans weight_db ne doit pas
		 * écraser le poids, et régler le poids ne touche pas l'adhésion */
		int has_on = json_get_int(line, "on", &on) == 0;
		int has_w  = json_get_float(line, "weight_db", &wdb) == 0;
		{
			const int lp = link_partner(src);   /* V13.3 */
			pthread_mutex_lock(&g_st.target_lock);
			if (has_on) {
				g_st.automix_member[src] = on ? 1 : 0;
				if (!on)
					g_st.automix_gtarget[src] = 1.0f;
				if (lp >= 0) {
					g_st.automix_member[lp] = on ? 1 : 0;
					if (!on)
						g_st.automix_gtarget[lp] = 1.0f;
				}
			}
			if (has_w && wdb >= -20.0f && wdb <= 20.0f) {
				g_st.automix_weight[src] = powf(10.0f, wdb / 20.0f);
				if (lp >= 0)
					g_st.automix_weight[lp] =
						g_st.automix_weight[src];
			}
			pthread_mutex_unlock(&g_st.target_lock);
		}
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_automix\",\"src\":%d,"
			    "\"on\":%d}\n", src, g_st.automix_member[src]);
		return 1;
	}
	if (json_has_op(line, "set_automix_cfg")) {
		/* {"op":"set_automix_cfg","on":0|1,"resp_ms":F,"floor_db":F} */
		int on = -1;
		float resp = -1.0f, floordb = 1.0f;
		(void)json_get_int(line, "on", &on);
		(void)json_get_float(line, "resp_ms", &resp);
		(void)json_get_float(line, "floor_db", &floordb);
		pthread_mutex_lock(&g_st.target_lock);
		if (on >= 0)
			g_st.automix_on = on ? 1 : 0;
		if (resp >= 10.0f && resp <= 2000.0f)
			g_st.automix_resp_ms = resp;
		if (floordb <= 0.0f && floordb >= -40.0f)
			g_st.automix_floor = powf(10.0f, floordb / 20.0f);
		pthread_mutex_unlock(&g_st.target_lock);
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_automix_cfg\",\"on\":%d}\n",
			g_st.automix_on);
		return 1;
	}
	if (json_has_op(line, "get_automix")) {
		/* état + gains courants (dB) pour la GUI */
		int n = snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			"{\"ok\":true,\"on\":%d,\"resp_ms\":%.0f,"
			"\"floor_db\":%.1f,\"members\":[",
			g_st.automix_on, g_st.automix_resp_ms,
			20.0f * log10f(g_st.automix_floor + 1e-9f));
		for (int i = 0; i < N_INPUT_REAL; i++)
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%d",
				      i ? "," : "", g_st.automix_member[i]);
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "],\"gains_db\":[");
		for (int i = 0; i < N_INPUT_REAL; i++)
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%.1f",
				      i ? "," : "",
				      20.0f * log10f(g_st.automix_gain[i] + 1e-9f));
		/* V12-AMX-UI : poids par tranche (dB) pour le panneau réglages */
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "],\"weights_db\":[");
		for (int i = 0; i < N_INPUT_REAL; i++)
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%.1f",
				      i ? "," : "",
				      20.0f * log10f(g_st.automix_weight[i] + 1e-9f));
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "]}\n");
		write(fd, g_ctl_reply, n);
		return 1;
	}
	if (json_has_op(line, "bandmix_role")) {
		/* V13 : {"op":"bandmix_role","src":N,"role":"lead|choir|..."} */
		int src = -1;
		char rn[16] = "";
		(void)json_get_int(line, "src", &src);
		(void)json_get_str(line, "role", rn, sizeof(rn));
		int role = -1;
		for (int r = 0; r < BR_NROLES; r++)
			if (!strcmp(rn, BR_NAMES[r])) role = r;
		if (src < 0 || src >= N_EXP_CH || role < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad role\"}\n");
			return 1;
		}
		g_bmx.role[src] = role;
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"src\":%d,\"role\":\"%s\"}\n",
			src, BR_NAMES[role]);
		return 1;
	}
	if (json_has_op(line, "bandmix_measure")) {
		/* {"op":"bandmix_measure","src":N} — 12 s, auto-stop.
		 * src:-1 = annuler. */
		int src = -2;
		(void)json_get_int(line, "src", &src);
		if (src == -1) {
			atomic_store(&g_bmx.meas_src, -1);
			dprintf(fd, "{\"ok\":true,\"measuring\":-1}\n");
			return 1;
		}
		if (src < 0 || src >= N_EXP_CH ||
		    atomic_load(&g_bmx.meas_src) >= 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"busy or bad src\"}\n");
			return 1;
		}
		g_bmx.acc_ms = 0; g_bmx.nblk_s = 0;
		g_bmx.peak_max = 0; g_bmx.sm = 0;
		g_bmx.minsm = 1e9f; g_bmx.warm = 0;
		clock_gettime(CLOCK_MONOTONIC, &g_bmx.meas_t0);
		atomic_store(&g_bmx.meas_src, src);
		dprintf(fd, "{\"ok\":true,\"measuring\":%d,\"secs\":12}\n", src);
		return 1;
	}
	if (json_has_op(line, "bandmix_calc")) {
		bmx_calc();
		dprintf(fd, "{\"ok\":true,\"op\":\"bandmix_calc\"}\n");
		return 1;
	}
	if (json_has_op(line, "bandmix_lock")) {
		memset(g_bmx.lock_acc, 0, sizeof(g_bmx.lock_acc));
		g_bmx.lock_ticks = 0;
		g_bmx.locking = 1;
		dprintf(fd, "{\"ok\":true,\"op\":\"bandmix_lock\",\"secs\":30}\n");
		return 1;
	}
	if (json_has_op(line, "bandmix_live")) {
		int on = 0;
		(void)json_get_int(line, "on", &on);
		g_bmx.live = on ? 1 : 0;
		if (!g_bmx.live) {
			/* retour doux à 0 dB */
			memset(g_bmx.kdb, 0, sizeof(g_bmx.kdb));
			pthread_mutex_lock(&g_st.target_lock);
			for (int i = 0; i < N_INPUT_TOTAL; i++)
				g_st.keeper_target[i] = 1.0f;
			pthread_mutex_unlock(&g_st.target_lock);
		}
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"live\":%d}\n", g_bmx.live);
		return 1;
	}
	if (json_has_op(line, "bandmix_autolive")) {
		/* V13.5 : automix continu — un seul interrupteur, aucun
		 * soundcheck/verrouillage. {"op":"bandmix_autolive","on":0|1} */
		int on = 0;
		(void)json_get_int(line, "on", &on);
		g_bmx.autolive = on ? 1 : 0;
		if (g_bmx.autolive) {
			for (int i = 0; i < N_EXP_CH; i++)
				g_bmx.al_ref[i] = g_bmx.risk[i] = -120.0f;   /* recale les peak-holds */
			g_bmx.al_anchor = -120.0f;           /* ré-init de l'ancre */
			/* V15.1 (mesure 2026-08-05, validé Michael) : les gains de
			 * groupe repartent de 0 dB — l'héritage de la fin du morceau
			 * précédent (+36 dB d'outro calme) faisait démarrer le
			 * suivant TROP FORT pendant la phase où la balance est gelée.
			 * Départ jamais plus fort que les stems, le staging 8 dB/s
			 * remonte au niveau en ~2 s. (Remplace la décision V13.9
			 * « gains conservés » — le trop-fort est pire que la montée.) */
			g_bmx.g_voice_db = g_bmx.g_choir_db = g_bmx.g_music_db = 0.0f;
			g_bmx.prog_peak = -120.0f;
			g_bmx.bal_staged = 0;
			memset(g_bmx.act_ticks, 0, sizeof(g_bmx.act_ticks));
			/* V13.9 — reset solo (l'auto se re-déclenchera si mérité) */
			g_bmx.solo_src = -1;
			g_bmx.solo_is_auto = 0;
			g_bmx.solo_on_cnt = g_bmx.solo_off_cnt = 0;
			for (int i = 0; i < N_EXP_CH; i++)
				g_bmx.solo_base[i] = -999.0f;   /* base v2 à réapprendre */
			/* V13.6 : EQ de placement.
			 * V13.9 : le vfocus n'est PLUS forcé ici — un reset ne doit
			 * JAMAIS écraser un réglage posé par l'opérateur (le bouton
			 * PLACE À LA VOIX semblait « cassé » : choix OFF silencieuse-
			 * ment ré-armé à chaque lancement de morceau). */
			for (int i = 0; i < N_EXP_CH; i++)
				g_eqx.role_of[i] = -1;       /* force le recalcul coefs */
			atomic_store(&g_eqx.on, 1);
			/* V13.7 — étage master : EQ mastering + makeup LUFS */
			memset(g_meq_st, 0, sizeof(g_meq_st));
			g_meq_fading = 0;
			meq_init();   /* pose l'EQ direct (pas de fondu à l'activation) */
			memset(g_mk.k1, 0, sizeof(g_mk.k1));
			memset(g_mk.k2, 0, sizeof(g_mk.k2));
			g_mk.ms = 0.0f;
			g_mk.mk_db = 0.0f;
			g_mk.makeup_cur = 1.0f;
			atomic_store(&g_mk.makeup_mq, 1000);
			atomic_store(&g_mk.lufs_c, -12000);   /* gelé au démarrage */
			atomic_store(&g_master_on, 1);
		} else {
			memset(g_bmx.kdb, 0, sizeof(g_bmx.kdb));
			atomic_store(&g_eqx.on, 0);
			atomic_store(&g_master_on, 0);
			atomic_store(&g_mk.makeup_mq, 1000);
			pthread_mutex_lock(&g_st.target_lock);
			for (int i = 0; i < N_INPUT_TOTAL; i++)
				g_st.keeper_target[i] = 1.0f;
			/* V13.6 : coupe les comps auto (rôle ≠ off) posés par l'automix */
			for (int i = 0; i < N_EXP_CH; i++)
				if (g_bmx.role[i] != BR_OFF && g_cmp[i].on &&
				    BMX_P[g_bmx.role[i]].comp_on)
					cmp_configure(i, 0, g_cmp[i].thr_db,
						g_cmp[i].ratio, g_cmp[i].atk_ms,
						g_cmp[i].rel_ms, g_cmp[i].makeup_db);
			/* V13.9 : coupe aussi les GATES AUTO posées par l'automix */
			for (int i = 0; i < N_EXP_CH; i++)
				if (g_bmx.role[i] != BR_OFF && g_exp[i].on &&
				    BMX_P[g_bmx.role[i]].gate_on)
					exp_configure(i, 0, g_exp[i].thr_db,
						g_exp[i].ratio, g_exp[i].atk_ms,
						g_exp[i].rel_ms, g_exp[i].range_db,
						g_exp[i].hold_ms);
			pthread_mutex_unlock(&g_st.target_lock);
		}
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"autolive\":%d}\n", g_bmx.autolive);
		return 1;
	}
	if (json_has_op(line, "bandmix_solo")) {
		/* V13.9 — SOLO : {"op":"bandmix_solo","src":-1..15,"auto":0/1}
		 * src = voie à soloer (−1 = aucun), pose un solo MANUEL (que
		 * l'auto ne relâche pas). auto = détection automatique on/off. */
		int iv;
		if (json_get_int(line, "src", &iv) >= 0 && iv >= -1 && iv < N_EXP_CH) {
			g_bmx.solo_src = iv;
			g_bmx.solo_is_auto = 0;
			g_bmx.solo_on_cnt = g_bmx.solo_off_cnt = 0;
		}
		if (json_get_int(line, "auto", &iv) >= 0) {
			g_bmx.solo_auto = iv ? 1 : 0;
			/* seul le choix auto est persisté (pas le solo ponctuel) */
			atomic_store(&g_presets_dirty, 1);
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"bandmix_solo\",\"src\":%d,"
			"\"auto\":%d,\"is_auto\":%d}\n",
			g_bmx.solo_src, g_bmx.solo_auto, g_bmx.solo_is_auto);
		return 1;
	}
	if (json_has_op(line, "bandmix_status")) {
		int ms = atomic_load(&g_bmx.meas_src);
		int elapsed = 0;
		if (ms >= 0) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			elapsed = (int)(now.tv_sec - g_bmx.meas_t0.tv_sec);
		}
		int n = snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			"{\"ok\":true,\"live\":%d,\"ref_valid\":%d,"
			"\"autolive\":%d,"
			"\"locking\":%d,\"measuring\":%d,\"meas_elapsed\":%d,"
			"\"solo\":%d,\"solo_auto\":%d,\"solo_is_auto\":%d,"
			"\"chans\":[",
			g_bmx.live, g_bmx.ref_valid, g_bmx.autolive,
			g_bmx.locking, ms, elapsed,
			g_bmx.solo_src, g_bmx.solo_auto, g_bmx.solo_is_auto);
		for (int i = 0; i < N_EXP_CH; i++) {
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n,
				"%s{\"src\":%d,\"role\":\"%s\",\"done\":%d,"
				"\"rms_db\":%.1f,\"floor_db\":%.1f,"
				"\"keeper_db\":%.2f}",
				i ? "," : "", i, BR_NAMES[g_bmx.role[i]],
				g_bmx.m[i].done,
				g_bmx.m[i].done ? g_bmx.m[i].rms_avg_db : -99.0f,
				g_bmx.m[i].done ? g_bmx.m[i].floor_db : -99.0f,
				g_bmx.kdb[i]);
		}
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "]}\n");
		write(fd, g_ctl_reply, n);
		return 1;
	}
	if (json_has_op(line, "automix_tune")) {
		/* V13.9 — tunables automix réglables en LIVE (R&D) :
		 * {"op":"automix_tune","freeze_db":..,"risk_decay":..,"risk_margin":..}
		 * champs absents = inchangés ; sans champ = lecture. */
		float v; int chg = 0;
		if (json_get_float(line, "freeze_db",   &v) >= 0 && v >= 3.0f && v <= 40.0f)
			{ g_bmx.freeze_db = v; chg = 1; }
		if (json_get_float(line, "risk_decay",  &v) >= 0 && v >= 0.0f && v <= 2.0f)
			{ g_bmx.risk_decay = v; chg = 1; }
		if (json_get_float(line, "risk_margin", &v) >= 0 && v >= 0.0f && v <= 12.0f)
			{ g_bmx.risk_margin = v; chg = 1; }
		if (json_get_float(line, "gate_db", &v) >= 0 && v >= 3.0f && v <= 30.0f)
			{ g_bmx.gate_db = v; chg = 1; }
		if (chg)   /* pollé en lecture par la GUI : dirty SEULEMENT si set */
			atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"automix_tune\",\"freeze_db\":%.1f,"
			"\"risk_decay\":%.3f,\"risk_margin\":%.1f,\"gate_db\":%.1f}\n",
			g_bmx.freeze_db, g_bmx.risk_decay, g_bmx.risk_margin,
			g_bmx.gate_db);
		return 1;
	}
	if (json_has_op(line, "set_balance")) {
		/* V13.9 — BALANCE AUTO (table quadrants) : tient LUFS=lufs_tgt ET
		 * écart voix−musique = e_tgt en bougeant les gains de groupe.
		 * {"op":"set_balance","on":0/1,"lufs_tgt":-30..-6,"e_tgt":-6..12}
		 * absent=inchangé. Renvoie l'état + gains groupe + LUFS mesuré. */
		int iv; float v; int chg = 0;
		if (json_get_int(line, "on", &iv) >= 0)
			{ g_bmx.balance_on = iv ? 1 : 0; chg = 1; }
		if (json_get_float(line, "lufs_tgt", &v) >= 0 && v >= -30.0f && v <= -6.0f)
			{ g_bmx.bal_lufs_tgt = v; chg = 1; }
		if (json_get_float(line, "e_tgt", &v) >= 0 && v >= -6.0f && v <= 12.0f)
			{ g_bmx.bal_e_tgt = v; chg = 1; }
		if (json_get_float(line, "c_tgt", &v) >= 0 && v >= -6.0f && v <= 12.0f)
			{ g_bmx.bal_c_tgt = v; chg = 1; }
		if (chg)   /* pollé en lecture par la GUI : dirty SEULEMENT si set */
			atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_balance\",\"on\":%d,"
			"\"lufs_tgt\":%.1f,\"e_tgt\":%.1f,\"c_tgt\":%.1f,"
			"\"voice_db\":%.1f,\"choir_db\":%.1f,\"music_db\":%.1f,"
			"\"lufs\":%.1f}\n",
			g_bmx.balance_on, g_bmx.bal_lufs_tgt, g_bmx.bal_e_tgt,
			g_bmx.bal_c_tgt, g_bmx.g_voice_db, g_bmx.g_choir_db,
			g_bmx.g_music_db,
			atomic_load_explicit(&g_mk.lufs_c, memory_order_relaxed) * 0.01);
		return 1;
	}
	return 0;
}
