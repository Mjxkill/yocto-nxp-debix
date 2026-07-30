// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lv2_host — runtime du host LV2 : monde lilv partagé, features
 * (urid:map, options), workers, process/set_param/reset/get_state,
 * cleanup. Code déplacé tel quel depuis effects.c (V14.0 étape 5).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effects.h"
#include "fx_internal.h"
#include "fx_lv2.h"

/* ========================================================================
 *   5. LV2 plugin host (V9.2) — lilv-0
 * ======================================================================
 *
 * Charge un plugin LV2 RT-safe via lilv, expose un fx_engine_t wrapper.
 *
 * Per-sample processing : lilv_instance_run(N=1) à chaque sample. Pas
 * optimal (overhead par call) mais cohérent avec l'architecture vtable
 * frame-per-frame du mixer. Plugins simples (gain, biquad) tolèrent.
 * Plugins avec buffers internes (reverb, delay lines) fonctionnent
 * aussi car ils gardent leur state interne entre les runs.
 *
 * Sécurité RT : on filtre `lv2:hardRTCapable=true` au load time.
 * Plugin sans cette propriété = refusé (peut allouer en process).
 */

/* Global lilv world (shared par tous les bus LV2). Init lazy. */
LilvWorld *g_lv2_world           = NULL;
const LilvPlugins *g_lv2_plugins = NULL;
LilvNode  *g_uri_audio_port      = NULL;
LilvNode  *g_uri_control_port    = NULL;
LilvNode  *g_uri_input_port      = NULL;
LilvNode  *g_uri_output_port     = NULL;
LilvNode  *g_uri_hard_rt         = NULL;
/* V9.2-step5c : LV2 atom port support (control/automation/notify) */
LilvNode  *g_uri_atom_port       = NULL;
LV2_URID   g_urid_atom_sequence  = 0;
LV2_URID   g_urid_atom_chunk     = 0;
/* V9.5.21 — métadonnées d'affichage des paramètres (UI riche) */
LilvNode  *g_uri_toggled         = NULL;
LilvNode  *g_uri_enumeration     = NULL;
LilvNode  *g_uri_integer         = NULL;
LilvNode  *g_uri_logarithmic     = NULL;
LilvNode  *g_uri_units_unit      = NULL;
LilvNode  *g_uri_units_symbol    = NULL;
LilvNode  *g_uri_pg_group        = NULL;
LilvNode  *g_uri_rdfs_label      = NULL;

/* V9.2-step5d : LV2 options host feature globals.
 * Permet de passer maxBlockLength, sampleRate, etc. au plugin à init.
 * Beaucoup de plugins modernes (dragonfly Hall, calf, lsp) requièrent
 * `opts:options` pour allouer leurs buffers internes.
 */
static int32_t  g_opt_max_block    = 96;       /* period frames (cohérent ALSA) */
static int32_t  g_opt_min_block    = 1;        /* on run sample-par-sample */
static int32_t  g_opt_nom_block    = 96;       /* nominal = max */
static int32_t  g_opt_seq_size     = 8192;     /* atom_sequence capacity */
static float    g_opt_sample_rate  = 48000.0f; /* SAMPLE_RATE projet */
static LV2_URID g_urid_max_block   = 0;
static LV2_URID g_urid_min_block   = 0;
static LV2_URID g_urid_nom_block   = 0;
static LV2_URID g_urid_seq_size    = 0;
static LV2_URID g_urid_sample_rate = 0;
static LV2_URID g_urid_atom_int    = 0;
static LV2_URID g_urid_atom_float  = 0;
static LV2_Options_Option g_lv2_options[7];   /* 6 entries + terminator zero */
LV2_Feature g_feature_options = {
	.URI  = LV2_OPTIONS__options,
	.data = g_lv2_options,
};

/* V9.2 — host feature `urid:map` : service basique de mapping URI → ID.
 * Beaucoup de plugins LV2 modernes (scope, params, etc.) le require sinon
 * instantiate fail. Implémentation simple linear search (suffisant pour
 * < 100 URIs typique). */
