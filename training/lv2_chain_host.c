/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V9.5 — Mini host LV2 pour training PC (offline audio process).
 *
 * Charge une chaîne de N plugins LV2, expose set_param + process, libère.
 * Compilé en .so chargeable depuis Python via ctypes.
 *
 * Compile :
 *   gcc -O2 -fPIC -shared -o lv2_chain_host.so lv2_chain_host.c \
 *       $(pkg-config --cflags --libs lilv-0)
 *
 * Reproduit l'archi `effects.c::lv2_process_block` + `chain_process_block`
 * du mixer-pro board, mais sans worker thread (offline) et sans smoothing
 * (le NPU prédit déjà des valeurs lissées).
 *
 * Limitations volontaires V9.5.1 :
 *   - Pas de support worker:schedule (offline → pas critique)
 *   - Pas d'atom ports (autres que connect NULL = laisse défaut)
 *   - Pas de smoothing externe (write direct = équivalent à NPU final)
 *   - Audio ports > 2 : connect au silence (comme effects.c)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <lilv/lilv.h>

#define MAX_CHAIN     8
#define MAX_PORTS   256
#define MAX_AUDIO     2

typedef struct {
	LilvInstance *inst;
	int           n_ports;
	int           audio_in_idx[MAX_AUDIO];
	int           audio_out_idx[MAX_AUDIO];
	int           ctrl_in_n;
	int           ctrl_in_port_idx[MAX_PORTS];
	char          ctrl_in_name[MAX_PORTS][64];
	float         ctrl_values[MAX_PORTS];
	float         ctrl_min[MAX_PORTS];
	float         ctrl_max[MAX_PORTS];
	float         ctrl_def[MAX_PORTS];
	float         silence_in[8192];   /* large = couvre toutes period sizes */
	float         discard_out[8192];
} lv2_slot_t;

typedef struct {
	LilvWorld          *world;
	const LilvPlugins  *plugins;
	int                 n_slots;
	lv2_slot_t          slots[MAX_CHAIN];
	float               sr;
	uint32_t            block_size;
	float               tmp_a_l[8192];
	float               tmp_a_r[8192];
	float               tmp_b_l[8192];
	float               tmp_b_r[8192];
} chain_t;

/* URI nodes cache */
static LilvNode *u_audio, *u_input, *u_output, *u_ctrl, *u_atom;

/* ---------------- Public API (called from Python ctypes) ---------------- */

chain_t *chain_create(float sample_rate, uint32_t block_size)
{
	chain_t *c = calloc(1, sizeof(*c));
	if (!c) return NULL;
	c->world = lilv_world_new();
	lilv_world_load_all(c->world);
	c->plugins = lilv_world_get_all_plugins(c->world);
	c->sr = sample_rate;
	c->block_size = block_size > 8192 ? 8192 : block_size;

	u_audio  = lilv_new_uri(c->world, "http://lv2plug.in/ns/lv2core#AudioPort");
	u_input  = lilv_new_uri(c->world, "http://lv2plug.in/ns/lv2core#InputPort");
	u_output = lilv_new_uri(c->world, "http://lv2plug.in/ns/lv2core#OutputPort");
	u_ctrl   = lilv_new_uri(c->world, "http://lv2plug.in/ns/lv2core#ControlPort");
	u_atom   = lilv_new_uri(c->world, "http://lv2plug.in/ns/ext/atom#AtomPort");
	return c;
}

