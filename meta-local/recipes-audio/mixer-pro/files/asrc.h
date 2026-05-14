/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V8.2 — Async sample rate converter pour corriger le drift entre
 * 2 horloges audio (DSP TAC5212 SAI7 vs USB host PC vs Phone aloop).
 *
 * Algorithme : cubic Lagrange 4-tap (~21 µs latence), ratio R adaptatif
 * piloté par PID sur le fill-level d'un ring SPSC externe. PID gains
 * conservateurs + anti-windup. EMA sur ratio pour affichage drift_ppm.
 *
 * Faible latence : 1 sample d'avance pour le futur x[3] = 21 µs négligeable
 * devant le budget < 10 ms.
 *
 * Capacité : jusqu'à 8 canaux interleaved, float32 in/out, ratio clampé
 * ±500 ppm (= 0.0005). Si drift physique > 500 ppm, status "fail" remonté.
 */

#ifndef __ASRC_H__
#define __ASRC_H__

#include <stdint.h>
#include <stdatomic.h>

#define ASRC_MAX_CH        8
#define ASRC_RATIO_MIN     0.9995f   /* -500 ppm clamp */
#define ASRC_RATIO_MAX     1.0005f   /* +500 ppm clamp */
#define ASRC_FADE_FRAMES   240       /* 5 ms @ 48 kHz fade-in après reset */

typedef struct {
	int       channels;
	float     setpoint;        /* target ring fill in frames */

	/* PID state */
	float     kp;              /* proportional gain */
	float     ki;              /* integral gain */
	float     integral;        /* accumulated error, clamped */
	float     integral_max;    /* anti-windup clamp */
	float     ratio;           /* current resampling ratio (1.0 = nominal) */

	/* EMA for drift display (tau ~1s @ 500 Hz update rate) */
	float     ratio_ema;
	float     ema_alpha;       /* = 1 - exp(-dt/tau) */

	/* Interpolation state */
	float     pos;             /* sub-sample position [0..1) */
	float     hist[ASRC_MAX_CH][4];   /* per-channel 4-tap history */

	/* Crossfade after reset to mask history discontinuity */
	int       fade_remaining;  /* frames remaining of fade-in (0 = no fade) */

	/* Stats */
	atomic_ulong frames_in_total;
	atomic_ulong frames_out_total;
	atomic_int   status;       /* 0=ok, 1=warn (>100ppm), 2=fail (clamp atteint) */
} asrc_t;

/* Initialize ASRC for N channels, target ring fill setpoint */
void asrc_init(asrc_t *a, int channels, float setpoint_frames);

/* Reset interpolation history (call on PCM recover / replug).
 * Engages crossfade fade-in on next asrc_run to mask the discontinuity. */
void asrc_reset(asrc_t *a);

/* Process : convert in_frames frames of input to exactly out_frames output.
 * Returns number of input frames actually consumed. Modifies a->pos.
 * Note : out_frames is asked-target ; the function consumes input as needed.
 */
int  asrc_run(asrc_t *a, const float *in, int in_frames,
              float *out, int out_frames);

/* Update PID controller from ring fill (call at each audio_thread tick,
 * = 500 Hz). EMA on ratio for stable drift_ppm display. */
void asrc_update_pid(asrc_t *a, int ring_fill_frames);

/* Get smoothed drift in parts-per-million for display. */
float asrc_drift_ppm(const asrc_t *a);

/* Get current ratio (raw) for debugging. */
float asrc_ratio(const asrc_t *a);

#endif /* __ASRC_H__ */
