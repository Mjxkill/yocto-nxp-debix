// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * control — socket /run/mixer-pro.sock : accept + parse JSON + dispatch
 * (voir control.h). handle_cmd essaie d'abord les handlers des modules
 * (chaque module possède ses ops) puis traite les ops « cœur » : matrices
 * send/master, faders, mutes, fx bus, insert LV2, assistant, meters, taps,
 * tac/alsa, diag système get_drift, reset. Code déplacé tel quel depuis
 * mixer-pro.c (V14.0 étape 4b, extraction pure).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#include "state.h"       /* g_st + globales transversales */
#include "util.h"        /* mlog */
#include "effects.h"     /* fx engines + insert */
#include "analyzer.h"    /* taps (get_meters) */
#include "strip_dyn.h"   /* link_partner (miroir stéréo) + handlers */
#include "sampler.h"
#include "looper.h"
#include "midix.h"
#include "automix.h"
#include "master.h"
#include "voice.h"
#include "persist.h"
#include "antilarsen.h"
#include "voice_clean.h"
#include "uac2_ring.h"   /* compteurs get_drift/reset_drift_stats */
#include "audio_loop.h"  /* g_skip_*, histogrammes iter */
#include "control.h"

/* V9.3.3 : buffer de réponse control partagé (un seul thread) — extern
 * dans control.h, utilisé par les handlers d'ops des modules. */
char g_ctl_reply[49152];

/* ============================== Control socket ===================== */

/* Cherche une clé numérique dans une string JSON simple. -1 si absent.
 * Très minimaliste — pas un parser JSON complet, juste `"key":<number>`.
 */
/* Extrait une string entre guillemets pour une clé "key":"..." */
int json_get_str(const char *s, const char *key, char *out, int max)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(s, pattern);
	if (!p) return -1;
	p += strlen(pattern);
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	if (*p != '"') return -1;
	p++;
	int i = 0;
	while (*p && *p != '"' && i < max - 1) out[i++] = *p++;
	out[i] = 0;
	return (*p == '"') ? 0 : -1;
}

int json_get_int(const char *s, const char *key, int *out)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(s, pattern);
	if (!p) return -1;
	p += strlen(pattern);
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	*out = (int)strtol(p, NULL, 10);
	return 0;
}

int json_get_float(const char *s, const char *key, float *out)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(s, pattern);
	if (!p) return -1;
	p += strlen(pattern);
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	*out = strtof(p, NULL);
	return 0;
}

int json_has_op(const char *s, const char *op)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"op\"");
	const char *p = strstr(s, pattern);
	if (!p) return 0;
	p += strlen(pattern);
	while (*p == ' ' || *p == ':' || *p == '\t' || *p == '"') p++;
	size_t n = strlen(op);
	/* V9.4.1 : match exact — sinon "set_insert" matche "set_insert_param".
	 * Le char après op doit terminer la string JSON ("). */
	return strncmp(p, op, n) == 0 && p[n] == '"';
}

