/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V7.0-E6.e — Effets natifs C pour les 4 bus FX stéréo du mixer-pro.
 *
 * Abstraction `fx_engine` avec vtable pour permettre futur swap LV2 (E6.f+)
 * sans refactor du mixer core.
 *
 * Par défaut au démarrage :
 *   Bus FX1 (idx 0) = Compressor (peak + gain reduction)
 *   Bus FX2 (idx 1) = Reverb Schroeder (4 combs + 2 allpass)
 *   Bus FX3 (idx 2) = Delay (line + feedback)
 *   Bus FX4 (idx 3) = EQ 3-band (biquads low-shelf + peak + high-shelf)
 *
 * Tous les paramètres sont smoothing-applied dans le moteur lui-même
 * (envelope follower pour compresseur, ramp pour gains).
 */

#ifndef __MIXER_EFFECTS_H__
#define __MIXER_EFFECTS_H__

#include <stdint.h>
#include <stddef.h>

/* Paramètres communs : indexés par nom string dans le protocole socket. */
typedef struct fx_engine fx_engine_t;

struct fx_engine {
	const char *type_name;          /* "compressor" | "reverb" | "delay" | "eq" | "lv2" */
	void *state;                    /* opaque state par implémentation */

	/* V9.3 : process block stéréo de N samples.
	 * - in_l, in_r : buffers d'entrée (N floats chacun)
	 * - out_l, out_r : buffers de sortie (N floats chacun, écriture en place OK)
	 * - N : nombre de samples à traiter (typique 96 = PERIOD_FRAMES)
	 *
	 * Block-based pour permettre :
	 *  1. lilv_instance_run(N) en 1 appel pour LV2 (vs 96 calls × N=1)
	 *  2. Auto-vectorisation NEON par gcc sur les boucles inner
	 *  3. Amortir overhead vtable indirect (1 call vs 96 par bus)
	 */
	void (*process_block)(fx_engine_t *fx,
			      const float *in_l, const float *in_r,
			      float *out_l, float *out_r,
			      uint32_t N);

	/* set_param : retourne 0 si OK, -1 si param inconnu. */
	int  (*set_param)(fx_engine_t *fx, const char *name, float value);

	/* Reset état interne (zero buffers etc.) sans toucher aux paramètres. */
	void (*reset)(fx_engine_t *fx);

	/* Format JSON des paramètres courants dans `buf` (max `len` octets).
	 * Retourne nb octets écrits (sans le \0).
	 */
	int  (*get_state)(fx_engine_t *fx, char *buf, int len);
};

/* Constructors (allouent state interne, retournent 1 si OK). */
int fx_init_compressor(fx_engine_t *fx, float sample_rate);
int fx_init_reverb    (fx_engine_t *fx, float sample_rate);
int fx_init_delay     (fx_engine_t *fx, float sample_rate);
int fx_init_eq        (fx_engine_t *fx, float sample_rate);

/* V9.2 — LV2 plugin host (lilv-0).
 * Charge dynamiquement un plugin LV2 par son URI, vérifie hardRTCapable,
 * connecte les ports audio L/R + control ports avec valeurs default.
 * Retourne 1 si OK, 0 si plugin introuvable, non-RT, ou stéréo mismatch.
 *
 * fx_lv2_list_uris(): retourne une string JSON array des URIs des plugins
 *   LV2 RT-safe disponibles. À utiliser par GUI pour le sélecteur.
 *   Buffer alloué par caller, retourne nb d'octets écrits.
 */
int fx_init_lv2(fx_engine_t *fx, float sample_rate, const char *uri);
int fx_lv2_list_uris(char *buf, int len);

/* V9.4 — Chain : cascade de N sub-engines (LV2 ou builtin) sur 1 bus.
 * Usage : insert mastering post-master sur out_0+out_1 DSP.
 * MAX_CHAIN = 8 sub-engines max. Ping-pong buffers tmp_a/tmp_b globaux.
 *
 * Spec de chaque sub-engine :
 *   {engine: "lv2"|"compressor"|"reverb"|"delay"|"eq",
 *    uri: "..." (lv2 seulement)}
 */
#define FX_CHAIN_MAX 8

struct fx_chain_spec {
	const char *engine;   /* "lv2" | "compressor" | "reverb" | "delay" | "eq" */
	const char *uri;      /* pour lv2 seulement (sinon NULL/"") */
};

int fx_init_chain(fx_engine_t *fx, float sample_rate,
                  const struct fx_chain_spec *specs, int n_specs);

/* Cleanup (libère state). */
void fx_free(fx_engine_t *fx);

#endif /* __MIXER_EFFECTS_H__ */
