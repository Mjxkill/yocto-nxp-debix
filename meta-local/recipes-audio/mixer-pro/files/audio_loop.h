// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * audio_loop — le cœur temps réel du mixer.
 *
 *  - audio_thread (SCHED_FIFO 99, core 2, self-paced 2 ms) : capture 3 PCMs
 *    → convert S32→float (remap mics) → exp → eqx → cmp → duck(vfocus) →
 *    smp/loop/midix → automix_update → smooth_gains → mix_block (NEON) →
 *    taps analyzer + SHM tap → vspat → meq+makeup (crossfade) → insert LV2
 *    → out_gain → push rings (DSP eventfd, UAC2, phone) ;
 *  - play_thread : consumer du ring SPSC DSP → writei play_dsp.
 *
 * smooth_gains et mix_block sont INTERNES au module (seul audio_thread les
 * appelle). Les gains lissés de sortie (g_out_gain_cur) sont audio-owned.
 *
 * Extraction V14.0 (étape 3b, ARCHI_V14_RESTRUCTURATION.md) depuis
 * mixer-pro.c — code déplacé tel quel.
 */
#ifndef MIXER_AUDIO_LOOP_H
#define MIXER_AUDIO_LOOP_H

#include "mixer-pro.h"   /* N_OUTPUT_TOTAL */

/* Options --no-uac2 / --no-phone (écrites par main avant les threads).
 * Input skipped = samples à 0 ; output skipped = pas d'écriture. */
extern int g_skip_uac2;
extern int g_skip_phone;

/* gain de sortie lissé (audio-owned, init 1.0 par main au boot) */
extern float g_out_gain_cur[N_OUTPUT_TOTAL];

/* threads (spawnés par main) */
void *audio_thread(void *arg);
void *play_thread(void *arg);

#endif /* MIXER_AUDIO_LOOP_H */
