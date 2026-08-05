// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * antilarsen — notchs anti-larsen logiciels par voie (voir antilarsen.h).
 * V15 (ARCHI_V15_ANTILARSEN_V2.md). Le daemon décide, ce module applique.
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "state.h"       /* g_st.target_lock, g_presets_dirty */
#include "util.h"        /* mlog */
#include "dsp_bq.h"      /* eqx_peak (notch = peaking gain négatif Q élevé) */
#include "antilarsen.h"
#include "control.h"     /* json helpers + g_ctl_reply */

struct al_state g_al = { .q = 8.0f, .depth_max_db = -24.0f };

/* recalcule la banque INACTIVE de la voie depuis les métas puis bascule
 * atomiquement (pattern eqx : l'audio ne lit jamais des coefs à moitié
 * écrits, les états ne sont pas vidés → glissement sans clic).
 * Control thread, SOUS target_lock. */
static void al_rebuild(int src)
{
	struct al_voice *v = &g_al.v[src];
	int nb = !atomic_load_explicit(&v->bank, memory_order_relaxed);
	int n = 0;
	for (int s = 0; s < AL_SLOTS; s++) {
		if (v->freq_hz[s] > 0.0f) {
			eqx_peak(&v->bq[nb][s], v->freq_hz[s],
				 v->depth_db[s], g_al.q);
			n++;
		} else {
			v->bq[nb][s] = (struct eqx_bq){ 1, 0, 0, 0, 0 };
		}
	}
	atomic_store_explicit(&v->bank, nb, memory_order_release);
	atomic_store_explicit(&v->nact, n, memory_order_release);
}

/* Rendu (audio_thread, SOUS target_lock, entre exp_render et eqx_render).
 * Biquads forme II transposée, cascade — même noyau que eqx_render. */
void al_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	if (!atomic_load_explicit(&g_al.enable, memory_order_relaxed))
		return;
	for (int i = 0; i < N_EXP_CH; i++) {
		struct al_voice *v = &g_al.v[i];
		if (!atomic_load_explicit(&v->flag, memory_order_relaxed) ||
		    !atomic_load_explicit(&v->nact, memory_order_relaxed))
			continue;
		int bk = atomic_load_explicit(&v->bank, memory_order_acquire);
		for (int s = 0; s < AL_SLOTS; s++) {
			struct eqx_bq *q = &v->bq[bk][s];
			if (q->b0 == 1.0f && q->b1 == 0.0f && q->b2 == 0.0f)
				continue;   /* slot neutre */
			float z1 = v->st[s][0], z2 = v->st[s][1];
			float *x = in_block[i];
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float in = x[f];
				float y = q->b0 * in + z1;
				z1 = q->b1 * in - q->a1 * y + z2;
				z2 = q->b2 * in - q->a2 * y;
				x[f] = y;
			}
			v->st[s][0] = z1;
			v->st[s][1] = z2;
		}
	}
}

/* slot pour f : existant (±10 Hz), sinon libre, sinon -1 (plein — le
 * daemon gère l'éviction : ici on REFUSE, pas d'écrasement silencieux) */
static int al_slot_for(struct al_voice *v, float f)
{
	for (int s = 0; s < AL_SLOTS; s++)
		if (v->freq_hz[s] > 0.0f && fabsf(v->freq_hz[s] - f) < 10.0f)
			return s;
	for (int s = 0; s < AL_SLOTS; s++)
		if (v->freq_hz[s] <= 0.0f)
			return s;
	return -1;
}

/* V15 : ops du module (dispatcher control.c). Corps sous target_lock pour
 * toute écriture de métas (cohérent avec le reste du control plane). */