#define URID_MAP_MAX 256
static char       *g_urid_uris[URID_MAP_MAX];
static int         g_urid_count = 0;

static LV2_URID urid_map_fn(LV2_URID_Map_Handle handle, const char *uri)
{
	(void)handle;
	for (int i = 0; i < g_urid_count; i++) {
		if (strcmp(g_urid_uris[i], uri) == 0) return (LV2_URID)(i + 1);
	}
	if (g_urid_count >= URID_MAP_MAX) return 0;
	g_urid_uris[g_urid_count] = strdup(uri);
	return (LV2_URID)(++g_urid_count);
}

static LV2_URID_Map g_urid_map_data = {
	.handle = NULL,
	.map = urid_map_fn,
};
LV2_Feature g_feature_urid_map = {
	.URI  = LV2_URID__map,
	.data = &g_urid_map_data,
};
/* V9.2-step5d : g_host_features global = urid_map + options.
 * worker:schedule est INSTANCE-spécifique (handle = struct lv2_worker*),
 * donc construit per-plugin dans fx_init_lv2() à partir de ce array de base. */
const LV2_Feature *g_host_features[] = {
	&g_feature_urid_map,
	&g_feature_options,
	NULL
};

int lv2_world_init(void)
{
	if (g_lv2_world) return 1;
	g_lv2_world = lilv_world_new();
	if (!g_lv2_world) return 0;
	lilv_world_load_all(g_lv2_world);

	g_uri_audio_port    = lilv_new_uri(g_lv2_world, LV2_CORE__AudioPort);
	g_uri_control_port  = lilv_new_uri(g_lv2_world, LV2_CORE__ControlPort);
	g_uri_input_port    = lilv_new_uri(g_lv2_world, LV2_CORE__InputPort);
	g_uri_output_port   = lilv_new_uri(g_lv2_world, LV2_CORE__OutputPort);
	g_uri_hard_rt       = lilv_new_uri(g_lv2_world, LV2_CORE__hardRTCapable);
	g_uri_toggled       = lilv_new_uri(g_lv2_world, LV2_CORE__toggled);
	g_uri_enumeration   = lilv_new_uri(g_lv2_world, LV2_CORE__enumeration);
	g_uri_integer       = lilv_new_uri(g_lv2_world, LV2_CORE__integer);
	g_uri_logarithmic   = lilv_new_uri(g_lv2_world, "http://lv2plug.in/ns/ext/port-props#logarithmic");
	g_uri_units_unit    = lilv_new_uri(g_lv2_world, "http://lv2plug.in/ns/extensions/units#unit");
	g_uri_units_symbol  = lilv_new_uri(g_lv2_world, "http://lv2plug.in/ns/extensions/units#symbol");
	g_uri_pg_group      = lilv_new_uri(g_lv2_world, "http://lv2plug.in/ns/ext/port-groups#group");
	g_uri_rdfs_label    = lilv_new_uri(g_lv2_world, "http://www.w3.org/2000/01/rdf-schema#label");
	g_uri_atom_port     = lilv_new_uri(g_lv2_world, LV2_ATOM__AtomPort);

	/* Pre-map atom URIDs (utilisés à chaque cycle audio dans lv2_process) */
	g_urid_atom_sequence = urid_map_fn(NULL, LV2_ATOM__Sequence);
	g_urid_atom_chunk    = urid_map_fn(NULL, LV2_ATOM__Chunk);

	/* V9.2-step5d : pre-map options URIDs + init g_lv2_options[] array */
	g_urid_max_block    = urid_map_fn(NULL, LV2_BUF_SIZE__maxBlockLength);
	g_urid_min_block    = urid_map_fn(NULL, LV2_BUF_SIZE__minBlockLength);
	g_urid_nom_block    = urid_map_fn(NULL, LV2_BUF_SIZE__nominalBlockLength);
	g_urid_seq_size     = urid_map_fn(NULL, LV2_BUF_SIZE__sequenceSize);
	g_urid_sample_rate  = urid_map_fn(NULL, LV2_PARAMETERS__sampleRate);
	g_urid_atom_int     = urid_map_fn(NULL, LV2_ATOM__Int);
	g_urid_atom_float   = urid_map_fn(NULL, LV2_ATOM__Float);

	g_lv2_options[0] = (LV2_Options_Option){
		LV2_OPTIONS_INSTANCE, 0, g_urid_max_block,
		sizeof(int32_t), g_urid_atom_int, &g_opt_max_block };
	g_lv2_options[1] = (LV2_Options_Option){
		LV2_OPTIONS_INSTANCE, 0, g_urid_min_block,
		sizeof(int32_t), g_urid_atom_int, &g_opt_min_block };
	g_lv2_options[2] = (LV2_Options_Option){
		LV2_OPTIONS_INSTANCE, 0, g_urid_nom_block,
		sizeof(int32_t), g_urid_atom_int, &g_opt_nom_block };
	g_lv2_options[3] = (LV2_Options_Option){
		LV2_OPTIONS_INSTANCE, 0, g_urid_seq_size,
		sizeof(int32_t), g_urid_atom_int, &g_opt_seq_size };
	g_lv2_options[4] = (LV2_Options_Option){
		LV2_OPTIONS_INSTANCE, 0, g_urid_sample_rate,
		sizeof(float), g_urid_atom_float, &g_opt_sample_rate };
	g_lv2_options[5] = (LV2_Options_Option){ 0, 0, 0, 0, 0, NULL };  /* terminator */
	g_lv2_options[6] = (LV2_Options_Option){ 0, 0, 0, 0, 0, NULL };  /* safety */

	g_lv2_plugins = lilv_world_get_all_plugins(g_lv2_world);
	return 1;
}

