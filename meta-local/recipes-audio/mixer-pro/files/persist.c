// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * persist — sérialisation d'état + scènes (voir persist.h).
 * Code déplacé tel quel depuis mixer-pro.c (V14.0 étape 2e, extraction pure).
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "state.h"       /* g_st, g_presets_dirty, mic map, out gains, insert */
#include "util.h"        /* mlog */
#include "strip_dyn.h"   /* exp/cmp_configure, g_exp, g_cmp, g_link */
#include "automix.h"     /* g_bmx, BR_NAMES */
#include "master.h"      /* g_meq_p, meq_recalc, g_master_on */
#include "voice.h"       /* g_vf, g_vspat */
#include "persist.h"
#include "control.h"    /* handlers d'ops (V14.0 étape 4) */
#include <unistd.h>
#include <dirent.h>

#define PRESETS_PATH "/var/lib/mixer-pro/presets.json"
/* ============================== Persistence presets ================ */

/* V9.3.5 : sauvegarde atomique l'état des 4 bus FX dans
 * /var/lib/mixer-pro/presets.json. Écriture via .tmp + rename pour atomicité.
 * Format :
 *   {"version":1,"buses":[{"bus":0,<get_state output>}, ...]}
 *
 * Appelée par persistence_thread quand g_presets_dirty est settée par
 * set_fx_engine ou set_fx_param. mkdir -p si absent. */
void save_presets(void)
{
	mkdir("/var/lib/mixer-pro", 0755);
	char tmp_path[256];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", PRESETS_PATH);
	FILE *f = fopen(tmp_path, "w");
	if (!f) {
		fprintf(stderr, "save_presets: fopen %s failed: %s\n",
		        tmp_path, strerror(errno));
		return;
	}
	fprintf(f, "{\"version\":1,\"buses\":[");
	/* Hold lock pour cohérence engine state vs params */
	pthread_mutex_lock(&g_st.target_lock);
	for (int b = 0; b < N_BUS_FX; b++) {
		static char body[16384];
		g_st.fx_engines[b].get_state(&g_st.fx_engines[b], body, sizeof(body));
		fprintf(f, "%s{\"bus\":%d,%s}", b == 0 ? "" : ",", b, body);
	}
	pthread_mutex_unlock(&g_st.target_lock);
	fprintf(f, "]}\n");
	fclose(f);
	/* rename atomique */
	if (rename(tmp_path, PRESETS_PATH) < 0)
		fprintf(stderr, "save_presets: rename failed: %s\n", strerror(errno));
}

/* V9.5.21 — persistance dédiée du remap mic (fichier texte 8 entiers). */
#define MIC_MAP_PATH "/var/lib/mixer-pro/mic_map"
void save_mic_map(void)
{
	mkdir("/var/lib/mixer-pro", 0755);
	FILE *f = fopen(MIC_MAP_PATH, "w");
	if (!f) return;
	for (int i = 0; i < 8; i++)
		fprintf(f, "%d%s", atomic_load_explicit(&g_mic_map[i],
		        memory_order_relaxed), i < 7 ? " " : "\n");
	fclose(f);
}
void load_mic_map(void)
{
	FILE *f = fopen(MIC_MAP_PATH, "r");
	if (!f) return;
	int v[8];
	if (fscanf(f, "%d %d %d %d %d %d %d %d",
	           &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7]) == 8) {
		for (int i = 0; i < 8; i++)
			if (v[i] >= 0 && v[i] < 8)
				atomic_store_explicit(&g_mic_map[i], v[i], memory_order_relaxed);
	}
	fclose(f);
}

/* V9.5.21 — persistance des gains de sortie (×1000 milli-linéaire). */
#define OUT_GAIN_PATH "/var/lib/mixer-pro/out_gain"
void save_out_gain(void)
{
	mkdir("/var/lib/mixer-pro", 0755);
	FILE *f = fopen(OUT_GAIN_PATH, "w");
	if (!f) return;
	for (int o = 0; o < N_OUTPUT_TOTAL; o++)
		fprintf(f, "%d%s", atomic_load_explicit(&g_out_gain_m[o],
		        memory_order_relaxed), o < N_OUTPUT_TOTAL - 1 ? " " : "\n");
	fclose(f);
}
void load_out_gain(void)
{
	FILE *f = fopen(OUT_GAIN_PATH, "r");
	if (!f) return;
	for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
		int v;
		if (fscanf(f, "%d", &v) != 1) break;
		if (v >= 0 && v <= 4000)
			atomic_store_explicit(&g_out_gain_m[o], v, memory_order_relaxed);
	}
	fclose(f);
}

