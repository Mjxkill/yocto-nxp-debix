// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * midix — V12-MIDIX : expandeur MIDI (consumer du ring SHM).
 *
 * Le daemon midi-expander (fluidsynth, cores 0-1) rend le son du module
 * MIDI dans /dev/shm/ala-midix ; l'audio_thread le pop (non-bloquant,
 * zéros si retard/absent) et l'ADDITIONNE dans P1/P2 comme le sampleur
 * et le looper. mmap fait par persistence_thread (1 Hz, jamais en RT).
 * ARCHI_V12_MIDI_EXPANDER.md.
 *
 * Extraction V14.0 (étape 1, ARCHI_V14_RESTRUCTURATION.md) depuis
 * mixer-pro.c — code déplacé tel quel. g_midix reste exposé tant que les
 * ops (status/gain) vivent dans le control socket (migration étape 4).
 */
#ifndef MIXER_MIDIX_H
#define MIXER_MIDIX_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "mixer-pro.h"   /* N_INPUT_REAL, PERIOD_FRAMES */

#define MIDIX_MAGIC 0x4D494458u

struct midix_hdr {
	uint32_t magic;
	uint32_t ring_frames;
	_Atomic uint32_t widx;
	uint32_t _pad;
};

extern struct midix_state {
	struct midix_hdr *_Atomic hdr;   /* NULL tant que non mappé */
	float   *data;
	size_t   map_sz;
	uint32_t ridx;                    /* cursor consumer privé */
	float    gain;
	_Atomic uint32_t underruns;
	_Atomic uint32_t peak;
} g_midix;

/* persistence_thread (1 Hz) — tente le mmap tant que le daemon n'est pas
 * là ; invalide si le magic disparaît (arrêt propre du daemon). */
void midix_try_map(void);

/* Rendu (audio_thread, SOUS target_lock, après loop_render) */
void midix_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES]);

#endif /* MIXER_MIDIX_H */