/* V9.2-step5c : liste des URIs de features que l'host implémente. Utilisé
 * pour valider les required_features du plugin AVANT instantiate. Refus
 * propre si plugin demande worker/state/options/etc. non supportés.
 * On supporte aujourd'hui : urid:map (cf g_host_features ci-dessus).
 * hardRTCapable est dans CORE et n'est pas une feature, c'est un trait. */
int lv2_host_supports_feature(const char *uri)
{
	if (!uri) return 0;
	if (strcmp(uri, LV2_URID__map) == 0) return 1;
	if (strcmp(uri, LV2_OPTIONS__options) == 0) return 1;
	/* V9.2-step5d : worker:schedule supporté via thread per-plugin (cf
	 * struct lv2_worker dans fx_init_lv2). Le feature data est instance-
	 * spécifique, pas global. */
	if (strcmp(uri, LV2_WORKER__schedule) == 0) return 1;
	/* lv2:state (presets, save/restore) : pas implémenté V9.2, plugins
	 * qui le require seront refusés. À implémenter V9.3 si besoin. */
	return 0;
}

/* V9.5.12 : 64 → 256 pour LSP Para EQ x16 stereo (~158 ports : globals +
 * 9 params × 16 bands). Cap à 64 = seules les ~5 premières bandes visibles. */


/* V9.2-step5d : worker callbacks + thread.
 * - schedule_cb : appelée par plugin depuis run() audio_thread → queue request
 * - respond_cb  : appelée par plugin depuis work() worker_thread → buffer response
 * - thread_fn   : worker thread loop (sleep/work/respond)
 */