/* V13.7 — persistance des 7 params de l'EQ master (mastering). Fichier dédié,
 * retro-compatible (absent → défauts smile). */
#define MASTER_EQ_PATH "/var/lib/mixer-pro/master_eq"
void save_master_eq(void)
{
	mkdir("/var/lib/mixer-pro", 0755);
	FILE *f = fopen(MASTER_EQ_PATH, "w");
	if (!f) return;
	fprintf(f, "%.2f %.1f %.2f %.1f %.3f %.2f %.1f\n",
		g_meq_p.low_db, g_meq_p.low_hz, g_meq_p.mid_db, g_meq_p.mid_hz,
		g_meq_p.mid_q, g_meq_p.air_db, g_meq_p.air_hz);
	fclose(f);
}
void load_master_eq(void)
{
	FILE *f = fopen(MASTER_EQ_PATH, "r");
	if (!f) return;
	float a, b, c, d, e, g, h;
	if (fscanf(f, "%f %f %f %f %f %f %f", &a, &b, &c, &d, &e, &g, &h) == 7) {
		g_meq_p.low_db = a; g_meq_p.low_hz = b; g_meq_p.mid_db = c;
		g_meq_p.mid_hz = d; g_meq_p.mid_q  = e; g_meq_p.air_db = g;
		g_meq_p.air_hz = h;
	}
	fclose(f);
}


/* V9.5.21b — persistance de l'état COMPLET du mixer (le manque n°1 de la
 * revue : la chaîne insert mastering, le mode assistant et le routage étaient
 * perdus à chaque reboot → re-setup manuel systématique).
 * Fichier texte versionné, écriture atomique (tmp + rename). */
#define MIXER_STATE_PATH "/var/lib/mixer-pro/mixer_state"
/* V13-SCENES : écrit l'état complet vers un chemin arbitraire (état
 * courant OU slot de scène — même format, même parseur au retour). */
