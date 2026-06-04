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

#define MIXER_VERSION  "v9.3.2-neon"

/* E6.g Phase 2 + E6.h tuning : ring buffer SPSC entre thread audio (cap+mix)
 * et thread play DSP. Taille = N_RING_PERIODS périodes × 18 ch × 4 B.
 * 8 périodes × 96 frames × 18 × 4 = 55 KB → tient en L1+L2 A53.
 * Tampon de 16 ms = compromis latence/recover :
 *   - absorbe < 16 ms de jitter sans drop
 *   - recover SOF > 16 ms (peut atteindre 60 ms) → drops bornés visibles
 *     dans ring_drops, audio coupé pendant le recover (acceptable, rare)
 */
#define N_RING_PERIODS  8
#define RING_FRAMES     (PERIOD_FRAMES * N_RING_PERIODS)

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
#define N_PERIODS       4       /* 4 periods = 8 ms buffer ALSA (E6.g.p2 : le ring SPSC absorbe les recover) */
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

/* V9.0 — RT priorities + per-thread CPU pinning (PREEMPT_RT kernel).
 *
 * Sur cores isolés (isolcpus=2,3) on pin chaque thread sur 1 core unique
 * pour éliminer la concurrence intra-prio.
 *
 * Core 2 : audio + DSP play   (pipeline DSP cap → mix → DSP play)
 *   audio_thread       prio 99 → max RT, jamais préempté
 *   play_thread        prio 98 → tourne dans les "trous" d'audio (sleep/blocking)
 *
 * Core 3 : USB UAC2 cap + play (pipeline USB host ↔ ring SPSC)
 *   cap_uac2_thread    prio 95
 *   play_uac2_thread   prio 95
 *
 * Cores 0,1 : non-RT critique
 *   analyzer_thread    prio 60 (FFT taps, peut tolérer du jitter)
 *   control_thread     SCHED_OTHER (Unix socket IPC)
 *   mixer-gui-http     CPUAffinity service
 */
#define RT_PRIO_AUDIO        99
#define RT_PRIO_PLAY         98
#define RT_PRIO_UAC2_CAP     95
#define RT_PRIO_UAC2_PLAY    95
#define CPU_AUDIO            2
#define CPU_PLAY             2
#define CPU_UAC2_CAP         3
#define CPU_UAC2_PLAY        3

/* In/out vector indices (utiles pour le protocole JSON) :
 *   in_id  : 0..N_INPUT_REAL-1 = sources réelles
 *            N_INPUT_REAL..N_INPUT_TOTAL-1 = returns FX (18..25)
 *   out_id : 0..N_OUTPUT_TOTAL-1
 *   bus_id : 0..N_BUS_FX_CH-1  (4 stéréo = ch 0L 0R 1L 1R 2L 2R 3L 3R)
 */

/* E7.5 — analyzer taps.
 *   N_TAPS     : 4 GUI analyzer slots, user-configurable at runtime
 *   FFT_N      : 1024 samples = ~21 ms @ 48 kHz, gives 23 Hz/bin resolution
 *   BINS_OUT   : 128 half-spectrum bins delivered to the GUI (downsampled
 *                from FFT_N/2 = 512 via 4:1 magnitude peak hold)
 *   SCOPE_N    : 64 stereo sample pairs (~1.3 ms) for the X-Y phase scope.
 *   ANALYZER_PERIOD_US : 33 ms = ~30 Hz refresh.
 */
#define N_TAPS                4
#define TAP_FFT_N          1024
#define TAP_BINS_OUT        128
#define TAP_SCOPE_N          64
#define ANALYZER_PERIOD_US 33000
#define RT_PRIO_ANALYZER     60

typedef enum {
	TAP_KIND_NONE     = 0,
	TAP_KIND_INPUT    = 1,   /* a/b : 0..17 in[], 18..25 returns FX */
	TAP_KIND_BUS_PRE  = 2,   /* a/b : 0..7 bus pre-FX                */
	TAP_KIND_OUTPUT   = 3,   /* a/b : 0..17 out[]                    */
} tap_kind_t;

#endif /* __MIXER_PRO_H__ */