LV2_Worker_Status lv2_worker_schedule_cb(LV2_Worker_Schedule_Handle handle,
                                                uint32_t size, const void *data)
{
	struct lv2_worker *w = (struct lv2_worker *)handle;
	if (!w || !data || size == 0 || size > LV2_WORKER_BUF_SIZE) {
		if (w) w->drops++;
		return LV2_WORKER_ERR_NO_SPACE;
	}
	/* Non-blocking trylock pour rester RT-safe sur audio_thread. */
	if (pthread_mutex_trylock(&w->mutex) != 0) {
		w->drops++;
		return LV2_WORKER_ERR_UNKNOWN;
	}
	if (w->req_pending) {
		/* Worker thread n'a pas encore consommé le request précédent.
		 * Plugin doit retry au prochain run(). */
		w->drops++;
		pthread_mutex_unlock(&w->mutex);
		return LV2_WORKER_ERR_UNKNOWN;
	}
	memcpy(w->req_buf, data, size);
	w->req_size = size;
	__sync_synchronize();
	w->req_pending = 1;
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->mutex);
	return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status lv2_worker_respond_cb(LV2_Worker_Respond_Handle handle,
                                               uint32_t size, const void *data)
{
	struct lv2_worker *w = (struct lv2_worker *)handle;
	if (!w || !data || size == 0 || size > LV2_WORKER_BUF_SIZE)
		return LV2_WORKER_ERR_NO_SPACE;
	/* Worker thread est le seul writer, audio thread le seul reader.
	 * resp_pending = 0 sur entrée garanti par audio thread après consume. */
	memcpy(w->resp_buf, data, size);
	w->resp_size = size;
	__sync_synchronize();
	w->resp_pending = 1;
	return LV2_WORKER_SUCCESS;
}

void *lv2_worker_thread_fn(void *arg)
{
	struct lv2_worker *w = (struct lv2_worker *)arg;

	while (!w->exit_flag) {
		pthread_mutex_lock(&w->mutex);
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 2;  /* 2s timeout pour relire exit_flag (deadlock guard) */
		while (!w->req_pending && !w->exit_flag) {
			int rc = pthread_cond_timedwait(&w->cond, &w->mutex, &ts);
			if (rc == ETIMEDOUT) break;
		}
		if (w->exit_flag) {
			pthread_mutex_unlock(&w->mutex);
			break;
		}
		if (!w->req_pending) {
			pthread_mutex_unlock(&w->mutex);
			continue;
		}
		/* Copy request hors mutex avant d'appeler work() (qui peut être long) */
		uint8_t local[LV2_WORKER_BUF_SIZE];
		uint32_t sz = w->req_size;
		memcpy(local, w->req_buf, sz);
		w->req_pending = 0;
		pthread_mutex_unlock(&w->mutex);

		if (w->iface && w->iface->work)
			w->iface->work(w->plugin_handle, lv2_worker_respond_cb, w, sz, local);
	}
	return NULL;
}

/* V9.3 : process_block — le GROS GAIN du refactor.
 * AVANT V9.3 : lilv_instance_run(N=1) appelée N fois par cycle audio.
 *   → 96 calls × 4 bus = 384 calls par cycle, overhead jump table +
 *     state restore × 384. Pour LSP Para EQ 16-band = ~30 ms par cycle.
 * APRÈS V9.3 : 1 call lilv_instance_run(N=96) par bus → 4 calls par cycle.
 *   Le plugin process son block en interne (avec ses optims internes
 *   block-loop, NEON, SIMD si présentes dans le code source LSP/calf).
 *   → Gain attendu 30-60× sur plugins lourds.
 *
 * Worker response commit + atom reset : 1 fois par block (vs 96 fois). */
/* V9.4.1 / V9.5.5 / V9.5.12 : smoothing alpha pour ctrl params LV2.
 * V9.4.1  = tau 50 ms (alpha 0.039, 95% en ~150 ms) — update NPU 5-10 Hz.
 * V9.5.5  = tau 10 ms (alpha 0.18,  95% en ~30 ms)  — update NPU 50 Hz fast.
 * V9.5.12 = tau 50 ms (alpha 0.039, 95% en ~150 ms) — update NPU 100 Hz
 *           SLOW SMOOTH : updates fréquents mais smoothing lent absorbe le
 *           vibrato des paramètres dû aux transients (trompette etc.).
 * Formule : alpha = 1 - expf(-PERIOD_FRAMES / (tau * SAMPLE_RATE))
 *         = 1 - expf(-96 / (0.050 * 48000)) = 1 - expf(-0.04) ≈ 0.0392. */