void save_state_to(const char *path)
{
	mkdir("/var/lib/mixer-pro", 0755);
	char tmp_path[256];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	FILE *f = fopen(tmp_path, "w");
	if (!f) return;

	pthread_mutex_lock(&g_st.target_lock);
	fprintf(f, "version 1\n");
	fprintf(f, "insert %d\n", g_insert_spec_n);
	for (int i = 0; i < g_insert_spec_n; i++)
		fprintf(f, "%s %s\n", g_insert_spec_engine[i],
		        g_insert_spec_uri[i][0] ? g_insert_spec_uri[i] : "-");
	fprintf(f, "assistant %d %d\n",
	        atomic_load_explicit(&g_assistant_mode,   memory_order_relaxed),
	        atomic_load_explicit(&g_assistant_source, memory_order_relaxed));
	/* V12-AMX */
	fprintf(f, "automix %d %.1f %.4f\n", g_st.automix_on,
	        g_st.automix_resp_ms, g_st.automix_floor);
	fprintf(f, "automix_members");
	for (int i = 0; i < N_INPUT_REAL; i++)
		fprintf(f, " %d", g_st.automix_member[i]);
	fprintf(f, "\nautomix_weights");
	for (int i = 0; i < N_INPUT_REAL; i++)
		fprintf(f, " %.4f", g_st.automix_weight[i]);
	fprintf(f, "\n");
	fprintf(f, "mute_mask %u\n", g_st.mute_mask);
	fprintf(f, "input_gains");
	for (int i = 0; i < N_INPUT_TOTAL; i++)
		fprintf(f, " %.4f", g_st.input_target[i]);
	fprintf(f, "\nfx_bus");
	for (int b = 0; b < N_BUS_FX_CH; b++)
		fprintf(f, " %.4f", g_st.fx_bus_target[b]);
	fprintf(f, "\nmaster\n");
	for (int s = 0; s < N_INPUT_TOTAL; s++) {
		for (int o = 0; o < N_OUTPUT_TOTAL; o++)
			fprintf(f, "%.4f%s", g_st.master_target[s][o],
			        o < N_OUTPUT_TOTAL - 1 ? " " : "\n");
	}
	/* V12-EXP (fin de fichier — absent des états antérieurs) */
	for (int i = 0; i < N_EXP_CH; i++)
		fprintf(f, "expander %d %d %.1f %.1f %.1f %.0f %.0f %.0f\n",
			i, g_exp[i].on, g_exp[i].thr_db, g_exp[i].ratio,
			g_exp[i].atk_ms, g_exp[i].rel_ms, g_exp[i].range_db,
			g_exp[i].hold_ms);
	/* V13-COMP + BANDMIX (littéraux avec espace de tête au load) */
	for (int i = 0; i < N_EXP_CH; i++)
		fprintf(f, "comp %d %d %.1f %.1f %.1f %.0f %.1f\n",
			i, g_cmp[i].on, g_cmp[i].thr_db, g_cmp[i].ratio,
			g_cmp[i].atk_ms, g_cmp[i].rel_ms, g_cmp[i].makeup_db);
	for (int i = 0; i < N_EXP_CH; i++)
		fprintf(f, "bandmix %d %d %.6e\n", i, g_bmx.role[i],
			g_bmx.ref_valid ? g_bmx.ref_share[i] : 0.0f);
	fprintf(f, "bandmix_live %d %d %d\n", g_bmx.live, g_bmx.ref_valid,
		g_bmx.autolive);   /* V13.5 : 3e champ autolive (rétro-compat) */
	fprintf(f, "vfocus %d %.0f %.1f\n", g_vf.on,
		g_vf.amount * 100.0f, g_vf.max_cut_db);
	/* V13.9 (revue F15 + fiabilisation n°4) : persistance des réglages
	 * V13.7-V13.9 — EQ master, tunables automix, balance, spatializer,
	 * solo auto. Lignes ignorées par les anciens loaders (rétro-compat). */
	fprintf(f, "master_eq %.1f %.1f %.1f %.1f %.2f %.1f %.1f\n",
		g_meq_p.low_hz, g_meq_p.low_db, g_meq_p.mid_hz, g_meq_p.mid_db,
		g_meq_p.mid_q, g_meq_p.air_hz, g_meq_p.air_db);
	fprintf(f, "automix_tune %.1f %.3f %.1f %.1f\n", g_bmx.freeze_db,
		g_bmx.risk_decay, g_bmx.risk_margin, g_bmx.gate_db);
	fprintf(f, "balance %d %.1f %.1f %.1f\n", g_bmx.balance_on,
		g_bmx.bal_lufs_tgt, g_bmx.bal_e_tgt, g_bmx.bal_c_tgt);
	fprintf(f, "vspatial %d %d %d\n",
		(int)atomic_load(&g_vspat.on),
		(int)atomic_load(&g_vspat.amount_mq),
		(int)atomic_load(&g_vspat.delay_smp));
	fprintf(f, "solo_auto %d\n", g_bmx.solo_auto);
	/* V13.1 : matrice des sends par tranche (départs FX) — trouvé absent
	 * par la campagne de validation. En FIN de fichier, une ligne par
	 * tranche, parsé par la boucle fgets des loaders (états antérieurs
	 * sans ces lignes = compatibles). */
	for (int s = 0; s < N_INPUT_TOTAL; s++) {
		fprintf(f, "sends %d", s);
		for (int b = 0; b < N_BUS_FX_CH; b++)
			fprintf(f, " %.4f", g_st.send_target[s][b]);
		fprintf(f, "\n");
	}
	/* V13.3 : liens stéréo (8 paires) */
	fprintf(f, "links");
	for (int i = 0; i < N_LINK_PAIRS; i++)
		fprintf(f, " %d", atomic_load(&g_link[i]));
	fprintf(f, "\n");
	pthread_mutex_unlock(&g_st.target_lock);

	fclose(f);
	rename(tmp_path, path);
}

void save_mixer_state(void)
{
	save_state_to(MIXER_STATE_PATH);
}

