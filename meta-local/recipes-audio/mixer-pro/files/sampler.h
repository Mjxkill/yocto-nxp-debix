// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * sampler — V12-SMP : sampleur one-shot (page PADS).
 *
 * WAVs de /var/lib/ala/samples préchargés en RAM (control thread), lus
 * one-shot dans les tranches P1/P2. Publication sous g_st.target_lock ;
 * libération DIFFÉRÉE des anciens buffers (purge au reload suivant —
 * jamais de free d'un buffer potentiellement lu par l'audio).
 * ARCHI_V12_SAMPLER.md.
 *
 * Extraction V14.0 (étape 1, ARCHI_V14_RESTRUCTURATION.md) depuis
 * mixer-pro.c — code déplacé tel quel. g_smp reste exposé tant que les
 * ops (trigger/stop/list/reload) vivent dans le control socket ; elles
 * migreront ici à l'étape 4 et g_smp redeviendra privé.
 */
#ifndef MIXER_SAMPLER_H
#define MIXER_SAMPLER_H

#include <stdatomic.h>
#include <stdint.h>

#include "mixer-pro.h"   /* N_INPUT_REAL, PERIOD_FRAMES */

#define SMP_SLOTS 16
#define SMP_DIR "/var/lib/ala/samples"   /* main() fait le mkdir au boot */

struct smp_slot {
	char name[64];
	float *buf;              /* stéréo entrelacé LR, 48 kHz */
	uint32_t frames;
	_Atomic int playing;
	_Atomic uint32_t pos;
	float gain;
};

extern struct smp_slot g_smp[SMP_SLOTS];   /* lu par les ops control (étape 4 : privé) */

/* Scan du répertoire (tri alpha → slots). Appelé au démarrage (avant
 * threads, locked=0) et par sampler_reload (control thread, locked=1 :
 * publie sous g_st.target_lock). */
void smp_scan(int locked);

/* Rendu (audio_thread, SOUS target_lock, après le convert S32→float) :
 * mixe les slots actifs dans les tranches P1/P2 (in_block[16/17]). */
void smp_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES]);

#endif /* MIXER_SAMPLER_H */
