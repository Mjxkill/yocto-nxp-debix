// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fx_ops — ops control des effets : bus FX natifs, insert mastering LV2,
 * mode assistant. Domaine « effets » (le découpage complet d'effects.c en
 * modules arrive à l'étape 5 — ces ops y migreront avec lui).
 * Corps déplacés tels quels depuis handle_cmd (V14.0 étape 4b, extraction
 * pure). Proto du handler : effects.h.
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "state.h"       /* g_st.fx_engines, insert, assistant */
#include "util.h"        /* mlog */
#include "effects.h"
#include "control.h"

/* V14.0 étape 4b : ops fx/insert/assistant — domaine effets (le module
 * effects/ complet arrive à l'étape 5). Corps déplacés tels quels
 * depuis handle_cmd (extraction pure). */
int fx_handle_op(int fd, const char *line)
{
	if (json_has_op(line, "set_fx_param")) {
		int bus;
		char param[32];
		float value = 0;
		if (json_get_int(line, "bus", &bus) < 0 ||
		    json_get_str(line, "param", param, sizeof(param)) < 0 ||
		    json_get_float(line, "value", &value) < 0 ||
		    bus < 0 || bus >= N_BUS_FX) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_fx_param args\"}\n");
			return 1;
		}
		pthread_mutex_lock(&g_st.target_lock);
		int rc = g_st.fx_engines[bus].set_param(&g_st.fx_engines[bus], param, value);
		pthread_mutex_unlock(&g_st.target_lock);
		if (rc < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"unknown fx param\"}\n");
		} else {
			atomic_store(&g_presets_dirty, 1);  /* V9.3.5 */
			snprintf(g_ctl_reply, sizeof(g_ctl_reply),
				 "{\"ok\":true,\"op\":\"set_fx_param\",\"bus\":%d,"
				 "\"param\":\"%s\",\"value\":%.4f}\n",
				 bus, param, value);
			write(fd, g_ctl_reply, strlen(g_ctl_reply));
		}
		return 1;
	}
	if (json_has_op(line, "get_fx")) {
		int bus;
		if (json_get_int(line, "bus", &bus) < 0 ||
		    bus < 0 || bus >= N_BUS_FX) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad get_fx args\"}\n");
			return 1;
		}
		/* V9.3.3 : 8 KB pour tenir params + ranges (NPU). LSP MB Comp x8
		 * a ~200 params × ~30 chars = 6 KB + ranges 6 KB → 12 KB sécurité. */
		static char body[49152];
		g_st.fx_engines[bus].get_state(&g_st.fx_engines[bus], body, sizeof(body));
		snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			 "{\"ok\":true,\"bus\":%d,%s}\n", bus, body);
		write(fd, g_ctl_reply, strlen(g_ctl_reply));
		return 1;
	}
	if (json_has_op(line, "set_fx_engine")) {
		/* V9.2 — Change l'engine d'un bus FX. Engines builtin (compressor,
		 * reverb, delay, eq) OU LV2 plugin par URI.
		 * Format : {"op":"set_fx_engine","bus":N,"engine":"lv2","uri":"..."}
		 * Pour engines builtin : "engine":"compressor"|"reverb"|"delay"|"eq"
		 */
		int bus;
		char engine[32];
		char uri[256] = "";
		if (json_get_int(line, "bus", &bus) < 0 ||
		    json_get_str(line, "engine", engine, sizeof(engine)) < 0 ||
		    bus < 0 || bus >= N_BUS_FX) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_fx_engine args\"}\n");
			return 1;
		}
		int uri_set = (json_get_str(line, "uri", uri, sizeof(uri)) >= 0);

		fx_engine_t new_eng = {0};
		int ok = 0;
		if (!strcmp(engine, "passthrough")) ok = fx_init_passthrough(&new_eng, (float)SAMPLE_RATE);
		else if (!strcmp(engine, "compressor")) ok = fx_init_compressor(&new_eng, (float)SAMPLE_RATE);
		else if (!strcmp(engine, "reverb"))     ok = fx_init_reverb    (&new_eng, (float)SAMPLE_RATE);
		else if (!strcmp(engine, "delay"))      ok = fx_init_delay     (&new_eng, (float)SAMPLE_RATE);
		else if (!strcmp(engine, "eq"))         ok = fx_init_eq        (&new_eng, (float)SAMPLE_RATE);
		else if (!strcmp(engine, "lv2") && uri_set)
			ok = fx_init_lv2(&new_eng, (float)SAMPLE_RATE, uri);

		if (!ok) {
			dprintf(fd, "{\"ok\":false,\"err\":\"engine init failed\","
			        "\"engine\":\"%s\",\"uri\":\"%s\"}\n", engine, uri);
			return 1;
		}

		/* Swap atomic sous mutex. fx_free de l'ancien APRÈS swap pour que
		 * audio_thread voie toujours un engine valide. */
		pthread_mutex_lock(&g_st.target_lock);
		fx_engine_t old_eng = g_st.fx_engines[bus];
		g_st.fx_engines[bus] = new_eng;
		pthread_mutex_unlock(&g_st.target_lock);
		fx_free(&old_eng);
		atomic_store(&g_presets_dirty, 1);  /* V9.3.5 */

		snprintf(g_ctl_reply, sizeof(g_ctl_reply),
			 "{\"ok\":true,\"op\":\"set_fx_engine\",\"bus\":%d,"
			 "\"engine\":\"%s\",\"uri\":\"%s\"}\n",
			 bus, engine, uri);
		write(fd, g_ctl_reply, strlen(g_ctl_reply));
		return 1;
	}
	if (json_has_op(line, "reset_fx")) {
		int bus;
		if (json_get_int(line, "bus", &bus) < 0 ||
		    bus < 0 || bus >= N_BUS_FX) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad reset_fx args\"}\n");
			return 1;
		}
		pthread_mutex_lock(&g_st.target_lock);
		g_st.fx_engines[bus].reset(&g_st.fx_engines[bus]);
		pthread_mutex_unlock(&g_st.target_lock);
		dprintf(fd, "{\"ok\":true,\"op\":\"reset_fx\",\"bus\":%d}\n", bus);
		return 1;
	}
	if (json_has_op(line, "list_lv2_plugins")) {
		/* V9.2 — Énumère les plugins LV2 RT-safe disponibles. */
		static char lv2_buf[65536];
		int n = fx_lv2_list_uris(lv2_buf, sizeof(lv2_buf));
		dprintf(fd, "{\"ok\":true,\"op\":\"list_lv2_plugins\",\"plugins\":%s}\n",
		        n > 0 ? lv2_buf : "[]");
		return 1;
	}
	if (json_has_op(line, "set_insert")) {
		/* V9.4 — Configure la chaîne insert post-master.
		 * Format : {"op":"set_insert","plugins":[
		 *   {"engine":"lv2","uri":"http://..."},
		 *   {"engine":"compressor"},
		 *   ...
		 * ]}
		 * plugins:[] = bypass (insert désactivé).
		 *
		 * Parser ad-hoc : itère sur les `{...}` contenus entre `"plugins":[`
		 * et le matching `]`. Pour chaque, extrait engine + uri. Limite
		 * FX_CHAIN_MAX (8) plugins. */
		const char *p = strstr(line, "\"plugins\"");
		if (!p) { dprintf(fd, "{\"ok\":false,\"err\":\"missing plugins\"}\n"); return 1; }
		p = strchr(p, '['); if (!p) { dprintf(fd, "{\"ok\":false,\"err\":\"bad plugins array\"}\n"); return 1; }
		p++;
		struct fx_chain_spec specs[FX_CHAIN_MAX];
		char engines[FX_CHAIN_MAX][32], uris[FX_CHAIN_MAX][256];
		int n_specs = 0;
		while (*p && *p != ']' && n_specs < FX_CHAIN_MAX) {
			const char *brace = strchr(p, '{');
			if (!brace) break;
			const char *end = strchr(brace, '}');
			if (!end) break;
			char obj[512];
			size_t len_obj = (size_t)(end - brace + 1);
			if (len_obj >= sizeof(obj)) len_obj = sizeof(obj) - 1;
			memcpy(obj, brace, len_obj); obj[len_obj] = '\0';
			engines[n_specs][0] = '\0';
			uris[n_specs][0] = '\0';
			(void)json_get_str(obj, "engine", engines[n_specs], sizeof(engines[0]));
			(void)json_get_str(obj, "uri",     uris[n_specs],    sizeof(uris[0]));
			specs[n_specs].engine = engines[n_specs];
			specs[n_specs].uri    = uris[n_specs];
			n_specs++;
			p = end + 1;
		}

		if (n_specs == 0) {
			/* Bypass : désactive l'insert + free chain existante */
			pthread_mutex_lock(&g_st.target_lock);
			int was_active = atomic_exchange(&g_insert_active, 0);
			g_insert_spec_n = 0;   /* V9.5.21b : persiste le bypass */
			pthread_mutex_unlock(&g_st.target_lock);
			if (was_active) fx_free(&g_insert_chain);
			dprintf(fd, "{\"ok\":true,\"op\":\"set_insert\",\"n\":0}\n");
			atomic_store(&g_presets_dirty, 1);
			return 1;
		}

		fx_engine_t new_chain = {0};
		if (!fx_init_chain(&new_chain, (float)SAMPLE_RATE, specs, n_specs)) {
			dprintf(fd, "{\"ok\":false,\"err\":\"chain init failed\"}\n");
			return 1;
		}

		pthread_mutex_lock(&g_st.target_lock);
		fx_engine_t old_chain = g_insert_chain;
		int was_active = atomic_load(&g_insert_active);
		g_insert_chain = new_chain;
		atomic_store(&g_insert_active, 1);
		/* V9.5.21b : copie de la spec pour persistance */
		g_insert_spec_n = n_specs;
		for (int i = 0; i < n_specs; i++) {
			strncpy(g_insert_spec_engine[i], engines[i], sizeof(g_insert_spec_engine[0]) - 1);
			g_insert_spec_engine[i][sizeof(g_insert_spec_engine[0]) - 1] = '\0';
			strncpy(g_insert_spec_uri[i], uris[i], sizeof(g_insert_spec_uri[0]) - 1);
			g_insert_spec_uri[i][sizeof(g_insert_spec_uri[0]) - 1] = '\0';
		}
		pthread_mutex_unlock(&g_st.target_lock);
		if (was_active) fx_free(&old_chain);
		atomic_store(&g_presets_dirty, 1);

		dprintf(fd, "{\"ok\":true,\"op\":\"set_insert\",\"n\":%d}\n", n_specs);
		return 1;
	}
	if (json_has_op(line, "set_insert_param")) {
		/* Format : {"op":"set_insert_param","slot":N,"param":"name","value":X} */
		int slot;
		char param[32]; float value = 0;
		if (json_get_int(line, "slot", &slot) < 0 ||
		    json_get_str(line, "param", param, sizeof(param)) < 0 ||
		    json_get_float(line, "value", &value) < 0 ||
		    slot < 0 || slot >= FX_CHAIN_MAX) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad args\"}\n"); return 1;
		}
		if (!atomic_load(&g_insert_active)) {
			dprintf(fd, "{\"ok\":false,\"err\":\"insert not active\"}\n"); return 1;
		}
		/* Construit "<slot>/<param>" pour chain_set_param.
		 * V9.5.12 — PAS de target_lock : chain.set_param est interne
		 * lock-free (LV2 = atomic ctrl_target write, para_eq_x16 = direct
		 * struct write + biquad recalc). Lock contention avec audio_thread
		 * RT99 causait xrun + kernel freeze sous flux dense (50+ Hz). */
		char composite[64];
		snprintf(composite, sizeof(composite), "%d/%s", slot, param);
		int rc = g_insert_chain.set_param(&g_insert_chain, composite, value);
		if (rc < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"unknown param or slot\"}\n");
		} else {
			atomic_store(&g_presets_dirty, 1);
			dprintf(fd, "{\"ok\":true,\"op\":\"set_insert_param\",\"slot\":%d,"
			            "\"param\":\"%s\",\"value\":%.4f}\n",
			        slot, param, value);
		}
		return 1;
	}
	if (json_has_op(line, "set_insert_params_bulk")) {
		/* V9.5.5 : set N params en 1 seule call HTTP pour 50 Hz update NPU.
		 *
		 * Format : {"op":"set_insert_params_bulk","params":[
		 *   [slot, "name", value],
		 *   [slot, "name", value],
		 *   ...
		 * ]}
		 *
		 * Parser ad-hoc : itère sur les `[slot,"name",value]` entre `"params":[`
		 * et le matching `]` final. Pour chaque triple, set le param.
		 * Tous les sets sont effectués sous un seul lock pour cohérence atomic. */
		if (!atomic_load(&g_insert_active)) {
			dprintf(fd, "{\"ok\":false,\"err\":\"insert not active\"}\n"); return 1;
		}
		const char *p = strstr(line, "\"params\"");
		if (!p) { dprintf(fd, "{\"ok\":false,\"err\":\"missing params\"}\n"); return 1; }
		p = strchr(p, '[');
		if (!p) { dprintf(fd, "{\"ok\":false,\"err\":\"bad params array\"}\n"); return 1; }
		p++;
		int n_set = 0, n_fail = 0;
		/* V9.5.12 — PAS de target_lock : chain.set_param est lock-free
		 * en interne (cf set_insert_param ci-dessus). Évite contention
		 * avec audio_thread RT99 sous flux dense (10+ Hz × 76 params). */
		while (*p && *p != ']') {
			/* Find next `[slot,"name",value]` */
			while (*p == ' ' || *p == ',') p++;
			if (*p != '[') break;
			p++;   /* skip '[' */
			while (*p == ' ') p++;
			int slot = atoi(p);
			while (*p && *p != ',') p++;
			if (*p == ',') p++;
			while (*p == ' ') p++;
			if (*p != '"') break;
			p++;
			char pname[32];
			int i_name = 0;
			while (*p && *p != '"' && i_name < (int)sizeof(pname) - 1)
				pname[i_name++] = *p++;
			pname[i_name] = 0;
			if (*p == '"') p++;
			while (*p == ' ' || *p == ',') p++;
			float value = (float)atof(p);
			/* Skip value digits */
			while (*p && *p != ']' && *p != ',') p++;
			while (*p && *p != ']') p++;
			if (*p == ']') p++;
			/* Apply */
			char composite[64];
			snprintf(composite, sizeof(composite), "%d/%s", slot, pname);
			int rc = g_insert_chain.set_param(&g_insert_chain, composite, value);
			if (rc < 0) n_fail++;
			else n_set++;
		}
		/* (target_lock retiré V9.5.12 — voir commentaire avant la boucle) */
		if (n_set > 0) atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_insert_params_bulk\","
		            "\"set\":%d,\"fail\":%d}\n", n_set, n_fail);
		return 1;
	}
	if (json_has_op(line, "get_insert")) {
		/* Dump JSON full : type + n + chain[] avec slot/state/ranges */
		if (!atomic_load(&g_insert_active)) {
			dprintf(fd, "{\"ok\":true,\"active\":false}\n"); return 1;
		}
		static char insert_buf[32768];
		pthread_mutex_lock(&g_st.target_lock);
		int n = g_insert_chain.get_state(&g_insert_chain, insert_buf, sizeof(insert_buf));
		pthread_mutex_unlock(&g_st.target_lock);
		dprintf(fd, "{\"ok\":true,\"active\":true,%s}\n", n > 0 ? insert_buf : "");
		return 1;
	}
	if (json_has_op(line, "insert_bypass")) {
		/* Format : {"op":"insert_bypass","bypass":true|false}.
		 * Quand bypass=true : désactive l'insert sans free la chain
		 * (réactivable par bypass=false instantanément). */
		int bypass_flag = 1;   /* default true si pas spécifié */
		(void)json_get_int(line, "bypass", &bypass_flag);
		atomic_store(&g_insert_active, bypass_flag ? 0 : 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"insert_bypass\",\"active\":%s}\n",
		        bypass_flag ? "false" : "true");
		return 1;
	}
	if (json_has_op(line, "set_insert_bypass")) {
		/* V13-SCENES : bouton MASTERING ON/OFF (chaîne gardée chaude) */
		int on = 0;
		(void)json_get_int(line, "on", &on);
		atomic_store(&g_insert_bypass, on ? 0 : 1);   /* on=1 → actif */
		dprintf(fd, "{\"ok\":true,\"mastering_on\":%d}\n", on ? 1 : 0);
		return 1;
	}
	if (json_has_op(line, "get_insert_bypass")) {
		dprintf(fd, "{\"ok\":true,\"chain\":%d,\"bypass\":%d,"
			"\"mastering_on\":%d}\n",
			atomic_load(&g_insert_active),
			atomic_load(&g_insert_bypass),
			atomic_load(&g_insert_active) &&
			!atomic_load(&g_insert_bypass));
		return 1;
	}
	if (json_has_op(line, "set_assistant_mode")) {
		/* V9.5.12 — Stocke l'état Mixer Assistant. mixer-pro ne fait PAS
		 * d'inférence (process séparé mixer-ml-inference s'en charge,
		 * pour éviter freeze kernel TFLite+galcore+RT99). Le daemon poll
		 * get_assistant pour savoir quoi faire.
		 *
		 * Format : {"op":"set_assistant_mode","mode":"mastering"|"passthrough",
		 *          "source":"hw"|"usb"}    (source optionnel, défaut hw)
		 */
		char mode_str[32] = "", src_str[8] = "";
		(void)json_get_str(line, "mode",   mode_str, sizeof(mode_str));
		(void)json_get_str(line, "source", src_str,  sizeof(src_str));
		int mode = (strcmp(mode_str, "mastering") == 0) ? 1 : 0;
		int src  = (strcmp(src_str,  "usb")       == 0) ? 1 : 0;
		atomic_store_explicit(&g_assistant_mode,   mode, memory_order_release);
		atomic_store_explicit(&g_assistant_source, src,  memory_order_release);
		atomic_store(&g_presets_dirty, 1);   /* V9.5.21b : persistance */
		dprintf(fd, "{\"ok\":true,\"op\":\"set_assistant_mode\","
		            "\"mode\":\"%s\",\"source\":\"%s\"}\n",
		        mode ? "mastering" : "passthrough",
		        src  ? "usb"       : "hw");
		return 1;
	}
	if (json_has_op(line, "get_assistant")) {
		/* Renvoie état Mixer Assistant. Le daemon mixer-ml-inference
		 * poll cet endpoint pour savoir source/mode actuels. */
		int mode = atomic_load_explicit(&g_assistant_mode,   memory_order_relaxed);
		int src  = atomic_load_explicit(&g_assistant_source, memory_order_relaxed);
		dprintf(fd, "{\"ok\":true,\"mode\":\"%s\",\"source\":\"%s\"}\n",
		        mode ? "mastering" : "passthrough",
		        src  ? "usb"       : "hw");
		return 1;
	}
	return 0;
}