/* ============ V13-SCENES — rappel de profil sans coupure audio ============
 * Même format que mixer_state (GARDER EN PHASE avec load_mixer_state).
 * Le fichier est lu EN MÉMOIRE puis parsé via fmemopen : aucune I/O
 * disque sous target_lock. L'insert chain (LV2, lourde) est ré-initiée
 * HORS lock puis swappée (pattern set_insert) seulement si le spec
 * diffère du courant. Les gains atterrissent dans les TARGETS → les
 * valeurs réelles glissent via smooth_gains (aucun clic). */
/* ===== Parseur COMMUN des lignes d'état (revue code 2026-07-28, lot 5b) =====
 * Boucle fgets/sscanf partagée par load_mixer_state (boot) et scene_apply
 * (rappel live) — était dupliquée à l'identique dans les deux (77 lignes),
 * chaque évolution devait être faite 2 fois (source de divergence).
 * APPELANT responsable du verrouillage (les deux appellent sous target_lock).
 * ATTENTION ordre des tests sscanf : les littéraux mangent les PRÉFIXES des
 * mots-clés voisins ("bandmix" avale le début de "bandmix_live") et
 * désynchronisent le flux. Tester le mot-clé long AVANT le court. */
static void parse_state_lines(FILE *f)
{
	char bl[160];
	int src, on, role, live, rv, al = 0;
	float thr, ratio, atk, rel, mk, shr, sv[8];
	int lk[8];
	while (fgets(bl, sizeof(bl), f)) {
		if (sscanf(bl, "comp %d %d %f %f %f %f %f",
			   &src, &on, &thr, &ratio, &atk, &rel,
			   &mk) == 7)
			cmp_configure(src, on, thr, ratio, atk, rel, mk);
		else if (sscanf(bl, "bandmix_live %d %d %d",
				&live, &rv, &al) >= 2) {
			g_bmx.ref_valid = rv ? 1 : 0;
			g_bmx.live = (live && rv) ? 1 : 0;
			g_bmx.autolive = al ? 1 : 0;   /* V13.5 */
		} else if (sscanf(bl, "bandmix %d %d %f",
				  &src, &role, &shr) == 3 &&
			   src >= 0 && src < N_EXP_CH &&
			   role >= 0 && role < BR_NROLES) {
			g_bmx.role[src] = role;
			g_bmx.ref_share[src] = shr;
		} else if (sscanf(bl, "vfocus %d %f %f",
				  &on, &atk, &rel) == 3) {
			g_vf.on = on ? 1 : 0;
			if (atk >= 0.0f && atk <= 100.0f)
				g_vf.amount = atk / 100.0f;
			if (rel >= 0.0f && rel <= 12.0f)
				g_vf.max_cut_db = rel;
		/* V13.9 (revue F15 + fiabilisation n°4) : restauration des
		 * réglages V13.7-V13.9 — mêmes plages de validation que les
		 * ops live ; meq_recalc = bascule crossfadée sans clic. */
		} else if (sscanf(bl, "master_eq %f %f %f %f %f %f %f",
				  &sv[0], &sv[1], &sv[2], &sv[3],
				  &sv[4], &sv[5], &sv[6]) == 7) {
			g_meq_p.low_hz = sv[0]; g_meq_p.low_db = sv[1];
			g_meq_p.mid_hz = sv[2]; g_meq_p.mid_db = sv[3];
			g_meq_p.mid_q  = sv[4];
			g_meq_p.air_hz = sv[5]; g_meq_p.air_db = sv[6];
			meq_recalc();
			save_master_eq();   /* fichier dédié cohérent */
		} else if (sscanf(bl, "automix_tune %f %f %f %f",
				  &sv[0], &sv[1], &sv[2], &sv[3]) == 4) {
			if (sv[0] >= 3.0f  && sv[0] <= 40.0f) g_bmx.freeze_db   = sv[0];
			if (sv[1] >= 0.0f  && sv[1] <= 2.0f)  g_bmx.risk_decay  = sv[1];
			if (sv[2] >= 0.0f  && sv[2] <= 12.0f) g_bmx.risk_margin = sv[2];
			if (sv[3] >= 3.0f  && sv[3] <= 30.0f) g_bmx.gate_db     = sv[3];
		} else if (sscanf(bl, "balance %d %f %f %f",
				  &on, &sv[0], &sv[1], &sv[2]) == 4) {
			g_bmx.balance_on = on ? 1 : 0;
			if (sv[0] >= -30.0f && sv[0] <= -6.0f) g_bmx.bal_lufs_tgt = sv[0];
			if (sv[1] >= -6.0f  && sv[1] <= 12.0f) g_bmx.bal_e_tgt   = sv[1];
			if (sv[2] >= -6.0f  && sv[2] <= 12.0f) g_bmx.bal_c_tgt   = sv[2];
		} else if (sscanf(bl, "vspatial %d %d %d",
				  &on, &src, &role) == 3) {
			atomic_store(&g_vspat.on, on ? 1 : 0);
			if (src >= 0 && src <= 1000)
				atomic_store(&g_vspat.amount_mq, src);
			if (role >= 144 && role <= 1920)   /* 3..40 ms @48k */
				atomic_store(&g_vspat.delay_smp, role);
		} else if (sscanf(bl, "solo_auto %d", &on) == 1) {
			g_bmx.solo_auto = on ? 1 : 0;
		} else if (sscanf(bl, "sends %d %f %f %f %f %f %f %f %f",
				  &src, &sv[0], &sv[1], &sv[2], &sv[3],
				  &sv[4], &sv[5], &sv[6], &sv[7]) == 9 &&
			   src >= 0 && src < N_INPUT_TOTAL) {
			/* V13.1 : départs FX par tranche */
			for (int b = 0; b < N_BUS_FX_CH && b < 8; b++)
				if (sv[b] >= 0.0f && sv[b] <= 8.0f)
					g_st.send_target[src][b] = sv[b];
		} else if (sscanf(bl, "links %d %d %d %d %d %d %d %d",
				  &lk[0], &lk[1], &lk[2], &lk[3],
				  &lk[4], &lk[5], &lk[6], &lk[7]) == 8) {
			/* V13.3 : liens stéréo */
			for (int i = 0; i < N_LINK_PAIRS; i++)
				atomic_store(&g_link[i], lk[i] ? 1 : 0);
		}
	}
}