static void handle_cmd(int fd, const char *line)
{

	/* V14.0 étape 4 : chaque module possède ses ops (noms disjoints →
	 * l'ordre des essais est sans effet). Cœur (matrices/fx/insert/
	 * meters/diag) traité ci-dessous. */
	if (sampler_handle_op(fd, line)   || looper_handle_op(fd, line) ||
	    midix_handle_op(fd, line)     || strip_dyn_handle_op(fd, line) ||
	    automix_handle_op(fd, line)   || master_handle_op(fd, line) ||
	    voice_handle_op(fd, line)     || persist_handle_op(fd, line) ||
	    antilarsen_handle_op(fd, line) || voice_clean_handle_op(fd, line) ||
	    fx_handle_op(fd, line)        || tac_handle_op(fd, line))
		return;

	if (json_has_op(line, "set_send")) {
		int in, bus;
		float gain = 0;
		if (json_get_int(line, "in", &in) < 0 ||
		    json_get_int(line, "bus", &bus) < 0 ||
		    json_get_float(line, "gain", &gain) < 0 ||
		    in < 0 || in >= N_INPUT_TOTAL ||
		    bus < 0 || bus >= N_BUS_FX_CH) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_send args\"}\n");
			return;
		}
		pthread_mutex_lock(&g_st.target_lock);
		g_st.send_target[in][bus] = gain;
		pthread_mutex_unlock(&g_st.target_lock);
		atomic_store(&g_presets_dirty, 1);   /* V13.1 : persistance sends */
		snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			 "{\"ok\":true,\"op\":\"set_send\",\"in\":%d,\"bus\":%d,\"gain\":%.4f}\n",
			 in, bus, gain);
		write(fd, g_ctl_reply, strlen(g_ctl_reply));

	} else if (json_has_op(line, "set_master")) {
		int src, out;
		float gain = 0;
		if (json_get_int(line, "src", &src) < 0 ||
		    json_get_int(line, "out", &out) < 0 ||
		    json_get_float(line, "gain", &gain) < 0 ||
		    src < 0 || src >= N_INPUT_TOTAL ||
		    out < 0 || out >= N_OUTPUT_TOTAL) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_master args\"}\n");
			return;
		}
		pthread_mutex_lock(&g_st.target_lock);
		g_st.master_target[src][out] = gain;
		pthread_mutex_unlock(&g_st.target_lock);
		atomic_store(&g_presets_dirty, 1);   /* V9.5.21b : persistance */
		snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			 "{\"ok\":true,\"op\":\"set_master\",\"src\":%d,\"out\":%d,\"gain\":%.4f}\n",
			 src, out, gain);
		write(fd, g_ctl_reply, strlen(g_ctl_reply));

	} else if (json_has_op(line, "set_fx_bus")) {
		int bus;
		float gain = 0;
		if (json_get_int(line, "bus", &bus) < 0 ||
		    json_get_float(line, "gain", &gain) < 0 ||
		    bus < 0 || bus >= N_BUS_FX_CH) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_fx_bus args\"}\n");
			return;
		}
		pthread_mutex_lock(&g_st.target_lock);
		g_st.fx_bus_target[bus] = gain;
		pthread_mutex_unlock(&g_st.target_lock);
		atomic_store(&g_presets_dirty, 1);   /* V9.5.21b : persistance */
		snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			 "{\"ok\":true,\"op\":\"set_fx_bus\",\"bus\":%d,\"gain\":%.4f}\n",
			 bus, gain);
		write(fd, g_ctl_reply, strlen(g_ctl_reply));

	} else if (json_has_op(line, "set_input_gain")) {
		int src;
		float gain = 1.0f;
		if (json_get_int(line, "src", &src) < 0 ||
		    json_get_float(line, "gain", &gain) < 0 ||
		    src < 0 || src >= N_INPUT_TOTAL) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_input_gain args\"}\n");
			return;
		}
		{
			const int lp = link_partner(src);   /* V13.3 */
			pthread_mutex_lock(&g_st.target_lock);
			g_st.input_target[src] = gain;
			if (lp >= 0)
				g_st.input_target[lp] = gain;
			pthread_mutex_unlock(&g_st.target_lock);
		}
		atomic_store(&g_presets_dirty, 1);   /* V9.5.21b : persistance */
		snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			 "{\"ok\":true,\"op\":\"set_input_gain\",\"src\":%d,\"gain\":%.4f}\n",
			 src, gain);
		write(fd, g_ctl_reply, strlen(g_ctl_reply));

	} else if (json_has_op(line, "set_mute")) {
		int src, mute;
		if (json_get_int(line, "src", &src) < 0 ||
		    json_get_int(line, "mute", &mute) < 0 ||
		    src < 0 || src >= N_INPUT_TOTAL) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_mute args\"}\n");
			return;
		}
		{
			const int lp = link_partner(src);   /* V13.3 */
			pthread_mutex_lock(&g_st.target_lock);
			if (mute) {
				g_st.mute_mask |= (1u << src);
				if (lp >= 0) g_st.mute_mask |= (1u << lp);
			} else {
				g_st.mute_mask &= ~(1u << src);
				if (lp >= 0) g_st.mute_mask &= ~(1u << lp);
			}
			pthread_mutex_unlock(&g_st.target_lock);
		}
		atomic_store(&g_presets_dirty, 1);   /* V9.5.21b : persistance */
		snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			 "{\"ok\":true,\"op\":\"set_mute\",\"src\":%d,\"mute\":%d}\n",
			 src, mute);
		write(fd, g_ctl_reply, strlen(g_ctl_reply));

	} else if (json_has_op(line, "get_strip_routing")) {
		/* E7.3a : retourne l'état routing complet pour 1 input strip :
		 *   - sends[8]    : send_target[src][bus] pour bus 0..7
		 *   - master[18]  : master_target[src][out] pour out 0..17
		 *   - gain        : input_target[src] (strip fader)
		 *   - mute        : (mute_mask >> src) & 1
		 */
		int src;
		if (json_get_int(line, "src", &src) < 0 ||
		    src < 0 || src >= N_INPUT_TOTAL) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad get_strip_routing src\"}\n");
			return;
		}
		int n = snprintf(g_ctl_reply, sizeof(g_ctl_reply),
				 "{\"ok\":true,\"src\":%d,\"sends\":[", src);
		pthread_mutex_lock(&g_st.target_lock);
		for (int b = 0; b < N_BUS_FX_CH && n < (int)sizeof(g_ctl_reply); b++)
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%.4f",
				      b ? "," : "", g_st.send_target[src][b]);
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "],\"master\":[");
		for (int o = 0; o < N_OUTPUT_TOTAL && n < (int)sizeof(g_ctl_reply); o++)
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%.4f",
				      o ? "," : "", g_st.master_target[src][o]);
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n,
			      "],\"gain\":%.4f,\"mute\":%d}\n",
			      g_st.input_target[src],
			      (g_st.mute_mask >> src) & 1);
		pthread_mutex_unlock(&g_st.target_lock);
		write(fd, g_ctl_reply, strlen(g_ctl_reply));

	} else if (json_has_op(line, "get_state")) {
		/* snd_pcm_delay : nb de frames entre le pointeur applicatif et le hw.
		 * cap delay = samples accumulés non encore lus
		 * play delay = samples écrits non encore joués
		 * latence DSP one-way ≈ play_delay / 48 ms (à 48 kHz).
		 */
		snd_pcm_sframes_t cap_d = 0, play_d = 0;
		snd_pcm_delay(g_st.cap_dsp.pcm,  &cap_d);
		snd_pcm_delay(g_st.play_dsp.pcm, &play_d);
		snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			 "{\"ok\":true,\"version\":\"%s\",\"frames\":%lu,\"xrun\":%lu,"
			 "\"mute_mask\":%u,\"cap_delay_frames\":%ld,\"play_delay_frames\":%ld,"
			 "\"latency_us_one_way\":%ld,"
			 "\"prof_cap_us\":%ld,\"prof_mix_us\":%ld,\"prof_play_us\":%ld,"
			 "\"prof_iter_us\":%ld,\"ring_drops\":%lu,"
			 "\"ring_fill_frames\":%u}\n",
			 MIXER_VERSION,
			 (unsigned long)atomic_load(&g_st.frames_processed),
			 (unsigned long)atomic_load(&g_st.xrun_count),
			 g_st.mute_mask,
			 (long)cap_d, (long)play_d,
			 (long)((cap_d + play_d) * 1000000L / SAMPLE_RATE),
			 (long)atomic_load(&g_st.last_cap_read_us),
			 (long)atomic_load(&g_st.last_mix_us),
			 (long)atomic_load(&g_st.last_play_write_us),
			 (long)atomic_load(&g_st.last_iter_us),
			 (unsigned long)atomic_load(&g_st.ring_drops),
			 (unsigned)(atomic_load(&g_st.ring_write_idx) -
				    atomic_load(&g_st.ring_read_idx)));
		write(fd, g_ctl_reply, strlen(g_ctl_reply));

	} else if (json_has_op(line, "set_input_map")) {
		/* V9.5.21 — remap mic DSP : {"op":"set_input_map","mic":I,"slot":S}
		 * (un mic) ou {"op":"set_input_map","map":[s0..s7]} (les 8). */
		int mic, slot;
		if (json_get_int(line, "mic", &mic) >= 0 &&
		    json_get_int(line, "slot", &slot) >= 0 &&
		    mic >= 0 && mic < 8 && slot >= 0 && slot < 8) {
			atomic_store_explicit(&g_mic_map[mic], slot, memory_order_relaxed);
			atomic_store(&g_presets_dirty, 1);
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"set_input_map\",\"map\":[");
		for (int i = 0; i < 8; i++)
			dprintf(fd, "%s%d", i ? "," : "",
			        atomic_load_explicit(&g_mic_map[i], memory_order_relaxed));
		dprintf(fd, "]}\n");

	} else if (json_has_op(line, "get_input_map")) {
		dprintf(fd, "{\"ok\":true,\"op\":\"get_input_map\",\"map\":[");
		for (int i = 0; i < 8; i++)
			dprintf(fd, "%s%d", i ? "," : "",
			        atomic_load_explicit(&g_mic_map[i], memory_order_relaxed));
		dprintf(fd, "]}\n");

	} else if (json_has_op(line, "set_output_gain")) {
		/* V9.5.21 — gain d'une sortie : {"op":"set_output_gain","out":O,"db":X}
		 * out : 0..N_OUTPUT_TOTAL-1 (0-7 DSP, 8-15 USB, 16-17 phone).
		 * db : -60..+12 dB (ou "gain" linéaire direct). */
		int out;
		float db, gain;
		if (json_get_int(line, "out", &out) >= 0 &&
		    out >= 0 && out < N_OUTPUT_TOTAL) {
			float g = 1.0f;
			if (json_get_float(line, "db", &db) >= 0)
				g = (db <= -60.0f) ? 0.0f : powf(10.0f, db / 20.0f);
			else if (json_get_float(line, "gain", &gain) >= 0)
				g = gain;
			int gm = (int)(g * 1000.0f + 0.5f);
			if (gm < 0) gm = 0;
			if (gm > 4000) gm = 4000;
			atomic_store_explicit(&g_out_gain_m[out], gm, memory_order_relaxed);
			atomic_store(&g_presets_dirty, 1);
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"set_output_gain\",\"gains\":[");
		for (int o = 0; o < N_OUTPUT_TOTAL; o++)
			dprintf(fd, "%s%d", o ? "," : "",
			        atomic_load_explicit(&g_out_gain_m[o], memory_order_relaxed));
		dprintf(fd, "]}\n");

	} else if (json_has_op(line, "get_output_gain")) {
		dprintf(fd, "{\"ok\":true,\"op\":\"get_output_gain\",\"gains\":[");
		for (int o = 0; o < N_OUTPUT_TOTAL; o++)
			dprintf(fd, "%s%d", o ? "," : "",
			        atomic_load_explicit(&g_out_gain_m[o], memory_order_relaxed));
		dprintf(fd, "]}\n");

	} else if (json_has_op(line, "get_meters_lite")) {
		/* V10-N2 : peaks seuls (in/out/fx), SANS le payload analyzer
		 * (~4.8 KB) — pour l'app native mixer-console qui poll à 30 Hz
		 * et n'affiche pas encore de spectre. */
		int n = 0;
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "{\"ok\":true,\"in\":[");
		for (int i = 0; i < N_INPUT_TOTAL && n < (int)sizeof(g_ctl_reply); i++)
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%u", i ? "," : "",
				      atomic_load_explicit(&g_st.peak_in[i], memory_order_relaxed));
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "],\"out\":[");
		for (int o = 0; o < N_OUTPUT_TOTAL && n < (int)sizeof(g_ctl_reply); o++)
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%u", o ? "," : "",
				      atomic_load_explicit(&g_st.peak_out[o], memory_order_relaxed));
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "],\"fx\":[");
		for (int b = 0; b < N_BUS_FX_CH && n < (int)sizeof(g_ctl_reply); b++)
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%u", b ? "," : "",
				      atomic_load_explicit(&g_st.peak_fx[b], memory_order_relaxed));
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "]}\n");
		write(fd, g_ctl_reply, n);

	} else if (json_has_op(line, "get_meters")) {
		/* E7.1 + E7.5 : retourne peaks + analyzer (spectrum + scope) en
		 * un seul round-trip, consommé par mixer-gui-http /api/stream.
		 * Conversion dBFS peaks côté client : 20*log10(peak/2147483648).
		 */
		static char g_ctl_reply[16384];
		int n = 0;
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "{\"ok\":true,\"in\":[");
		for (int i = 0; i < N_INPUT_TOTAL && n < (int)sizeof(g_ctl_reply); i++)
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%u", i ? "," : "",
				      atomic_load_explicit(&g_st.peak_in[i], memory_order_relaxed));
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "],\"out\":[");
		for (int o = 0; o < N_OUTPUT_TOTAL && n < (int)sizeof(g_ctl_reply); o++)
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%u", o ? "," : "",
				      atomic_load_explicit(&g_st.peak_out[o], memory_order_relaxed));
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "],\"fx\":[");
		for (int b = 0; b < N_BUS_FX_CH && n < (int)sizeof(g_ctl_reply); b++)
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%u", b ? "," : "",
				      atomic_load_explicit(&g_st.peak_fx[b], memory_order_relaxed));
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "],\"analyzer\":[");
		for (int t = 0; t < N_TAPS && n < (int)sizeof(g_ctl_reply); t++) {
			int k = atomic_load_explicit(&g_taps[t].kind, memory_order_relaxed);
			int aa = atomic_load_explicit(&g_taps[t].a, memory_order_relaxed);
			int bb = atomic_load_explicit(&g_taps[t].b, memory_order_relaxed);
			int8_t  spec[TAP_BINS_OUT];
			int16_t scope[TAP_SCOPE_N * 2];
			float   rms_dB;
			pthread_mutex_lock(&g_taps[t].out_lock);
			memcpy(spec,  g_taps[t].out_spec,  sizeof(spec));
			memcpy(scope, g_taps[t].out_scope, sizeof(scope));
			rms_dB = g_taps[t].out_rms_dB;
			pthread_mutex_unlock(&g_taps[t].out_lock);
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n,
				      "%s{\"k\":%d,\"a\":%d,\"b\":%d,\"rms\":%.1f,\"s\":[",
				      t ? "," : "", k, aa, bb, rms_dB);
			for (int i = 0; i < TAP_BINS_OUT && n < (int)sizeof(g_ctl_reply); i++)
				n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%d",
					      i ? "," : "", (int)spec[i]);
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "],\"x\":[");
			for (int i = 0; i < TAP_SCOPE_N * 2 && n < (int)sizeof(g_ctl_reply); i++)
				n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "%s%d",
					      i ? "," : "", (int)scope[i]);
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "]}");
		}
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "]}\n");
		write(fd, g_ctl_reply, strlen(g_ctl_reply));

	} else if (json_has_op(line, "set_tap")) {
		int t, k, a, b;
		if (json_get_int(line, "tap",  &t) < 0 ||
		    json_get_int(line, "kind", &k) < 0 ||
		    t < 0 || t >= N_TAPS) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_tap args\"}\n");
			return;
		}
		if (json_get_int(line, "a", &a) < 0) a = 0;
		if (json_get_int(line, "b", &b) < 0) b = -1;
		int amax = 0;
		switch (k) {
		case TAP_KIND_NONE:    amax = 0;             break;
		case TAP_KIND_INPUT:   amax = N_INPUT_TOTAL; break;
		case TAP_KIND_BUS_PRE: amax = N_BUS_FX_CH;   break;
		case TAP_KIND_OUTPUT:  amax = N_OUTPUT_TOTAL;break;
		default:
			dprintf(fd, "{\"ok\":false,\"err\":\"bad kind\"}\n");
			return;
		}
		if (k != TAP_KIND_NONE &&
		    (a < 0 || a >= amax || (b >= 0 && b >= amax))) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad a/b for kind\"}\n");
			return;
		}
		atomic_store_explicit(&g_taps[t].a, a, memory_order_relaxed);
		atomic_store_explicit(&g_taps[t].b, b, memory_order_relaxed);
		atomic_store_explicit(&g_taps[t].kind, k, memory_order_release);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_tap\",\"tap\":%d,\"kind\":%d,"
			    "\"a\":%d,\"b\":%d}\n", t, k, a, b);

	} else if (json_has_op(line, "get_taps")) {
		char g_ctl_reply[256];
		int n = snprintf(g_ctl_reply, sizeof(g_ctl_reply), "{\"ok\":true,\"taps\":[");
		for (int t = 0; t < N_TAPS; t++) {
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n,
				      "%s{\"k\":%d,\"a\":%d,\"b\":%d}",
				      t ? "," : "",
				      atomic_load_explicit(&g_taps[t].kind, memory_order_relaxed),
				      atomic_load_explicit(&g_taps[t].a,    memory_order_relaxed),
				      atomic_load_explicit(&g_taps[t].b,    memory_order_relaxed));
		}
		snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "]}\n");
		write(fd, g_ctl_reply, strlen(g_ctl_reply));

	} else if (json_has_op(line, "get_drift")) {
		/* V8.1.b — drift USB↔DSP mesuré passivement par cap_uac2_thread.
		 * V8.2 — shift_ppm = correction adaptative par feedback xrun.
		 * Un seul drift partagé play/cap (même horloge USB host). */
		int x100 = atomic_load(&g_usb_drift_ppm_x100);
		int valid = atomic_load(&g_usb_drift_valid);
		int shift = atomic_load(&g_shift_ppm);
		unsigned long xc = atomic_load(&g_ring_uac2_cap.xruns);
		unsigned long xp = atomic_load(&g_ring_uac2_play.xruns);
		unsigned long dp = atomic_load(&g_ring_uac2_play.drops);
		/* V8.6 — 4 compteurs d'events ring (diag) : full+empty cap+play. */
		unsigned long cfe = atomic_load(&g_ring_uac2_cap.drops_evt);
		unsigned long cee = atomic_load(&g_ring_uac2_cap.empty_evt);
		unsigned long pfe = atomic_load(&g_ring_uac2_play.drops_evt);
		unsigned long pee = atomic_load(&g_ring_uac2_play.empty_evt);
		unsigned long ri = atomic_load(&g_dbg_corr_req_insert);
		unsigned long rd = atomic_load(&g_dbg_corr_req_drop);
		unsigned long ai = atomic_load(&g_dbg_corr_app_insert);
		unsigned long ad = atomic_load(&g_dbg_corr_app_drop);
		/* V8.32 — Stats timing : min/max globaux persistants,
		 * moyenne sur fenêtre glissante de TIMING_WINDOW_SEC buckets. */
		struct timespec n_now;
		clock_gettime(CLOCK_MONOTONIC, &n_now);
		uint64_t now_sec = (uint64_t)n_now.tv_sec;
		uint64_t wr_sum = 0, rd_sum = 0;
		uint32_t wr_n = 0, rd_n = 0;
		for (int k = 0; k < TIMING_WINDOW_SEC; k++) {
			uint64_t e = atomic_load_explicit(&g_wr_bucket_epoch[k], memory_order_relaxed);
			if (e != 0 && now_sec - e < TIMING_WINDOW_SEC) {
				wr_sum += atomic_load_explicit(&g_wr_bucket_sum[k], memory_order_relaxed);
				wr_n   += atomic_load_explicit(&g_wr_bucket_cnt[k], memory_order_relaxed);
			}
			e = atomic_load_explicit(&g_rd_bucket_epoch[k], memory_order_relaxed);
			if (e != 0 && now_sec - e < TIMING_WINDOW_SEC) {
				rd_sum += atomic_load_explicit(&g_rd_bucket_sum[k], memory_order_relaxed);
				rd_n   += atomic_load_explicit(&g_rd_bucket_cnt[k], memory_order_relaxed);
			}
		}
		uint32_t wr_avg = wr_n ? (uint32_t)(wr_sum / wr_n) : 0;
		uint32_t rd_avg = rd_n ? (uint32_t)(rd_sum / rd_n) : 0;
		uint32_t wr_min = atomic_load_explicit(&g_wr_min_us, memory_order_relaxed);
		uint32_t wr_max = atomic_load_explicit(&g_wr_max_us, memory_order_relaxed);
		uint32_t rd_min = atomic_load_explicit(&g_rd_min_us, memory_order_relaxed);
		uint32_t rd_max = atomic_load_explicit(&g_rd_max_us, memory_order_relaxed);
		if (wr_min == UINT32_MAX) wr_min = 0;
		if (rd_min == UINT32_MAX) rd_min = 0;
		unsigned long n1 = atomic_load(&g_dbg_readi_lt10);
		unsigned long n2 = atomic_load(&g_dbg_readi_10_50);
		unsigned long n3 = atomic_load(&g_dbg_readi_50_100);
		unsigned long n4 = atomic_load(&g_dbg_readi_ge100);
		/* V9.1 — wake jitter avg = sum/count (en µs) */
		long wj_sum = atomic_load(&g_wake_jitter_sum_us);
		unsigned long wj_cnt = atomic_load(&g_wake_jitter_count);
		long wj_avg = wj_cnt ? (wj_sum / (long)wj_cnt) : 0;
		long wj_max = atomic_load(&g_wake_jitter_max_us);
		unsigned long it_lt18  = atomic_load(&g_iter_lt18);
		unsigned long it_18_22 = atomic_load(&g_iter_18_22);
		unsigned long it_22_30 = atomic_load(&g_iter_22_30);
		unsigned long it_30_50 = atomic_load(&g_iter_30_50);
		unsigned long it_ge50  = atomic_load(&g_iter_ge50);

		char g_ctl_reply[1200];
		snprintf(g_ctl_reply, sizeof(g_ctl_reply),
		         "{\"ok\":true,\"drift_ppm\":%.2f,\"valid\":%d,"
		         "\"shift_ppm\":%d,"
		         "\"xruns_cap\":%lu,\"xruns_play\":%lu,\"drops_play\":%lu,"
		         "\"cap_full_evt\":%lu,\"cap_empty_evt\":%lu,"
		         "\"play_full_evt\":%lu,\"play_empty_evt\":%lu,"
		         "\"corr_req_insert\":%lu,\"corr_app_insert\":%lu,"
		         "\"corr_req_drop\":%lu,\"corr_app_drop\":%lu,"
		         "\"readi_lt10\":%lu,\"readi_10_50\":%lu,"
		         "\"readi_50_100\":%lu,\"readi_ge100\":%lu,"
		         "\"cc_called\":%lu,\"cc_nonzero\":%lu,\"corr_acc_max\":%d,"
		         "\"uac2_cap_fill\":%u,\"uac2_play_fill\":%u,"
		         "\"uac2_cap_mode\":%d,\"uac2_play_mode\":%d,"
		         "\"uac2_cap_warm\":%d,\"uac2_play_warm\":%d,"
		         "\"wr_us_min\":%u,\"wr_us_max\":%u,\"wr_us_avg\":%u,"
		         "\"rd_us_min\":%u,\"rd_us_max\":%u,\"rd_us_avg\":%u,"
		         "\"wake_jit_max_us\":%ld,\"wake_jit_avg_us\":%ld,"
		         "\"iter_lt18\":%lu,\"iter_18_22\":%lu,"
		         "\"iter_22_30\":%lu,\"iter_30_50\":%lu,\"iter_ge50\":%lu}\n",
		         (double)x100 / 100.0, valid, shift,
		         xc, xp, dp, cfe, cee, pfe, pee,
		         ri, ai, rd, ad, n1, n2, n3, n4,
		         atomic_load(&g_dbg_cc_called),
		         atomic_load(&g_dbg_cc_nonzero),
		         atomic_load(&g_dbg_corr_acc_max),
		         uac2_ring_fill(&g_ring_uac2_cap),
		         uac2_ring_fill(&g_ring_uac2_play),
		         atomic_load(&g_uac2_cap_mode),
		         atomic_load(&g_uac2_play_mode),
		         atomic_load(&g_uac2_cap_warm),
		         atomic_load(&g_uac2_play_warm),
		         wr_min, wr_max, wr_avg,
		         rd_min, rd_max, rd_avg,
		         wj_max, wj_avg,
		         it_lt18, it_18_22, it_22_30, it_30_50, it_ge50);
		write(fd, g_ctl_reply, strlen(g_ctl_reply));

	} else if (json_has_op(line, "apply_drift_as_shift")) {
		/* V8.14 — force shift_ppm = round(drift_ppm) en un coup,
		 * sans attendre que shift_controller_thread accumule. */
		int x100 = atomic_load(&g_usb_drift_ppm_x100);
		int shift_target = (x100 >= 0) ? (x100 + 50) / 100
		                               : (x100 - 50) / 100;
		atomic_store(&g_shift_ppm, shift_target);
		dprintf(fd,
		        "{\"ok\":true,\"op\":\"apply_drift_as_shift\","
		        "\"shift_ppm\":%d,\"drift_ppm_x100\":%d}\n",
		        shift_target, x100);

	} else if (json_has_op(line, "reset_drift_stats")) {
		/* V8.12 — Reset complet des stats drift/ring : remet à zéro
		 * shift, drift mesuré, et TOUS les compteurs (xruns/drops/events)
		 * sur cap et play. Utile pour repartir d'une base propre après
		 * un démarrage transient, sans redémarrer mixer-pro. */
		atomic_store(&g_shift_ppm, 0);
		atomic_store(&g_usb_drift_ppm_x100, 0);
		atomic_store(&g_usb_drift_valid, 0);
		atomic_store(&g_ring_uac2_cap.xruns,      0);
		atomic_store(&g_ring_uac2_cap.drops,      0);
		atomic_store(&g_ring_uac2_cap.drops_evt,  0);
		atomic_store(&g_ring_uac2_cap.empty_evt,  0);
		atomic_store(&g_ring_uac2_play.xruns,     0);
		atomic_store(&g_ring_uac2_play.drops,     0);
		atomic_store(&g_ring_uac2_play.drops_evt, 0);
		atomic_store(&g_ring_uac2_play.empty_evt, 0);
		/* V8.32 — reset stats timing wr/rd (min/max + buckets) */
		atomic_store(&g_wr_min_us, UINT32_MAX);
		atomic_store(&g_wr_max_us, 0);
		atomic_store(&g_rd_min_us, UINT32_MAX);
		atomic_store(&g_rd_max_us, 0);
		for (int k = 0; k < TIMING_WINDOW_SEC; k++) {
			atomic_store(&g_wr_bucket_sum[k], 0);
			atomic_store(&g_wr_bucket_cnt[k], 0);
			atomic_store(&g_wr_bucket_epoch[k], 0);
			atomic_store(&g_rd_bucket_sum[k], 0);
			atomic_store(&g_rd_bucket_cnt[k], 0);
			atomic_store(&g_rd_bucket_epoch[k], 0);
		}
		/* V9.1 — reset histogram prof_iter + wake_jitter */
		atomic_store(&g_iter_lt18, 0);
		atomic_store(&g_iter_18_22, 0);
		atomic_store(&g_iter_22_30, 0);
		atomic_store(&g_iter_30_50, 0);
		atomic_store(&g_iter_ge50, 0);
		atomic_store(&g_wake_jitter_max_us, 0);
		atomic_store(&g_wake_jitter_sum_us, 0);
		atomic_store(&g_wake_jitter_count, 0);
		dprintf(fd, "{\"ok\":true,\"op\":\"reset_drift_stats\"}\n");

	} else if (json_has_op(line, "reset")) {
		pthread_mutex_lock(&g_st.target_lock);
		memset(g_st.send_target,   0, sizeof(g_st.send_target));
		memset(g_st.master_target, 0, sizeof(g_st.master_target));
		for (int b = 0; b < N_BUS_FX_CH; b++)
			g_st.fx_bus_target[b] = 1.0f;
		for (int i = 0; i < N_INPUT_TOTAL; i++)
			g_st.input_target[i] = 1.0f;
		g_st.mute_mask = 0;
		pthread_mutex_unlock(&g_st.target_lock);
		dprintf(fd, "{\"ok\":true,\"op\":\"reset\"}\n");

	} else {
		dprintf(fd, "{\"ok\":false,\"err\":\"unknown op\"}\n");
	}
}

