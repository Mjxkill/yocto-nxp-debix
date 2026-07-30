// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lv2_load — chargement d'un plugin LV2 : validation des features
 * requises, instanciation (+2e instance mono→stéréo), harvest des
 * métadonnées UI V9.5.21 (groupes, unités, scale points), catalogue
 * (fx_lv2_list_uris). Code déplacé tel quel (V14.0 étape 5).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effects.h"
#include "fx_internal.h"
#include "fx_lv2.h"

int fx_init_lv2(fx_engine_t *fx, float sample_rate, const char *uri)
{
	if (!uri || !*uri) return 0;
	if (!lv2_world_init()) return 0;

	LilvNode *plug_uri = lilv_new_uri(g_lv2_world, uri);
	const LilvPlugin *plug = lilv_plugins_get_by_uri(g_lv2_plugins, plug_uri);
	lilv_node_free(plug_uri);
	if (!plug) {
		fprintf(stderr, "LV2: plugin %s not found\n", uri);
		return 0;
	}

	/* RT safety filter — refuse plugin sans hardRTCapable */
	if (!lilv_plugin_has_feature(plug, g_uri_hard_rt)) {
		fprintf(stderr, "LV2: %s NOT hardRTCapable — refused\n", uri);
		return 0;
	}

	/* V9.2-step5c : vérifier que toutes les required_features sont
	 * supportées par l'host (sinon plugin va segfault à activate ou run).
	 * Refus propre avec log de la feature manquante. */
	int needs_worker = 0;
	LilvNodes *req = lilv_plugin_get_required_features(plug);
	if (req) {
		LILV_FOREACH(nodes, it, req) {
			const LilvNode *f = lilv_nodes_get(req, it);
			const char *furi = lilv_node_as_uri(f);
			if (!lv2_host_supports_feature(furi)) {
				fprintf(stderr, "LV2: %s requires unsupported feature '%s' — refused\n",
				        uri, furi ? furi : "(null)");
				lilv_nodes_free(req);
				return 0;
			}
			if (furi && strcmp(furi, LV2_WORKER__schedule) == 0)
				needs_worker = 1;
		}
		lilv_nodes_free(req);
	}
	/* Optional worker support : si plugin l'OFFRE même sans le require, on
	 * lui donne aussi (certains plugins comme calf l'utilisent en optional). */
	if (!needs_worker) {
		LilvNodes *opt = lilv_plugin_get_optional_features(plug);
		if (opt) {
			LILV_FOREACH(nodes, it, opt) {
				const char *furi = lilv_node_as_uri(lilv_nodes_get(opt, it));
				if (furi && strcmp(furi, LV2_WORKER__schedule) == 0) {
					needs_worker = 1;
					break;
				}
			}
			lilv_nodes_free(opt);
		}
	}

	struct lv2_state *st = calloc(1, sizeof(*st));
	if (!st) return 0;
	st->sr  = sample_rate;
	st->uri = strdup(uri);
	st->plugin = plug;
	st->n_ports = (int)lilv_plugin_get_num_ports(plug);
	st->audio_in_idx[0]  = st->audio_in_idx[1]  = -1;
	st->audio_out_idx[0] = st->audio_out_idx[1] = -1;

	/* V9.2-step5d : si plugin demande worker, alloc + spawn thread + build
	 * local features array incluant LV2_WORKER__schedule. Sinon utilise
	 * g_host_features global. */
	const LV2_Feature **features_to_use = g_host_features;
	LV2_Feature feature_worker_local;
	const LV2_Feature *features_local[8] = { NULL };
	if (needs_worker) {
		st->worker = calloc(1, sizeof(*st->worker));
		if (!st->worker) {
			fprintf(stderr, "LV2: worker alloc failed for %s\n", uri);
			free(st->uri); free(st);
			return 0;
		}
		pthread_mutex_init(&st->worker->mutex, NULL);
		pthread_cond_init(&st->worker->cond, NULL);
		st->worker->schedule.handle        = st->worker;
		st->worker->schedule.schedule_work = lv2_worker_schedule_cb;
		feature_worker_local.URI  = LV2_WORKER__schedule;
		feature_worker_local.data = &st->worker->schedule;

		/* Copy g_host_features puis append worker_schedule */
		features_local[0] = &g_feature_urid_map;
		features_local[1] = &g_feature_options;
		features_local[2] = &feature_worker_local;
		features_local[3] = NULL;
		features_to_use = features_local;
	}

	/* Instantiate avec host features (urid:map + options + optionnel worker) */
	st->instance = lilv_plugin_instantiate(plug, (double)sample_rate, features_to_use);
	if (!st->instance) {
		fprintf(stderr, "LV2: instantiate failed for %s\n", uri);
		if (st->worker) {
			pthread_mutex_destroy(&st->worker->mutex);
			pthread_cond_destroy(&st->worker->cond);
			free(st->worker);
		}
		free(st->uri); free(st);
		return 0;
	}

	/* V9.2-step5d : récupère iface worker + spawn thread maintenant que
	 * instance existe. */
	if (st->worker) {
		const LV2_Worker_Interface *iface = (const LV2_Worker_Interface *)
			lilv_instance_get_extension_data(st->instance, LV2_WORKER__interface);
		if (!iface || !iface->work) {
			fprintf(stderr, "LV2: %s claims worker support but no work() iface — refused\n", uri);
			lilv_instance_free(st->instance);
			pthread_mutex_destroy(&st->worker->mutex);
			pthread_cond_destroy(&st->worker->cond);
			free(st->worker);
			free(st->uri); free(st);
			return 0;
		}
		st->worker->iface         = iface;
		st->worker->plugin_handle = lilv_instance_get_handle(st->instance);
		/* V9.3.4 : worker thread pinné sur cores 0+1 (non-RT) pour ne pas
		 * impacter le jitter audio_thread RT prio 99 sur core 2.
		 * sched OTHER par défaut, peut être préempté librement. */
		pthread_attr_t wattr;
		pthread_attr_init(&wattr);
		cpu_set_t wcpus;
		CPU_ZERO(&wcpus);
		CPU_SET(0, &wcpus);
		CPU_SET(1, &wcpus);
		pthread_attr_setaffinity_np(&wattr, sizeof(wcpus), &wcpus);
		int rc = pthread_create(&st->worker->thread, &wattr,
		                        lv2_worker_thread_fn, st->worker);
		pthread_attr_destroy(&wattr);
		if (rc != 0) {
			fprintf(stderr, "LV2: %s worker pthread_create failed — refused\n", uri);
			lilv_instance_free(st->instance);
			pthread_mutex_destroy(&st->worker->mutex);
			pthread_cond_destroy(&st->worker->cond);
			free(st->worker);
			free(st->uri); free(st);
			return 0;
		}
		fprintf(stderr, "LV2: %s worker thread spawned (cores 0-1)\n", uri);
	}

	/* Get default + min + max control values (V9.3.3 : ranges pour NPU).
	 * Si min/max manquent dans le TTL, lilv met NaN → on garde NaN qui se
	 * sérialise en "null" JSON pour signaler "unbounded" au NPU. */
	float *defaults = calloc(st->n_ports, sizeof(float));
	float *mins = calloc(st->n_ports, sizeof(float));
	float *maxs = calloc(st->n_ports, sizeof(float));
	lilv_plugin_get_port_ranges_float(plug, mins, maxs, defaults);

	/* Scan + connect ports */
	int audio_in_n = 0, audio_out_n = 0;
	for (int i = 0; i < st->n_ports; i++) {
		const LilvPort *port = lilv_plugin_get_port_by_index(plug, i);
		int is_audio  = lilv_port_is_a(plug, port, g_uri_audio_port);
		int is_ctrl   = lilv_port_is_a(plug, port, g_uri_control_port);
		int is_atom   = lilv_port_is_a(plug, port, g_uri_atom_port);
		int is_input  = lilv_port_is_a(plug, port, g_uri_input_port);

		/* V9.3.1.2 : détecter sidechain via symbol "sc..." pour ne pas le
		 * compter comme audio input principal (faussait la détection
		 * mono 1/1 quand plugin avait main+sc, ex: sc_compressor_mono). */
		int is_sidechain = 0;
		if (is_audio && is_input) {
			LilvNode *sym = (LilvNode *)lilv_port_get_symbol(plug, port);
			const char *s = sym ? lilv_node_as_string(sym) : NULL;
			if (s && strncmp(s, "sc", 2) == 0)
				is_sidechain = 1;
		}

		if (is_audio && is_input && !is_sidechain && audio_in_n < 2) {
			st->audio_in_idx[audio_in_n] = i;
			lilv_instance_connect_port(st->instance, i,
				audio_in_n == 0 ? &st->buf_in_l : &st->buf_in_r);
			audio_in_n++;
		} else if (is_audio && is_input) {
			/* V9.3.1.1 : audio input supplémentaire (sidechain ou >2).
			 * Connect au extra_in_silence (zeros) → self-keyed sans signal. */
			lilv_instance_connect_port(st->instance, i, st->extra_in_silence);
		} else if (is_audio && !is_input && audio_out_n < 2) {
			st->audio_out_idx[audio_out_n] = i;
			lilv_instance_connect_port(st->instance, i,
				audio_out_n == 0 ? &st->buf_out_l : &st->buf_out_r);
			audio_out_n++;
		} else if (is_audio && !is_input) {
			/* V9.3.1.1 : audio output supplémentaire (>2). Ex: oscilloscope_x2
			 * a plus de 2 outputs. On les jette dans extra_out_discard. */
			lilv_instance_connect_port(st->instance, i, st->extra_out_discard);
		} else if (is_atom) {
			/* V9.2-step5c : AtomPort = control/automation/notify.
			 * Alloue un buffer 8 KB par port, init en sequence vide,
			 * connecte. Reset à chaque cycle dans lv2_process(). */
			int *cnt = is_input ? &st->n_atom_in : &st->n_atom_out;
			if (*cnt >= LV2_MAX_ATOM_PORTS) {
				fprintf(stderr, "LV2: %s too many atom ports (>%d) — refused\n",
				        uri, LV2_MAX_ATOM_PORTS);
				free(defaults); free(mins); free(maxs);
				/* cleanup partial alloc + return */
				for (int k = 0; k < st->n_atom_in; k++)  free(st->atom_in_bufs[k]);
				for (int k = 0; k < st->n_atom_out; k++) free(st->atom_out_bufs[k]);
				lilv_instance_free(st->instance);
				free(st->uri); free(st);
				return 0;
			}
			uint8_t *buf = calloc(1, LV2_ATOM_BUF_SIZE);
			if (!buf) {
				fprintf(stderr, "LV2: %s atom buf alloc failed\n", uri);
				free(defaults);
				for (int k = 0; k < st->n_atom_in; k++)  free(st->atom_in_bufs[k]);
				for (int k = 0; k < st->n_atom_out; k++) free(st->atom_out_bufs[k]);
				lilv_instance_free(st->instance);
				free(st->uri); free(st);
				return 0;
			}
			if (is_input) {
				LV2_Atom *atom = (LV2_Atom *)buf;
				atom->size = sizeof(LV2_Atom_Sequence_Body);
				atom->type = g_urid_atom_sequence;
				st->atom_in_idx[st->n_atom_in] = i;
				st->atom_in_bufs[st->n_atom_in++] = buf;
			} else {
				LV2_Atom *atom = (LV2_Atom *)buf;
				atom->size = LV2_ATOM_BUF_SIZE - sizeof(LV2_Atom);
				atom->type = g_urid_atom_chunk;
				st->atom_out_idx[st->n_atom_out] = i;
				st->atom_out_bufs[st->n_atom_out++] = buf;
			}
			lilv_instance_connect_port(st->instance, i, buf);
		} else if (is_ctrl && is_input && st->n_ctrl_in < LV2_MAX_CTRL_PORTS) {
			int idx = st->n_ctrl_in++;
			st->ctrl_in_idx[idx] = i;
			st->ctrl_values[idx]     = defaults[i];
			st->ctrl_target[idx]     = defaults[i];   /* V9.4.1 smoothing init */
			st->ctrl_in_min[idx]     = mins[i];
			st->ctrl_in_max[idx]     = maxs[i];
			st->ctrl_in_default[idx] = defaults[i];
			/* clé = symbol (unique, pour set_param + clé JSON) */
			LilvNode *sym = (LilvNode *)lilv_port_get_symbol(plug, port);
			strncpy(st->ctrl_in_name[idx], sym ? lilv_node_as_string(sym) : "?",
			        LV2_MAX_NAME_LEN - 1);
			/* label d'affichage = lilv_port_get_name (« Threshold ») */
			LilvNode *pname = lilv_port_get_name(plug, port);
			strncpy(st->ctrl_label[idx],
			        pname ? lilv_node_as_string(pname) : st->ctrl_in_name[idx],
			        LV2_MAX_NAME_LEN - 1);
			if (pname) lilv_node_free(pname);

			/* V9.5.21 — type du paramètre pour le bon widget GUI */
			uint8_t kind = 0;   /* continu */
			if (lilv_port_has_property(plug, port, g_uri_toggled))     kind = 1;
			else if (lilv_port_has_property(plug, port, g_uri_enumeration)) kind = 2;
			else if (lilv_port_has_property(plug, port, g_uri_integer)) kind = 3;
			if (lilv_port_has_property(plug, port, g_uri_logarithmic))  kind |= 0x10;
			st->ctrl_kind[idx] = kind;

			/* scale points (valeurs nommées) → "v=Label;v=Label;..." */
			st->ctrl_sp[idx] = NULL;
			LilvScalePoints *sps = lilv_port_get_scale_points(plug, port);
			if (sps) {
				char spbuf[512]; int sn = 0; spbuf[0] = '\0';
				LILV_FOREACH(scale_points, sit, sps) {
					const LilvScalePoint *sp = lilv_scale_points_get(sps, sit);
					const LilvNode *sv = lilv_scale_point_get_value(sp);
					const LilvNode *sl = lilv_scale_point_get_label(sp);
					if (!sv || !sl) continue;
					sn += snprintf(spbuf + sn, sizeof(spbuf) - sn, "%s%g=%s",
					               sn ? ";" : "",
					               lilv_node_as_float(sv), lilv_node_as_string(sl));
					if (sn >= (int)sizeof(spbuf) - 32) break;
				}
				if (sn > 0) st->ctrl_sp[idx] = strdup(spbuf);
				lilv_scale_points_free(sps);
			}

			/* unité (symbole : dB, Hz, ms...) */
			st->ctrl_unit[idx][0] = '\0';
			LilvNodes *us = lilv_port_get_value(plug, port, g_uri_units_unit);
			if (us && lilv_nodes_size(us) > 0) {
				const LilvNode *u = lilv_nodes_get_first(us);
				/* 1) symbole inline (units custom) */
				LilvNodes *sy = lilv_world_find_nodes(g_lv2_world, u, g_uri_units_symbol, NULL);
				if (sy && lilv_nodes_size(sy) > 0)
					strncpy(st->ctrl_unit[idx], lilv_node_as_string(lilv_nodes_get_first(sy)), 15);
				if (sy) lilv_nodes_free(sy);
				/* 2) sinon mappe les units LV2 standard par leur URI (le symbole
				 * est dans l'ontologie units, souvent non chargée). */
				if (st->ctrl_unit[idx][0] == '\0' && lilv_node_is_uri(u)) {
					const char *uu = lilv_node_as_uri(u);
					const char *h = strchr(uu, '#');
					const char *suf = h ? h + 1 : uu;
					static const struct { const char *k, *v; } M[] = {
						{"db","dB"},{"hz","Hz"},{"khz","kHz"},{"mhz","MHz"},
						{"s","s"},{"ms","ms"},{"min","min"},{"pc","%"},
						{"degree","°"},{"semitone12TET","st"},{"cent","ct"},
						{"bpm","bpm"},{"oct","oct"},{"hz","Hz"},{"bel","B"},
						{"m","m"},{"cm","cm"},{"mm","mm"},{"km","km"},
						{"coef","x"},{"frame","fr"},{NULL,NULL}};
					for (int mi = 0; M[mi].k; mi++)
						if (!strcmp(suf, M[mi].k)) {
							strncpy(st->ctrl_unit[idx], M[mi].v, 15); break;
						}
				}
			}
			if (us) lilv_nodes_free(us);

			/* groupe (section : Compressor, Band 1...) */
			st->ctrl_group[idx][0] = '\0';
			LilvNodes *gn = lilv_port_get_value(plug, port, g_uri_pg_group);
			if (gn && lilv_nodes_size(gn) > 0) {
				const LilvNode *g = lilv_nodes_get_first(gn);
				LilvNodes *gl = lilv_world_find_nodes(g_lv2_world, g, g_uri_rdfs_label, NULL);
				if (gl && lilv_nodes_size(gl) > 0)
					strncpy(st->ctrl_group[idx], lilv_node_as_string(lilv_nodes_get_first(gl)), LV2_MAX_NAME_LEN - 1);
				if (gl) lilv_nodes_free(gl);
			}
			if (gn) lilv_nodes_free(gn);

			lilv_instance_connect_port(st->instance, i,
				&st->ctrl_values[idx]);
		} else if (is_ctrl) {
			/* Control OUTPUT port — connect to dummy buffer */
			lilv_instance_connect_port(st->instance, i,
				&st->ctrl_out_dummy[i % LV2_MAX_CTRL_PORTS]);
		}
	}
	free(defaults);
	free(mins);
	free(maxs);

	/* Stéréo natif (2/2) : OK direct.
	 * Mono (1/1) : instancier une 2e fois pour le canal R, partageant
	 *              les contrôles. Plugin doit être stateless ou
	 *              indépendant par instance (typique : gain, biquad).
	 * Autre : refus.
	 */
	if (audio_in_n == 2 && audio_out_n == 2) {
		st->is_mono = 0;
	} else if (audio_in_n == 1 && audio_out_n == 1) {
		st->is_mono = 1;
		st->instance2 = lilv_plugin_instantiate(plug, (double)sample_rate, g_host_features);
		if (!st->instance2) {
			fprintf(stderr, "LV2: %s 2nd instance failed for mono→stereo\n", uri);
			lilv_instance_free(st->instance);
			free(st->uri); free(st);
			return 0;
		}
		/* Connect ports de l'instance 2 :
		 *  - audio input (1) → buf_in_r
		 *  - audio output (1) → buf_out_r
		 *  - control inputs → MÊMES buffers que instance1 (params partagés)
		 *  - control outputs → ctrl_out_dummy
		 */
		int ctrl_i = 0;
		int ain_i = 0, aout_i = 0;
		for (int i = 0; i < st->n_ports; i++) {
			const LilvPort *port = lilv_plugin_get_port_by_index(plug, i);
			int is_audio  = lilv_port_is_a(plug, port, g_uri_audio_port);
			int is_ctrl   = lilv_port_is_a(plug, port, g_uri_control_port);
			int is_atom   = lilv_port_is_a(plug, port, g_uri_atom_port);
			int is_input  = lilv_port_is_a(plug, port, g_uri_input_port);

			if (is_audio && is_input)
				lilv_instance_connect_port(st->instance2, i, &st->buf_in_r);
			else if (is_audio && !is_input)
				lilv_instance_connect_port(st->instance2, i, &st->buf_out_r);
			else if (is_atom) {
				/* V9.2-step5c : partage des buffers atom avec instance1.
				 * Limitation : pas d'automation indépendante par instance.
				 * Suffisant en mode passif (pas de MIDI/automation envoyés). */
				uint8_t *buf = NULL;
				if (is_input)
					buf = (ain_i < st->n_atom_in) ? st->atom_in_bufs[ain_i++] : NULL;
				else
					buf = (aout_i < st->n_atom_out) ? st->atom_out_bufs[aout_i++] : NULL;
				/* Si pas de buf (mismatch), connect NULL = laisser flotter
				 * → mais le plugin a déjà passé l'init donc tolère probablement.
				 * Cas non observé jusqu'ici. */
				if (buf) lilv_instance_connect_port(st->instance2, i, buf);
			}
			else if (is_ctrl && is_input)
				lilv_instance_connect_port(st->instance2, i, &st->ctrl_values[ctrl_i++]);
			else if (is_ctrl)
				lilv_instance_connect_port(st->instance2, i,
					&st->ctrl_out_dummy[i % LV2_MAX_CTRL_PORTS]);
		}
		lilv_instance_activate(st->instance2);
		fprintf(stderr, "LV2: %s mono→stereo (2 instances)\n", uri);
	} else if (audio_in_n == 1 && audio_out_n == 2) {
		/* V9.3.1.3 : mono in, stéréo out natif (room_builder_mono,
		 * certains reverbs/spatialisations). 1 seule instance, on mixe
		 * L+R en mono dans process_block. */
		st->is_mono_in_stereo_out = 1;
		fprintf(stderr, "LV2: %s mono-in/stereo-out (1 instance, L+R mixed)\n", uri);
	} else {
		fprintf(stderr, "LV2: %s I/O mismatch (in=%d out=%d, want 2/2, 1/1 ou 1/2)\n",
		        uri, audio_in_n, audio_out_n);
		lilv_instance_free(st->instance);
		free(st->uri); free(st);
		return 0;
	}

	lilv_instance_activate(st->instance);

	fx->type_name = "lv2";
	fx->state     = st;
	fx->process_block = lv2_process_block;
	fx->set_param = lv2_set_param;
	fx->reset     = lv2_reset;
	fx->get_state = lv2_get_state;
	return 1;
}