int scene_apply(const char *path)
{
	FILE *df = fopen(path, "r");
	if (!df)
		return -1;
	char *buf = malloc(65536);
	if (!buf) { fclose(df); return -1; }
	size_t bn = fread(buf, 1, 65535, df);
	fclose(df);
	buf[bn] = '\0';
	FILE *f = fmemopen(buf, bn, "r");
	if (!f) { free(buf); return -1; }

	int ver = 0;
	if (fscanf(f, "version %d\n", &ver) != 1 || ver != 1) {
		fclose(f); free(buf);
		return -1;
	}
	/* --- insert spec → local (application différée hors lock) --- */
	static char eng[FX_CHAIN_MAX][32], uri[FX_CHAIN_MAX][256];
	int n_ins = 0, n_decl = 0;
	if (fscanf(f, "insert %d\n", &n_decl) == 1 &&
	    n_decl > 0 && n_decl <= FX_CHAIN_MAX) {
		int ok = 1;
		for (int i = 0; i < n_decl; i++) {
			if (fscanf(f, "%31s %255s\n", eng[i], uri[i]) != 2)
				{ ok = 0; break; }
			if (!strcmp(uri[i], "-"))
				uri[i][0] = '\0';
		}
		if (ok) n_ins = n_decl;
	}
	int am = 0, as = 0;
	if (fscanf(f, "assistant %d %d\n", &am, &as) == 2) {
		atomic_store_explicit(&g_assistant_mode,   am ? 1 : 0, memory_order_relaxed);
		atomic_store_explicit(&g_assistant_source, as ? 1 : 0, memory_order_relaxed);
	}

	/* --- le reste sous lock (parse depuis la MÉMOIRE, pas le disque) --- */
	pthread_mutex_lock(&g_st.target_lock);
	{
		int aon;
		float aresp, afloor;
		if (fscanf(f, " automix %d %f %f\n", &aon, &aresp, &afloor) == 3) {
			g_st.automix_on = aon ? 1 : 0;
			if (aresp >= 10.0f && aresp <= 2000.0f)
				g_st.automix_resp_ms = aresp;
			if (afloor > 0.0f && afloor <= 1.0f)
				g_st.automix_floor = afloor;
			if (fscanf(f, " automix_members") == 0)
				for (int i = 0; i < N_INPUT_REAL; i++) {
					int v;
					if (fscanf(f, "%d", &v) != 1) break;
					g_st.automix_member[i] = v ? 1 : 0;
				}
			if (fscanf(f, " automix_weights") == 0)
				for (int i = 0; i < N_INPUT_REAL; i++) {
					float v;
					if (fscanf(f, "%f", &v) != 1) break;
					if (v >= 0.01f && v <= 100.0f)
						g_st.automix_weight[i] = v;
				}
		}
	}
	{
		unsigned mm = 0;
		if (fscanf(f, " mute_mask %u\n", &mm) == 1)
			g_st.mute_mask = mm;
	}
	if (fscanf(f, " input_gains") == 0)
		for (int i = 0; i < N_INPUT_TOTAL; i++) {
			float v;
			if (fscanf(f, "%f", &v) != 1) break;
			if (v >= 0.0f && v <= 8.0f) g_st.input_target[i] = v;
		}
	if (fscanf(f, " fx_bus") == 0)
		for (int b = 0; b < N_BUS_FX_CH; b++) {
			float v;
			if (fscanf(f, "%f", &v) != 1) break;
			if (v >= 0.0f && v <= 8.0f) g_st.fx_bus_target[b] = v;
		}
	if (fscanf(f, " master") == 0)
		for (int s = 0; s < N_INPUT_TOTAL; s++)
			for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
				float v;
				if (fscanf(f, "%f", &v) != 1) goto tail;
				if (v >= 0.0f && v <= 8.0f)
					g_st.master_target[s][o] = v;
			}