/* Add a plugin to the chain by URI. Returns slot index on success, -1 on fail. */
int chain_add_plugin(chain_t *c, const char *uri)
{
	if (!c || c->n_slots >= MAX_CHAIN) return -1;
	LilvNode *plug_uri = lilv_new_uri(c->world, uri);
	const LilvPlugin *plug = lilv_plugins_get_by_uri(c->plugins, plug_uri);
	lilv_node_free(plug_uri);
	if (!plug) return -1;

	int slot = c->n_slots;
	lv2_slot_t *s = &c->slots[slot];
	s->inst = lilv_plugin_instantiate(plug, (double)c->sr, NULL);
	if (!s->inst) return -1;

	s->n_ports = (int)lilv_plugin_get_num_ports(plug);
	if (s->n_ports > MAX_PORTS) { lilv_instance_free(s->inst); return -1; }
	float *mins  = calloc(s->n_ports, sizeof(float));
	float *maxs  = calloc(s->n_ports, sizeof(float));
	float *defs  = calloc(s->n_ports, sizeof(float));
	lilv_plugin_get_port_ranges_float(plug, mins, maxs, defs);

	int audio_in_n = 0, audio_out_n = 0;
	for (int i = 0; i < s->n_ports; i++) {
		const LilvPort *port = lilv_plugin_get_port_by_index(plug, i);
		int is_audio = lilv_port_is_a(plug, port, u_audio);
		int is_ctrl  = lilv_port_is_a(plug, port, u_ctrl);
		int is_in    = lilv_port_is_a(plug, port, u_input);

		if (is_audio && is_in && audio_in_n < MAX_AUDIO) {
			s->audio_in_idx[audio_in_n++] = i;
		} else if (is_audio && is_in) {
			/* Sidechain / extras → silence */
			lilv_instance_connect_port(s->inst, i, s->silence_in);
		} else if (is_audio && !is_in && audio_out_n < MAX_AUDIO) {
			s->audio_out_idx[audio_out_n++] = i;
		} else if (is_audio && !is_in) {
			lilv_instance_connect_port(s->inst, i, s->discard_out);
		} else if (is_ctrl && is_in && s->ctrl_in_n < MAX_PORTS) {
			int idx = s->ctrl_in_n++;
			s->ctrl_in_port_idx[idx] = i;
			s->ctrl_values[idx] = defs[i];
			s->ctrl_min[idx] = mins[i];
			s->ctrl_max[idx] = maxs[i];
			s->ctrl_def[idx] = defs[i];
			LilvNode *sym = (LilvNode *)lilv_port_get_symbol(plug, port);
			const char *sym_s = sym ? lilv_node_as_string(sym) : "?";
			strncpy(s->ctrl_in_name[idx], sym_s, sizeof(s->ctrl_in_name[0]) - 1);
			lilv_instance_connect_port(s->inst, i, &s->ctrl_values[idx]);
		} else if (is_ctrl) {
			/* Control output dummy : connect au discard scalar */
			lilv_instance_connect_port(s->inst, i, &s->discard_out[0]);
		}
		/* Atom ports : laisser NULL (offline, pas critique) */
	}
	free(mins); free(maxs); free(defs);

	lilv_instance_activate(s->inst);
	c->n_slots++;
	return slot;
}

/* Get param count + names for a slot. Python iterates 0..n-1 to inspect. */
int chain_slot_n_params(chain_t *c, int slot)
{
	if (!c || slot < 0 || slot >= c->n_slots) return -1;
	return c->slots[slot].ctrl_in_n;
}

/* Returns the symbol name of a param (writes into out). Returns 0 OK. */
int chain_slot_param_name(chain_t *c, int slot, int p, char *out, int max)
{
	if (!c || slot < 0 || slot >= c->n_slots) return -1;
	if (p < 0 || p >= c->slots[slot].ctrl_in_n) return -1;
	strncpy(out, c->slots[slot].ctrl_in_name[p], max - 1);
	out[max - 1] = 0;
	return 0;
}

/* Returns the (min, max, default) of a param. Writes into out (3 floats). */
int chain_slot_param_range(chain_t *c, int slot, int p, float *out_minmaxdef)
{
	if (!c || slot < 0 || slot >= c->n_slots) return -1;
	if (p < 0 || p >= c->slots[slot].ctrl_in_n) return -1;
	out_minmaxdef[0] = c->slots[slot].ctrl_min[p];
	out_minmaxdef[1] = c->slots[slot].ctrl_max[p];
	out_minmaxdef[2] = c->slots[slot].ctrl_def[p];
	return 0;
}

/* Set a param by index. Direct write to ctrl_values (no smoothing). */
int chain_set_param(chain_t *c, int slot, int p, float value)
{
	if (!c || slot < 0 || slot >= c->n_slots) return -1;
	if (p < 0 || p >= c->slots[slot].ctrl_in_n) return -1;
	c->slots[slot].ctrl_values[p] = value;
	return 0;
}

/* Set a param by NAME (matches symbol). Slow lookup, mostly for debug. */
int chain_set_param_by_name(chain_t *c, int slot, const char *name, float value)
{
	if (!c || slot < 0 || slot >= c->n_slots) return -1;
	lv2_slot_t *s = &c->slots[slot];
	for (int i = 0; i < s->ctrl_in_n; i++) {
		if (strcmp(s->ctrl_in_name[i], name) == 0) {
			s->ctrl_values[i] = value;
			return 0;
		}
	}
	return -1;
}