/* V9.5.21 — compte les ports audio in (hors sidechain) / out d'un plugin et
 * en déduit sa catégorie pour la GUI :
 *   "effet"      : audio in >= 1 ET audio out >= 1  (traitement)
 *   "instrument" : audio in == 0 ET audio out >= 1  (synthé/sampler MIDI→audio)
 *   "graphique"  : audio out == 0                    (meter/scope/analyseur,
 *                                                     visualisation uniquement)
 * "ins"=1 si utilisable en insert stéréo (effet mono 1/1 ou stéréo 2/2 ;
 * les versions multi-canaux x8/x32 ont ai/ao > 2 → ins=0, non supportées). */
static const char *lv2_categorize(const LilvPlugin *plug, int *out_ai, int *out_ao)
{
	int np = (int)lilv_plugin_get_num_ports(plug);
	int ai = 0, ao = 0;
	for (int i = 0; i < np; i++) {
		const LilvPort *port = lilv_plugin_get_port_by_index(plug, i);
		if (!lilv_port_is_a(plug, port, g_uri_audio_port))
			continue;
		int is_in = lilv_port_is_a(plug, port, g_uri_input_port);
		if (is_in) {
			/* exclure les sidechains (symbol "sc...") du compte principal */
			LilvNode *sym = (LilvNode *)lilv_port_get_symbol(plug, port);
			const char *s = sym ? lilv_node_as_string(sym) : NULL;
			if (!(s && strncmp(s, "sc", 2) == 0))
				ai++;
		} else {
			ao++;
		}
	}
	*out_ai = ai;
	*out_ao = ao;

	/* Classe LV2 prioritaire : les meters/analyseurs (Bit Meter, EBU, VU,
	 * Spectrum, Histogram, Oscilloscope…) passent l'audio (ai/ao>=1) mais
	 * leur rôle est la VISUALISATION → catégorie "graphique" même s'ils ont
	 * des ports audio. Détecté via lv2:AnalyserPlugin. */
	const LilvPluginClass *cls = lilv_plugin_get_class(plug);
	if (cls) {
		const LilvNode *curi = lilv_plugin_class_get_uri(cls);
		const char *cs = curi ? lilv_node_as_string(curi) : NULL;
		if (cs && strstr(cs, "AnalyserPlugin"))
			return "graphique";
	}

	if (ai == 0 && ao == 0) return "midi";        /* séquenceur/MIDI, pas d'audio */
	if (ai == 0)            return "instrument";  /* synthé/sampler MIDI→audio */
	if (ao == 0)            return "graphique";   /* meter sans passthrough */
	return "effet";
}