tail:
	{
		int src, on;
		float thr, ratio, atk, rel, rng, hold;
		while (fscanf(f, " expander %d %d %f %f %f %f %f %f\n",
			      &src, &on, &thr, &ratio, &atk, &rel,
			      &rng, &hold) == 8)
			exp_configure(src, on, thr, ratio, atk, rel, rng, hold);
	}
	parse_state_lines(f);   /* boucle commune boot+scène */
	pthread_mutex_unlock(&g_st.target_lock);
	fclose(f);
	free(buf);

	/* --- insert chain : ré-init seulement si le spec diffère --- */
	int same = (n_ins == g_insert_spec_n);
	for (int i = 0; same && i < n_ins; i++)
		same = !strcmp(eng[i], g_insert_spec_engine[i]) &&
		       !strcmp(uri[i], g_insert_spec_uri[i]);
	if (!same) {
		if (n_ins == 0) {
			pthread_mutex_lock(&g_st.target_lock);
			int was = atomic_exchange(&g_insert_active, 0);
			g_insert_spec_n = 0;
			pthread_mutex_unlock(&g_st.target_lock);
			if (was) fx_free(&g_insert_chain);
		} else {
			struct fx_chain_spec specs[FX_CHAIN_MAX];
			for (int i = 0; i < n_ins; i++) {
				specs[i].engine = eng[i];
				specs[i].uri    = uri[i];
			}
			fx_engine_t chain = { 0 };
			if (fx_init_chain(&chain, (float)SAMPLE_RATE,
					  specs, n_ins)) {
				pthread_mutex_lock(&g_st.target_lock);
				fx_engine_t old = g_insert_chain;
				int was = atomic_load(&g_insert_active);
				for (int i = 0; i < n_ins; i++) {
					snprintf(g_insert_spec_engine[i], 32,
						 "%s", eng[i]);
					snprintf(g_insert_spec_uri[i], 256,
						 "%s", uri[i]);
				}
				g_insert_spec_n = n_ins;
				g_insert_chain = chain;
				atomic_store(&g_insert_active, 1);
				pthread_mutex_unlock(&g_st.target_lock);
				if (was) fx_free(&old);
			} else
				mlog("scene: insert chain init FAILED (spec gardé)");
		}
	}
	atomic_store(&g_presets_dirty, 1);   /* la scène devient l'état courant */
	mlog("scene: profil appliqué (%s)", path);
	return 0;
}