#define LV2_CTRL_SMOOTH_ALPHA  0.0392f

void lv2_process_block(fx_engine_t *fx,
			      const float *in_l, const float *in_r,
			      float *out_l, float *out_r,
			      uint32_t N)
{
	struct lv2_state *st = fx->state;

	/* V9.4.1 : smoothing 1 step par block (== 1 step toutes 2 ms).
	 * Interpole ctrl_values vers ctrl_target. Cas N=0 (target==value) ne
	 * coûte qu'une comparaison float trivialement vectorisable par gcc. */
	for (int i = 0; i < st->n_ctrl_in; i++) {
		st->ctrl_values[i] += (st->ctrl_target[i] - st->ctrl_values[i])
		                      * LV2_CTRL_SMOOTH_ALPHA;
	}

	/* Worker response commit avant run (LV2 spec) */
	if (st->worker && st->worker->resp_pending) {
		struct lv2_worker *w = st->worker;
		uint8_t local[LV2_WORKER_BUF_SIZE];
		uint32_t sz = w->resp_size;
		memcpy(local, w->resp_buf, sz);
		__sync_synchronize();
		w->resp_pending = 0;
		if (w->iface && w->iface->work_response)
			w->iface->work_response(w->plugin_handle, sz, local);
	}

	/* Reconnecte audio ports aux buffers externes (block).
	 * V9.3 : reconnect par cycle = function ptr set, négligeable vs gain N=96. */
	if (st->is_mono_in_stereo_out) {
		/* V9.3.1.3 : mono in, stéréo out natif.
		 * Mix L+R → mono_in_buf, connect input mono + outputs L/R. */
		const float *l = in_l, *r = in_r;
		for (uint32_t i = 0; i < N; i++)
			st->mono_in_buf[i] = (l[i] + r[i]) * 0.5f;
		if (st->audio_in_idx[0] >= 0)
			lilv_instance_connect_port(st->instance, st->audio_in_idx[0], st->mono_in_buf);
		if (st->audio_out_idx[0] >= 0)
			lilv_instance_connect_port(st->instance, st->audio_out_idx[0], out_l);
		if (st->audio_out_idx[1] >= 0)
			lilv_instance_connect_port(st->instance, st->audio_out_idx[1], out_r);
	} else if (!st->is_mono) {
		/* Stéréo natif 2/2 */
		if (st->audio_in_idx[0] >= 0)
			lilv_instance_connect_port(st->instance, st->audio_in_idx[0], (void *)in_l);
		if (st->audio_in_idx[1] >= 0)
			lilv_instance_connect_port(st->instance, st->audio_in_idx[1], (void *)in_r);
		if (st->audio_out_idx[0] >= 0)
			lilv_instance_connect_port(st->instance, st->audio_out_idx[0], out_l);
		if (st->audio_out_idx[1] >= 0)
			lilv_instance_connect_port(st->instance, st->audio_out_idx[1], out_r);
	} else {
		/* Mono 1/1 dupliqué : instance1 = L, instance2 = R */
		if (st->audio_in_idx[0] >= 0) {
			lilv_instance_connect_port(st->instance,  st->audio_in_idx[0], (void *)in_l);
			lilv_instance_connect_port(st->instance2, st->audio_in_idx[0], (void *)in_r);
		}
		if (st->audio_out_idx[0] >= 0) {
			lilv_instance_connect_port(st->instance,  st->audio_out_idx[0], out_l);
			lilv_instance_connect_port(st->instance2, st->audio_out_idx[0], out_r);
		}
	}

	/* Reset atom ports — 1 fois par block (vs N fois). */
	for (int k = 0; k < st->n_atom_in; k++) {
		LV2_Atom *atom = (LV2_Atom *)st->atom_in_bufs[k];
		atom->size = sizeof(LV2_Atom_Sequence_Body);
		atom->type = g_urid_atom_sequence;
	}
	for (int k = 0; k < st->n_atom_out; k++) {
		LV2_Atom *atom = (LV2_Atom *)st->atom_out_bufs[k];
		atom->size = LV2_ATOM_BUF_SIZE - sizeof(LV2_Atom);
		atom->type = g_urid_atom_chunk;
	}

	/* RUN N samples en 1 call (vs N × N=1). */
	lilv_instance_run(st->instance, N);
	if (st->is_mono && st->instance2)
		lilv_instance_run(st->instance2, N);
}