int antilarsen_handle_op(int fd, const char *line)
{
	if (json_has_op(line, "larsen_enable")) {
		/* {"op":"larsen_enable","on":0|1} — off = retrait de TOUS les
		 * notchs (banques neutres), flags conservés. */
		int on = 0;
		(void)json_get_int(line, "on", &on);
		pthread_mutex_lock(&g_st.target_lock);
		atomic_store(&g_al.enable, on ? 1 : 0);
		if (!on)
			for (int i = 0; i < N_EXP_CH; i++) {
				memset(g_al.v[i].freq_hz, 0,
				       sizeof(g_al.v[i].freq_hz));
				al_rebuild(i);
			}
		pthread_mutex_unlock(&g_st.target_lock);
		atomic_store(&g_presets_dirty, 1);
		mlog("al: enable=%d", on ? 1 : 0);
		dprintf(fd, "{\"ok\":true,\"op\":\"larsen_enable\",\"on\":%d}\n",
			on ? 1 : 0);
		return 1;
	}
	if (json_has_op(line, "larsen_flag")) {
		/* {"op":"larsen_flag","src":N,"on":0|1} — voie source possible
		 * (opérateur). off = retrait des notchs de la voie. */
		int src = -1, on = 0;
		(void)json_get_int(line, "src", &src);
		(void)json_get_int(line, "on", &on);
		if (src < 0 || src >= N_EXP_CH) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad src\"}\n");
			return 1;
		}
		pthread_mutex_lock(&g_st.target_lock);
		atomic_store(&g_al.v[src].flag, on ? 1 : 0);
		if (!on) {
			memset(g_al.v[src].freq_hz, 0,
			       sizeof(g_al.v[src].freq_hz));
			al_rebuild(src);
		}
		pthread_mutex_unlock(&g_st.target_lock);
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"larsen_flag\",\"src\":%d,"
			"\"on\":%d}\n", src, on ? 1 : 0);
		return 1;
	}
	if (json_has_op(line, "larsen_notch")) {
		/* {"op":"larsen_notch","src":N|-1,"freq":F,"depth":D<0}
		 * src=-1 = toutes les voies flaguées (sonde). Refusé si
		 * enable=0, voie non flaguée, ou slots pleins (le daemon
		 * décide des évictions — jamais d'écrasement silencieux). */
		int src = -2, posed = 0, refused = 0;
		float f = 0, d = -12.0f;
		(void)json_get_int(line, "src", &src);
		(void)json_get_float(line, "freq", &f);
		(void)json_get_float(line, "depth", &d);
		if (src < -1 || src >= N_EXP_CH || f < 40.0f || f > 18000.0f) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad args\"}\n");
			return 1;
		}
		if (!atomic_load(&g_al.enable)) {
			dprintf(fd, "{\"ok\":false,\"err\":\"disabled\"}\n");
			return 1;
		}
		if (d < g_al.depth_max_db) d = g_al.depth_max_db;
		if (d > -1.0f)             d = -1.0f;
		pthread_mutex_lock(&g_st.target_lock);
		for (int i = 0; i < N_EXP_CH; i++) {
			if (src >= 0 && i != src)
				continue;
			struct al_voice *v = &g_al.v[i];
			if (!atomic_load(&v->flag)) {
				if (src == i) refused++;
				continue;
			}
			int s = al_slot_for(v, f);
			if (s < 0) { refused++; continue; }
			v->freq_hz[s] = f;
			v->depth_db[s] = d;
			al_rebuild(i);
			posed++;
		}
		pthread_mutex_unlock(&g_st.target_lock);
		dprintf(fd, "{\"ok\":true,\"op\":\"larsen_notch\",\"freq\":%.1f,"
			"\"depth\":%.1f,\"posed\":%d,\"refused\":%d}\n",
			f, d, posed, refused);
		return 1;
	}
	if (json_has_op(line, "larsen_release")) {
		/* {"op":"larsen_release","src":N|-1,"freq":F|0} — freq 0 =
		 * tous les notchs de la/des voie(s). */
		int src = -2, removed = 0;
		float f = 0;
		(void)json_get_int(line, "src", &src);
		(void)json_get_float(line, "freq", &f);
		if (src < -1 || src >= N_EXP_CH) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad src\"}\n");
			return 1;
		}
		pthread_mutex_lock(&g_st.target_lock);
		for (int i = 0; i < N_EXP_CH; i++) {
			if (src >= 0 && i != src)
				continue;
			struct al_voice *v = &g_al.v[i];
			int chg = 0;
			for (int s = 0; s < AL_SLOTS; s++) {
				if (v->freq_hz[s] <= 0.0f)
					continue;
				if (f > 0.0f && fabsf(v->freq_hz[s] - f) >= 10.0f)
					continue;
				v->freq_hz[s] = 0.0f;
				removed++; chg = 1;
			}
			if (chg)
				al_rebuild(i);
		}
		pthread_mutex_unlock(&g_st.target_lock);
		dprintf(fd, "{\"ok\":true,\"op\":\"larsen_release\","
			"\"removed\":%d}\n", removed);
		return 1;
	}
	if (json_has_op(line, "larsen_cfg")) {
		/* {"op":"larsen_cfg","q":F,"depth_max":D} — champs absents =
		 * inchangés ; sans champ = lecture. Recalcule les notchs posés
		 * (bascule de banque, sans clic). */
		float v; int ch = 0;
		if (json_get_float(line, "q", &v) >= 0 &&
		    v >= 2.0f && v <= 40.0f) { g_al.q = v; ch = 1; }
		if (json_get_float(line, "depth_max", &v) >= 0 &&
		    v >= -40.0f && v <= -6.0f) { g_al.depth_max_db = v; ch = 1; }
		if (ch) {
			pthread_mutex_lock(&g_st.target_lock);
			for (int i = 0; i < N_EXP_CH; i++)
				al_rebuild(i);
			pthread_mutex_unlock(&g_st.target_lock);
			atomic_store(&g_presets_dirty, 1);
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"larsen_cfg\",\"q\":%.1f,"
			"\"depth_max\":%.1f}\n", g_al.q, g_al.depth_max_db);
		return 1;
	}
	if (json_has_op(line, "larsen_status")) {
		int n = snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			"{\"ok\":true,\"enable\":%d,\"q\":%.1f,"
			"\"depth_max\":%.1f,\"voices\":[",
			atomic_load(&g_al.enable), g_al.q, g_al.depth_max_db);
		for (int i = 0; i < N_EXP_CH; i++) {
			struct al_voice *v = &g_al.v[i];
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n,
				"%s{\"src\":%d,\"flag\":%d,\"notches\":[",
				i ? "," : "", i, atomic_load(&v->flag));
			int first = 1;
			for (int s = 0; s < AL_SLOTS; s++) {
				if (v->freq_hz[s] <= 0.0f)
					continue;
				n += snprintf(g_ctl_reply + n,
					sizeof(g_ctl_reply) - n,
					"%s{\"freq\":%.1f,\"depth\":%.1f}",
					first ? "" : ",", v->freq_hz[s],
					v->depth_db[s]);
				first = 0;
			}
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n,
				      "]}");
		}
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "]}\n");
		write(fd, g_ctl_reply, n);
		return 1;
	}
	return 0;
}