/* Appelée dans main() AVANT le démarrage des threads (pas de lock requis,
 * fx_init_chain initialise le monde lilv à la demande). */
void load_mixer_state(void)
{
	FILE *f = fopen(MIXER_STATE_PATH, "r");
	if (!f) return;
	int ver = 0;
	if (fscanf(f, "version %d\n", &ver) != 1 || ver != 1) {
		fclose(f);
		return;
	}
	int n_ins = 0;
	if (fscanf(f, "insert %d\n", &n_ins) == 1 &&
	    n_ins > 0 && n_ins <= FX_CHAIN_MAX) {
		struct fx_chain_spec specs[FX_CHAIN_MAX];
		int ok = 1;
		for (int i = 0; i < n_ins; i++) {
			if (fscanf(f, "%31s %255s\n", g_insert_spec_engine[i],
			           g_insert_spec_uri[i]) != 2) { ok = 0; break; }
			if (!strcmp(g_insert_spec_uri[i], "-"))
				g_insert_spec_uri[i][0] = '\0';
			specs[i].engine = g_insert_spec_engine[i];
			specs[i].uri    = g_insert_spec_uri[i];
		}
		if (ok) {
			fx_engine_t chain = {0};
			if (fx_init_chain(&chain, (float)SAMPLE_RATE, specs, n_ins)) {
				g_insert_chain = chain;
				atomic_store(&g_insert_active, 1);
				g_insert_spec_n = n_ins;
				mlog("state: insert chain restaurée (%d plugins)", n_ins);
			} else {
				mlog("state: insert chain restore FAILED (plugins absents ?)");
				g_insert_spec_n = 0;
			}
		}
	}
	int am = 0, as = 0;
	if (fscanf(f, "assistant %d %d\n", &am, &as) == 2) {
		atomic_store_explicit(&g_assistant_mode,   am ? 1 : 0, memory_order_relaxed);
		atomic_store_explicit(&g_assistant_source, as ? 1 : 0, memory_order_relaxed);
	}
	/* V12-AMX (optionnel — absent des états antérieurs) */
	{
		int aon;
		float aresp, afloor;
		if (fscanf(f, " automix %d %f %f\n", &aon, &aresp, &afloor) == 3) {
			g_st.automix_on = aon ? 1 : 0;
			if (aresp >= 10.0f && aresp <= 2000.0f)
				g_st.automix_resp_ms = aresp;
			if (afloor > 0.0f && afloor <= 1.0f)
				g_st.automix_floor = afloor;
			if (fscanf(f, " automix_members") == 0)
				for (int i = 0; i < N_INPUT_REAL; i++) {
					int v;
					if (fscanf(f, "%d", &v) != 1) break;
					g_st.automix_member[i] = v ? 1 : 0;
				}
			if (fscanf(f, " automix_weights") == 0)
				for (int i = 0; i < N_INPUT_REAL; i++) {
					float v;
					if (fscanf(f, "%f", &v) != 1) break;
					if (v >= 0.01f && v <= 100.0f)
						g_st.automix_weight[i] = v;
				}
		}
	}
	unsigned mm = 0;
	/* Espace de tête OBLIGATOIRE : la boucle automix_weights ci-dessus lit
	 * exactement N_INPUT_REAL floats et laisse le '\n' non consommé — un
	 * littéral sans skip d'espace échoue alors sans rien consommer et
	 * désynchronise TOUT le reste du parse (faders/mute/routing/expander
	 * perdus au boot — régression V12-AMX corrigée ici). */
	if (fscanf(f, " mute_mask %u\n", &mm) == 1)
		g_st.mute_mask = mm;
	if (fscanf(f, " input_gains") == 0) {
		for (int i = 0; i < N_INPUT_TOTAL; i++) {
			float v;
			if (fscanf(f, "%f", &v) != 1) break;
			if (v >= 0.0f && v <= 8.0f) g_st.input_target[i] = v;
		}
	}
	if (fscanf(f, " fx_bus") == 0) {
		for (int b = 0; b < N_BUS_FX_CH; b++) {
			float v;
			if (fscanf(f, "%f", &v) != 1) break;
			if (v >= 0.0f && v <= 8.0f) g_st.fx_bus_target[b] = v;
		}
	}
	if (fscanf(f, " master") == 0) {
		for (int s = 0; s < N_INPUT_TOTAL; s++)
			for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
				float v;
				if (fscanf(f, "%f", &v) != 1) goto done;
				if (v >= 0.0f && v <= 8.0f) g_st.master_target[s][o] = v;
			}
	}
	/* V12-EXP (optionnel) — exp_configure re-précalcule et clampe */
	{
		int src, on;
		float thr, ratio, atk, rel, rng, hold;
		while (fscanf(f, " expander %d %d %f %f %f %f %f %f\n",
			      &src, &on, &thr, &ratio, &atk, &rel,
			      &rng, &hold) == 8)
			exp_configure(src, on, thr, ratio, atk, rel, rng, hold);
	}
	parse_state_lines(f);   /* boucle commune boot+scène */
