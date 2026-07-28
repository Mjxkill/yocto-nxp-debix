// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * util — logging + helpers ALSA + conversions S32↔float du mixer.
 *
 * Extraction V14.0 (étape 0, ARCHI_V14_RESTRUCTURATION.md) depuis
 * mixer-pro.c — code déplacé tel quel, noms conservés.
 */
#ifndef MIXER_UTIL_H
#define MIXER_UTIL_H

#include <stdint.h>
#include <alsa/asoundlib.h>

#include "state.h"   /* struct alsa_pcm, g_st (xrun_count) */

/* log une ligne sur stderr (journald l'horodate) */
void mlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ouvre + configure un PCM : S32_LE interleaved, 48 kHz, period/buffer du
 * moteur, start_threshold 1 period, HW timestamping (V8.26). */
int pcm_open(struct alsa_pcm *p, const char *name, int channels,
	     snd_pcm_stream_t dir);

/* snd_pcm_recover + comptage xrun global (g_st.xrun_count) */
int pcm_recover(snd_pcm_t *pcm, int err);

/* Conversions S32_LE ↔ float [-1,1) — chemin RT, restent inline. */
static inline float s32_to_f(int32_t s)
{
	return (float)s / 2147483648.0f;
}

static inline int32_t f_to_s32(float f)
{
	if (f >  0.999999f) f =  0.999999f;
	if (f < -1.0f)      f = -1.0f;
	return (int32_t)(f * 2147483648.0f);
}

#endif /* MIXER_UTIL_H */