int lv2_set_param(fx_engine_t *fx, const char *name, float value)
{
	struct lv2_state *st = fx->state;
	for (int i = 0; i < st->n_ctrl_in; i++) {
		if (strcmp(st->ctrl_in_name[i], name) == 0) {
			/* V9.4.1 : écrit target, audio_thread interpole ctrl_values
			 * vers target via smoothing. Évite click NPU rapide. */
			st->ctrl_target[i] = value;
			return 0;
		}
	}
	return -1;
}

void lv2_reset(fx_engine_t *fx)
{
	struct lv2_state *st = fx->state;
	st->buf_in_l = st->buf_in_r = st->buf_out_l = st->buf_out_r = 0;
	if (st->instance) {
		lilv_instance_deactivate(st->instance);
		lilv_instance_activate(st->instance);
	}
	if (st->instance2) {
		lilv_instance_deactivate(st->instance2);
		lilv_instance_activate(st->instance2);
	}
}

/* V9.5.21b — copie s dans buf en échappant " \\ et les contrôles (JSON-safe).
 * Les labels/scale points lilv sont du contenu tiers : sans échappement, un
 * label contenant un guillemet casserait tout le get_fx (revue point 2). */
static int json_escape(char *buf, int len, const char *s)
{
	int n = 0;
	for (; s && *s && n < len - 7; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			buf[n++] = '\\'; buf[n++] = (char)c;
		} else if (c < 0x20) {
			n += snprintf(buf + n, len - n, "\\u%04x", c);
		} else {
			buf[n++] = (char)c;
		}
	}
	buf[n] = '\0';
	return n;
}

/* Format flottant JSON-safe : NaN/Inf → null (sinon JSON.parse rejette). */
static int json_float(char *buf, int len, float v)
{
	if (isnan(v) || isinf(v))
		return snprintf(buf, len, "null");
	return snprintf(buf, len, "%.4f", v);
}