/* Process N frames (typically chain->block_size) through all slots.
 * in_l/in_r and out_l/out_r are N float arrays. In-place OK. */
int chain_process(chain_t *c,
                  const float *in_l, const float *in_r,
                  float *out_l, float *out_r,
                  uint32_t N)
{
	if (!c || c->n_slots == 0) {
		/* Bypass */
		if (in_l != out_l) memcpy(out_l, in_l, N * sizeof(float));
		if (in_r != out_r) memcpy(out_r, in_r, N * sizeof(float));
		return 0;
	}
	if (N > c->block_size) return -1;

	/* Connect slot 0 inputs to (in_l, in_r) */
	lv2_slot_t *s0 = &c->slots[0];
	float *dst_l, *dst_r;
	if (c->n_slots == 1) {
		dst_l = out_l; dst_r = out_r;
	} else {
		dst_l = c->tmp_a_l; dst_r = c->tmp_a_r;
	}
	if (s0->audio_in_idx[0] >= 0)
		lilv_instance_connect_port(s0->inst, s0->audio_in_idx[0], (void *)in_l);
	if (s0->audio_in_idx[1] >= 0)
		lilv_instance_connect_port(s0->inst, s0->audio_in_idx[1], (void *)in_r);
	if (s0->audio_out_idx[0] >= 0)
		lilv_instance_connect_port(s0->inst, s0->audio_out_idx[0], dst_l);
	if (s0->audio_out_idx[1] >= 0)
		lilv_instance_connect_port(s0->inst, s0->audio_out_idx[1], dst_r);
	lilv_instance_run(s0->inst, N);

	int parity = 0;
	for (int i = 1; i < c->n_slots - 1; i++) {
		lv2_slot_t *si = &c->slots[i];
		float *src_l = parity ? c->tmp_b_l : c->tmp_a_l;
		float *src_r = parity ? c->tmp_b_r : c->tmp_a_r;
		float *dl    = parity ? c->tmp_a_l : c->tmp_b_l;
		float *dr    = parity ? c->tmp_a_r : c->tmp_b_r;
		if (si->audio_in_idx[0] >= 0)  lilv_instance_connect_port(si->inst, si->audio_in_idx[0], src_l);
		if (si->audio_in_idx[1] >= 0)  lilv_instance_connect_port(si->inst, si->audio_in_idx[1], src_r);
		if (si->audio_out_idx[0] >= 0) lilv_instance_connect_port(si->inst, si->audio_out_idx[0], dl);
		if (si->audio_out_idx[1] >= 0) lilv_instance_connect_port(si->inst, si->audio_out_idx[1], dr);
		lilv_instance_run(si->inst, N);
		parity = !parity;
	}

	if (c->n_slots > 1) {
		lv2_slot_t *sf = &c->slots[c->n_slots - 1];
		float *src_l = parity ? c->tmp_b_l : c->tmp_a_l;
		float *src_r = parity ? c->tmp_b_r : c->tmp_a_r;
		if (sf->audio_in_idx[0] >= 0)  lilv_instance_connect_port(sf->inst, sf->audio_in_idx[0], src_l);
		if (sf->audio_in_idx[1] >= 0)  lilv_instance_connect_port(sf->inst, sf->audio_in_idx[1], src_r);
		if (sf->audio_out_idx[0] >= 0) lilv_instance_connect_port(sf->inst, sf->audio_out_idx[0], out_l);
		if (sf->audio_out_idx[1] >= 0) lilv_instance_connect_port(sf->inst, sf->audio_out_idx[1], out_r);
		lilv_instance_run(sf->inst, N);
	}
	return 0;
}

void chain_free(chain_t *c)
{
	if (!c) return;
	for (int i = 0; i < c->n_slots; i++) {
		lilv_instance_deactivate(c->slots[i].inst);
		lilv_instance_free(c->slots[i].inst);
	}
	if (u_audio)  lilv_node_free(u_audio);
	if (u_input)  lilv_node_free(u_input);
	if (u_output) lilv_node_free(u_output);
	if (u_ctrl)   lilv_node_free(u_ctrl);
	if (u_atom)   lilv_node_free(u_atom);
	lilv_world_free(c->world);
	free(c);
}