void *control_thread(void *arg)
{
	(void)arg;
	/* V9.0 — pin sur cores 0,1 (non-RT, hors des cores isolcpus audio) */
	{
		cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0, &cs); CPU_SET(1, &cs);
		pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
	}
	mlog("control_thread : SCHED_OTHER cores 0,1");
	int srv = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv < 0) { mlog("socket: %s", strerror(errno)); return NULL; }

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, MIXER_SOCK_PATH, sizeof(addr.sun_path) - 1);
	unlink(MIXER_SOCK_PATH);

	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		mlog("bind %s: %s", MIXER_SOCK_PATH, strerror(errno));
		close(srv);
		return NULL;
	}
	chmod(MIXER_SOCK_PATH, 0660);
	if (listen(srv, 8) < 0) {
		mlog("listen: %s", strerror(errno));
		close(srv);
		return NULL;
	}
	mlog("control socket listening on %s", MIXER_SOCK_PATH);

	/* V10-N1.4 — MULTI-CLIENT (poll) : l'ancienne boucle servait UN client
	 * jusqu'à EOF — une connexion persistante (app native mixer-console,
	 * meters 30 Hz) affamait tous les autres (mixer-gui-http = GUI web).
	 * Jusqu'à CTL_MAX_CLIENTS simultanés, buffer d'accumulation PAR client
	 * (V9.4.3 : 16 KB pour les blobs DRC hex ; l'ancien buffer static
	 * unique aurait d'ailleurs été une corruption en multi-client).
	 * handle_cmd (dprintf bloquant) inchangé : clients locaux de confiance,
	 * risque d'un client-qui-ne-lit-pas identique à l'existant. */
