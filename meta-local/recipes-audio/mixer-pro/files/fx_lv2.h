// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fx_lv2 — interface PRIVÉE entre lv2_host.c (runtime), lv2_load.c
 * (instanciation + métadonnées) et fx_chain.c (fx_free). Pas une API
 * publique : les appelants passent par effects.h. (V14.0 étape 5.)
 */
#ifndef MIXER_FX_LV2_H
#define MIXER_FX_LV2_H

#include <pthread.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/atom.h>
#include <lv2/options/options.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/parameters/parameters.h>
#include <lv2/worker/worker.h>

#include "effects.h"

#define LV2_MAX_CTRL_PORTS 256
#define LV2_MAX_NAME_LEN   32

/* V9.2-step5d : LV2 worker support (1 thread non-RT per plugin instance).
 *
 * Architecture :
 *   audio_thread (RT prio 99) — appelle lilv_instance_run() qui peut
 *     appeler worker_schedule_cb() ; cette callback queue le request
 *     non-bloquant (pthread_mutex_trylock + counter drops si fail).
 *   worker thread (sched OTHER) — sleep sur cond, exécute iface->work()
 *     qui peut prendre 100ms+ (load IR file). Appelle worker_respond_cb()
 *     qui store la response dans un buffer per-instance.
 *   audio_thread (lv2_process) — au début, check si resp_pending,
 *     copy local + call iface->work_response() pour committer dans le plugin.
 *
 * Ring SPSC simple à 1 slot in / 1 slot out. Si plugin spam schedule_work
 * sans laisser le worker thread répondre → drops counter incrémenté.
 *
 * RT safety :
 *   - pthread_mutex_trylock dans audio_thread : non-bloquant (10-20 µs
 *     worst case sous contention PREEMPT_RT, négligeable / period 2 ms).
 *   - cond_wait avec timeout 2s côté worker thread pour détecter exit_flag
 *     (suggestion critic).
 *   - Si pthread_create fail → refus propre du plugin (suggestion critic).
 */
#define LV2_WORKER_BUF_SIZE  8192

struct lv2_worker {
	pthread_t          thread;
	pthread_mutex_t    mutex;
	pthread_cond_t     cond;

	/* SPSC 1-slot ring */
	uint8_t            req_buf[LV2_WORKER_BUF_SIZE];
	uint32_t           req_size;
	volatile int       req_pending;

	uint8_t            resp_buf[LV2_WORKER_BUF_SIZE];
	uint32_t           resp_size;
	volatile int       resp_pending;

	volatile int       exit_flag;
	uint64_t           drops;   /* schedule_work rejetées (critic suggestion) */

	const LV2_Worker_Interface *iface;
	LV2_Handle         plugin_handle;
	LV2_Worker_Schedule schedule;
};

/* Worker thread function : sleep sur cond, execute iface->work(), reboucle.
 * Timeout 2s sur cond_wait pour détecter exit_flag en cas de glitch (critic). */
void *lv2_worker_thread_fn(void *arg);

/* Callbacks (forward decl) */
LV2_Worker_Status lv2_worker_respond_cb(LV2_Worker_Respond_Handle handle,
                                               uint32_t size, const void *data);
LV2_Worker_Status lv2_worker_schedule_cb(LV2_Worker_Schedule_Handle handle,
                                                uint32_t size, const void *data);

/* V9.2-step5c : LV2 atom port buffers.
 * Buffer 8 KB par port = largement suffisant pour usage non-MIDI (state
 * notify, presets ack, peak meter feedback). Si plugin overflow, on logue
 * un warning + cap au capacity initial pour éviter corruption mémoire.
 * Mode mono→stereo : instance2 partage les mêmes buffers que instance1
 * (limitation : pas d'automation indépendante par instance, suffisant en
 * mode passif sans MIDI/automation host).
 */
#define LV2_MAX_ATOM_PORTS 8
#define LV2_ATOM_BUF_SIZE  8192

struct lv2_state {
	float          sr;
	char          *uri;
	LilvInstance  *instance;
	LilvInstance  *instance2;   /* V9.2 : 2e instance pour canal R en mode mono */
	int            is_mono;     /* 1 si plugin 1in/1out (2 instances pour L+R) */
	int            is_mono_in_stereo_out;  /* V9.3.1.3 : 1in/2out (ex: room_builder_mono) */
	const LilvPlugin *plugin;

	int            n_ports;
	int            audio_in_idx[2];   /* L, R, -1 si pas dispo */
	int            audio_out_idx[2];