done:
	fclose(f);
	mlog("state: mixer_state restauré (assistant=%d/%d mute=0x%x)", am, as, mm);
}


/* V14.0 étape 4 : ops du module — appelées par le dispatcher control.
 * Corps déplacés tels quels depuis handle_cmd (extraction pure) ;
 * retourne 1 si l'op est traitée, 0 sinon. */
int persist_handle_op(int fd, const char *line)
{
	if (json_has_op(line, "scene_save")) {
		/* V13-SCENES : {"op":"scene_save","slot":0-5,"name":"..."} */
		int slot = -1;
		char nm[48] = "";
		(void)json_get_int(line, "slot", &slot);
		(void)json_get_str(line, "name", nm, sizeof(nm));
		if (slot < 0 || slot >= SCENE_SLOTS) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad slot\"}\n");
			return 1;
		}
		mkdir(SCENE_DIR, 0755);
		char p[128];
		snprintf(p, sizeof(p), SCENE_DIR "/scene%d", slot);
		save_state_to(p);
		if (nm[0]) {
			snprintf(p, sizeof(p), SCENE_DIR "/scene%d.name", slot);
			FILE *nf = fopen(p, "w");
			if (nf) { fprintf(nf, "%s\n", nm); fclose(nf); }
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"scene_save\",\"slot\":%d}\n",
			slot);
		return 1;
	}
	if (json_has_op(line, "scene_recall")) {
		int slot = -1;
		(void)json_get_int(line, "slot", &slot);
		if (slot < 0 || slot >= SCENE_SLOTS) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad slot\"}\n");
			return 1;
		}
		char p[128];
		snprintf(p, sizeof(p), SCENE_DIR "/scene%d", slot);
		if (scene_apply(p) == 0)
			dprintf(fd, "{\"ok\":true,\"op\":\"scene_recall\","
				"\"slot\":%d}\n", slot);
		else
			dprintf(fd, "{\"ok\":false,\"err\":\"scene vide\"}\n");
		return 1;
	}
	if (json_has_op(line, "scene_list")) {
		int n = snprintf(g_ctl_reply, sizeof(g_ctl_reply),
				 "{\"ok\":true,\"scenes\":[");
		for (int s = 0; s < SCENE_SLOTS; s++) {
			char p[128], nm[48] = "";
			snprintf(p, sizeof(p), SCENE_DIR "/scene%d", s);
			int used = access(p, R_OK) == 0;
			snprintf(p, sizeof(p), SCENE_DIR "/scene%d.name", s);
			FILE *nf = fopen(p, "r");
			if (nf) {
				if (fgets(nm, sizeof(nm), nf)) {
					char *e = strchr(nm, '\n');
					if (e) *e = '\0';
				}
				fclose(nf);
			}
			if (!nm[0])
				snprintf(nm, sizeof(nm), "Scène %d", s + 1);
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n,
				"%s{\"slot\":%d,\"used\":%d,\"name\":\"%s\"}",
				s ? "," : "", s, used, nm);
		}
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "]}\n");
		write(fd, g_ctl_reply, n);
		return 1;
	}
	return 0;
}