#define CTL_MAX_CLIENTS 8
	static struct {
		int fd;
		size_t pos;
		char buf[16384];
	} cl[CTL_MAX_CLIENTS];
	for (int i = 0; i < CTL_MAX_CLIENTS; i++)
		cl[i].fd = -1;

	while (atomic_load(&g_st.running)) {
		struct pollfd pfd[1 + CTL_MAX_CLIENTS];
		int idx_of[1 + CTL_MAX_CLIENTS];
		nfds_t nf = 0;
		pfd[nf].fd = srv;
		pfd[nf].events = POLLIN;
		idx_of[nf++] = -1;
		for (int i = 0; i < CTL_MAX_CLIENTS; i++) {
			if (cl[i].fd < 0)
				continue;
			pfd[nf].fd = cl[i].fd;
			pfd[nf].events = POLLIN;
			idx_of[nf++] = i;
		}

		int pr = poll(pfd, nf, 500);
		if (pr < 0) {
			if (errno == EINTR) continue;
			mlog("poll: %s", strerror(errno));
			break;
		}
		if (pr == 0)
			continue;

		for (nfds_t k = 0; k < nf; k++) {
			if (!(pfd[k].revents & (POLLIN | POLLERR | POLLHUP)))
				continue;

			if (idx_of[k] < 0) {          /* socket serveur : accept */
				int c = accept(srv, NULL, NULL);
				if (c < 0)
					continue;
				int slot = -1;
				for (int i = 0; i < CTL_MAX_CLIENTS; i++)
					if (cl[i].fd < 0) { slot = i; break; }
				if (slot < 0) {
					dprintf(c, "{\"ok\":false,\"err\":\"too many clients\"}\n");
					close(c);
					continue;
				}
				cl[slot].fd = c;
				cl[slot].pos = 0;
				continue;
			}

			int i = idx_of[k];
			ssize_t n = read(cl[i].fd, cl[i].buf + cl[i].pos,
					 sizeof(cl[i].buf) - 1 - cl[i].pos);
			if (n <= 0) {                 /* EOF ou erreur : libère */
				close(cl[i].fd);
				cl[i].fd = -1;
				continue;
			}
			cl[i].pos += (size_t)n;
			cl[i].buf[cl[i].pos] = 0;
			char *line = cl[i].buf, *next;
			while (line && *line) {
				next = strchr(line, '\n');
				if (!next) break;         /* ligne incomplète */
				*next++ = 0;
				if (*line) handle_cmd(cl[i].fd, line);
				line = next;
			}
			if (line && *line) {
				size_t rem = strlen(line);
				memmove(cl[i].buf, line, rem);
				cl[i].pos = rem;
			} else {
				cl[i].pos = 0;
			}
			/* ligne plus longue que le buffer : reset défensif */
			if (cl[i].pos >= sizeof(cl[i].buf) - 1)
				cl[i].pos = 0;
		}
	}

	for (int i = 0; i < CTL_MAX_CLIENTS; i++)
		if (cl[i].fd >= 0)
			close(cl[i].fd);
	close(srv);
	unlink(MIXER_SOCK_PATH);
	return NULL;
}

/* Persistance + scènes : déplacées dans persist.c/persist.h (V14.0 étape 2e). */

