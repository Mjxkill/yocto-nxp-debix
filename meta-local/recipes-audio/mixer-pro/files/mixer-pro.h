/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V7.0-E6.d — mixer-pro : console de mixage SW style DAW
 *
 * Architecture (V7.0) :
 *
 *   INPUTS (26 mono)                BUS FX (4 stéréo = 8 ch)        OUTPUTS (18 mono)
 *   ─────────────────────────       ────────────────────────         ───────────────────
 *   [0..7]   DSP cap mics      ─┐   FX1 L/R (passthrough MVP)        [0..7]  DSP play
 *   [8..15]  UAC2 in (stems)    ├── FX2 L/R                          [8..15] UAC2 out
 *   [16..17] Phone in           │   FX3 L/R                          [16..17] Phone out
 *   [18..25] returns FX1..4 LR  │   FX4 L/R
 *                               │              │
 *                               │              ▼ master matrix 34 × 18
 *                               └────────────► master gain
 *
 *   Matrix routing :
 *     send_gain[26][8]   — send level (par voie × bus L/R) : 4 sends stéréo
 *     master_gain[34][18] — mix final : (26 inputs + 8 returns) × 18 outputs
 *
 *   Format interne : float32 normalisé [-1.0, +1.0] (convertir ALSA S32_LE en in/out).
 */

#ifndef __MIXER_PRO_H__
#define __MIXER_PRO_H__

#include <stdint.h>

#define MIXER_VERSION  "v7.0-e6d"

#define N_INPUT_MICS    8      /* DSP TAC5212 cap */
#define N_INPUT_STEMS   8      /* UAC2 in (DAW PC) */
#define N_INPUT_PHONE   2      /* Phone aloop in (placeholder VoIP) */
#define N_INPUT_REAL    (N_INPUT_MICS + N_INPUT_STEMS + N_INPUT_PHONE)   /* 18 */

#define N_BUS_FX        4      /* 4 bus stéréo */
#define N_BUS_FX_CH     (N_BUS_FX * 2)                                  /* 8 ch */
#define N_RETURN_CH     N_BUS_FX_CH                                     /* 8 ch retours */

#define N_INPUT_TOTAL   (N_INPUT_REAL + N_RETURN_CH)                    /* 26 sources mixer */

#define N_OUTPUT_DSP    8      /* DSP play 8 ch speakers */
#define N_OUTPUT_UAC2   8      /* UAC2 out vers DAW PC */
#define N_OUTPUT_PHONE  2      /* Phone aloop out */
#define N_OUTPUT_TOTAL  (N_OUTPUT_DSP + N_OUTPUT_UAC2 + N_OUTPUT_PHONE) /* 18 */

#define SAMPLE_RATE     48000
#define PERIOD_FRAMES   96      /* 2 ms @ 48 kHz */
#define N_PERIODS       4       /* 4 periods = 8 ms buffer (marge xrun) */
#define BUFFER_FRAMES   (PERIOD_FRAMES * N_PERIODS)

/* Smoothing : 64 frames de ramp (= 1.33 ms) sur changement de gain */
#define GAIN_RAMP_FRAMES 64

/* PCM device names */
#define PCM_DSP_CAP     "hw:softac5212tdm,0"
#define PCM_DSP_PLAY    "hw:softac5212tdm,0"
#define PCM_UAC2_CAP    "hw:UAC2Gadget,0"
#define PCM_UAC2_PLAY   "hw:UAC2Gadget,0"
#define PCM_PHONE_CAP   "hw:Phone,1"           /* aloop subdev 1 = capture side */
#define PCM_PHONE_PLAY  "hw:Phone,0"

/* Control socket */
#define MIXER_SOCK_PATH "/run/mixer-pro.sock"

/* RT priorities */
#define RT_PRIO_AUDIO   80

/* In/out vector indices (utiles pour le protocole JSON) :
 *   in_id  : 0..N_INPUT_REAL-1 = sources réelles
 *            N_INPUT_REAL..N_INPUT_TOTAL-1 = returns FX (18..25)
 *   out_id : 0..N_OUTPUT_TOTAL-1
 *   bus_id : 0..N_BUS_FX_CH-1  (4 stéréo = ch 0L 0R 1L 1R 2L 2R 3L 3R)
 */

#endif /* __MIXER_PRO_H__ */