int fx_lv2_list_uris(char *buf, int len)
{
	if (!lv2_world_init()) return 0;
	int n = snprintf(buf, len, "[");
	int first = 1;
	LILV_FOREACH(plugins, it, g_lv2_plugins) {
		const LilvPlugin *plug = lilv_plugins_get(g_lv2_plugins, it);
		if (!lilv_plugin_has_feature(plug, g_uri_hard_rt))
			continue;   /* skip non-RT */
		const LilvNode *uri  = lilv_plugin_get_uri(plug);
		LilvNode *name       = lilv_plugin_get_name(plug);
		if (!uri) continue;
		if (n >= len - 256) break;
		int ai = 0, ao = 0;
		const char *cat = lv2_categorize(plug, &ai, &ao);
		int ins = (cat[0] == 'e' && ai >= 1 && ai <= 2 && ao >= 1 && ao <= 2);
		n += snprintf(buf + n, len - n,
		              "%s{\"uri\":\"%s\",\"name\":\"%s\",\"cat\":\"%s\","
		              "\"ai\":%d,\"ao\":%d,\"ins\":%d}",
		              first ? "" : ",",
		              lilv_node_as_string(uri),
		              name ? lilv_node_as_string(name) : "?",
		              cat, ai, ao, ins);
		lilv_node_free(name);
		first = 0;
	}
	if (n < len - 2) n += snprintf(buf + n, len - n, "]");
	return n;
}