int lv2_get_state(fx_engine_t *fx, char *buf, int len)
{
	struct lv2_state *st = fx->state;
	int n = snprintf(buf, len,
		"\"type\":\"lv2\",\"uri\":\"%s\",\"params\":{",
		st->uri ? st->uri : "");
	for (int i = 0; i < st->n_ctrl_in && n < len - 32; i++) {
		n += snprintf(buf + n, len - n, "%s\"%s\":%.4f",
		              i == 0 ? "" : ",",
		              st->ctrl_in_name[i],
		              st->ctrl_values[i]);
	}
	/* V9.3.3 : ranges pour normalisation NPU.
	 * Format : {"freq1":{"min":20,"max":20000,"def":1000}, ...}
	 * NaN/Inf → null (port unbounded ou non spécifié dans TTL). */
	if (n < len - 16)
		n += snprintf(buf + n, len - n, "},\"ranges\":{");
	for (int i = 0; i < st->n_ctrl_in && n < len - 80; i++) {
		n += snprintf(buf + n, len - n, "%s\"%s\":{\"min\":",
		              i == 0 ? "" : ",", st->ctrl_in_name[i]);
		n += json_float(buf + n, len - n, st->ctrl_in_min[i]);
		n += snprintf(buf + n, len - n, ",\"max\":");
		n += json_float(buf + n, len - n, st->ctrl_in_max[i]);
		n += snprintf(buf + n, len - n, ",\"def\":");
		n += json_float(buf + n, len - n, st->ctrl_in_default[i]);
		n += snprintf(buf + n, len - n, "}");
	}
	/* V9.5.21 — meta d'affichage : label, kind (0 cont/1 toggle/2 enum/3 int,
	 * +0x10 log), unit, group, scale points. Pour UI riche groupée. */
	if (n < len - 16)
		n += snprintf(buf + n, len - n, "},\"meta\":{");
	for (int i = 0; i < st->n_ctrl_in && n < len - 320; i++) {
		char esc[600];
		n += snprintf(buf + n, len - n, "%s\"%s\":{\"label\":\"",
		              i == 0 ? "" : ",", st->ctrl_in_name[i]);
		json_escape(esc, sizeof(esc), st->ctrl_label[i]);
		n += snprintf(buf + n, len - n, "%s\",\"kind\":%d,\"unit\":\"",
		              esc, st->ctrl_kind[i]);
		json_escape(esc, sizeof(esc), st->ctrl_unit[i]);
		n += snprintf(buf + n, len - n, "%s\",\"grp\":\"", esc);
		json_escape(esc, sizeof(esc), st->ctrl_group[i]);
		n += snprintf(buf + n, len - n, "%s\"", esc);
		if (st->ctrl_sp[i] && n < len - 700) {
			json_escape(esc, sizeof(esc), st->ctrl_sp[i]);
			n += snprintf(buf + n, len - n, ",\"sp\":\"%s\"", esc);
		}
		n += snprintf(buf + n, len - n, "}");
	}
	if (n < len - 4) n += snprintf(buf + n, len - n, "}");
	return n;
}


/* Cleanup INSTANCE lv2 (appelé par fx_free, fx_chain.c) — corps déplacé
 * tel quel de la branche type_name=="lv2" de fx_free (V14.0 étape 5). */
void lv2_free_state(fx_engine_t *fx)
{
	/* LV2 engine : cleanup lilv instance d'abord (différent du calloc).
	 * Détection par type_name (pas idéal mais évite refactor vtable). */
	if (fx->type_name && strcmp(fx->type_name, "lv2") == 0) {
	struct lv2_state *st = fx->state;
	/* V9.2-step5d : stop worker thread AVANT free instance.
	 * Set exit_flag + signal cond + join. Plugin work() ne sera plus
	 * appelée après ; instance peut être deactivate/free. */
	if (st->worker) {
		pthread_mutex_lock(&st->worker->mutex);
		st->worker->exit_flag = 1;
		pthread_cond_signal(&st->worker->cond);
		pthread_mutex_unlock(&st->worker->mutex);
		pthread_join(st->worker->thread, NULL);
		if (st->worker->drops)
		fprintf(stderr, "LV2: worker drops=%llu\n",
		        (unsigned long long)st->worker->drops);
		pthread_mutex_destroy(&st->worker->mutex);
		pthread_cond_destroy(&st->worker->cond);
		free(st->worker);
	}
	if (st->instance) {
		lilv_instance_deactivate(st->instance);
		lilv_instance_free(st->instance);
	}
	if (st->instance2) {
		lilv_instance_deactivate(st->instance2);
		lilv_instance_free(st->instance2);
	}
	/* V9.2-step5c : libère les buffers atom alloués en fx_init_lv2 */
	for (int k = 0; k < st->n_atom_in; k++)  free(st->atom_in_bufs[k]);
	for (int k = 0; k < st->n_atom_out; k++) free(st->atom_out_bufs[k]);
	/* V9.5.21 : libère les scale points strdup'd */
	for (int k = 0; k < st->n_ctrl_in; k++)  free(st->ctrl_sp[k]);
	free(st->uri);
	}
}
