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
	const char *type_name;          /* "compressor" | "reverb" | "delay" | "eq" */
	void *state;                    /* opaque state par implémentation */

	/* Process 1 frame stéréo (sample interleaved L+R en in, idem out). */
	void (*process)(fx_engine_t *fx, float in_l, float in_r,
			float *out_l, float *out_r);

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

/* Cleanup (libère state). */
void fx_free(fx_engine_t *fx);

#endif /* __MIXER_EFFECTS_H__ */