	int            n_ctrl_in;
	int            ctrl_in_idx[LV2_MAX_CTRL_PORTS];
	char           ctrl_in_name[LV2_MAX_CTRL_PORTS][LV2_MAX_NAME_LEN];
	float          ctrl_values[LV2_MAX_CTRL_PORTS];   /* live values, connected */
	/* V9.3.3 : ranges critiques pour normalisation NPU (lilv_plugin_get_port_ranges_float).
	 * NaN si non spécifié dans le TTL (NPU doit alors deviner ou utiliser defaults). */
	float          ctrl_in_min[LV2_MAX_CTRL_PORTS];
	float          ctrl_in_max[LV2_MAX_CTRL_PORTS];
	float          ctrl_in_default[LV2_MAX_CTRL_PORTS];
	/* V9.4.1 : smoothing externe NPU.
	 * set_param écrit dans ctrl_target ; process_block interpole ctrl_values
	 * vers ctrl_target avec alpha = 1 - exp(-PERIOD/(tau*SR)).
	 * Pour tau=50ms, period=96, sr=48k → alpha ≈ 0.0392 → 95% en ~150ms.
	 * Évite clicks sur changes NPU rapides (peut sauter dB d'un coup). */
	float          ctrl_target[LV2_MAX_CTRL_PORTS];
	/* V9.5.21 — métadonnées d'affichage par param (UI riche dans la GUI) :
	 * kind 0=continu 1=toggle 2=enum 3=entier ; flag log (bit 0x10).
	 * scale points (enum "v=Label;...") en string optionnelle (NULL sinon).
	 * group/unit : labels LV2 si déclarés (vides sinon). */
	uint8_t        ctrl_kind[LV2_MAX_CTRL_PORTS];
	char          *ctrl_sp[LV2_MAX_CTRL_PORTS];
	char           ctrl_label[LV2_MAX_CTRL_PORTS][LV2_MAX_NAME_LEN];  /* rdfs:label (affichage) */
	char           ctrl_group[LV2_MAX_CTRL_PORTS][LV2_MAX_NAME_LEN];
	char           ctrl_unit[LV2_MAX_CTRL_PORTS][16];

	/* Buffers I/O 1-sample (legacy V9.2 sample-by-sample, plus utilisés en V9.3). */
	float          buf_in_l, buf_in_r, buf_out_l, buf_out_r;

	/* Dummy buffer pour control output (1 par port output, ignoré) */
	float          ctrl_out_dummy[LV2_MAX_CTRL_PORTS];

	/* V9.3.1.1 : buffers internes pour audio ports supplémentaires (>2).
	 * Plugins comme sc_compressor_lr ont 4-6 ports audio (in_l/r + sc_l/r + out_l/r).
	 * Si on ne connecte que 2, le plugin lit/écrit des pointers NULL → SEGV au 1er run.
	 * Solution : buffer silence pour extras inputs (sidechain self-key = silence),
	 *            buffer discard pour extras outputs.
	 * Note : taille hardcodée PERIOD_FRAMES = 96, doit matcher mixer-pro.h. */
	float          extra_in_silence[96];   /* lecture seule = 0 (zeros via calloc) */
	float          extra_out_discard[96];  /* écriture jetée */

	/* V9.3.1.3 : buffer mono = (in_l + in_r) / 2 pour mode 1in/2out
	 * (mono input alimentant un plugin qui synthétise stéréo natif, ex:
	 * room_builder_mono, certains reverbs). Calculé chaque process_block. */
	float          mono_in_buf[96];

	/* V9.2-step5c : atom port support (control input + notify output) */
	int            n_atom_in, n_atom_out;
	int            atom_in_idx[LV2_MAX_ATOM_PORTS];
	int            atom_out_idx[LV2_MAX_ATOM_PORTS];
	uint8_t       *atom_in_bufs[LV2_MAX_ATOM_PORTS];
	uint8_t       *atom_out_bufs[LV2_MAX_ATOM_PORTS];

	/* V9.2-step5d : worker support (NULL si plugin ne demande pas worker:schedule) */
	struct lv2_worker *worker;
};

/* monde lilv partagé + URIs pré-résolues (définis dans lv2_host.c) */
extern LilvWorld *g_lv2_world;
extern const LilvPlugins *g_lv2_plugins;
extern LilvNode *g_uri_audio_port, *g_uri_control_port, *g_uri_input_port,
	*g_uri_output_port, *g_uri_hard_rt, *g_uri_atom_port;
extern LV2_URID g_urid_atom_sequence, g_urid_atom_chunk;
extern LilvNode *g_uri_toggled, *g_uri_enumeration, *g_uri_integer,
	*g_uri_logarithmic, *g_uri_units_unit, *g_uri_units_symbol,
	*g_uri_pg_group, *g_uri_rdfs_label;
extern LV2_Feature g_feature_options, g_feature_urid_map;
extern const LV2_Feature *g_host_features[];

int lv2_world_init(void);
int lv2_host_supports_feature(const char *uri);
LV2_Worker_Status lv2_worker_schedule_cb(LV2_Worker_Schedule_Handle handle,
					 uint32_t size, const void *data);
LV2_Worker_Status lv2_worker_respond_cb(LV2_Worker_Respond_Handle handle,
					uint32_t size, const void *data);
void *lv2_worker_thread_fn(void *arg);
void lv2_process_block(fx_engine_t *fx, const float *in_l, const float *in_r,
		       float *out_l, float *out_r, uint32_t N);
int  lv2_set_param(fx_engine_t *fx, const char *name, float value);
void lv2_reset(fx_engine_t *fx);
int  lv2_get_state(fx_engine_t *fx, char *buf, int len);
void lv2_free_state(fx_engine_t *fx);

#endif /* MIXER_FX_LV2_H */
