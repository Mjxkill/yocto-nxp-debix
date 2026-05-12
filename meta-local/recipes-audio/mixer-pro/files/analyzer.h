/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V7.0-E7.5 — analyzer : 4 configurable taps producing FFT magnitude
 * spectrum + X-Y phase scope samples for the GUI live view.
 *
 * Concept :
 *   - audio_thread writes 1 stereo sample per frame into each active tap's
 *     double-buffer ring (one buffer being written, one buffer ready).
 *   - analyzer_thread (priority RT_PRIO_ANALYZER, lower than audio's
 *     RT_PRIO_AUDIO) wakes every ANALYZER_PERIOD_US, runs Hann + FFT_1024
 *     on the ready buffer, downsamples to TAP_BINS_OUT magnitudes in dB,
 *     copies TAP_SCOPE_N stereo pairs, stores RMS dB, all under a per-tap
 *     mutex so the control_thread can snapshot it for the JSON wire.
 *
 * Sample sources (tap_kind_t / a, b) are described in mixer-pro.h.
 */

#ifndef __ANALYZER_H__
#define __ANALYZER_H__

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include "mixer-pro.h"

typedef struct {
	/* Configuration (atomic so audio_thread sees changes without lock).
	 * kind = TAP_KIND_NONE freezes writes (audio loop early-exit).
	 * b = -1 → mono (only L written, R duplicates L). */
	atomic_int kind;
	atomic_int a;
	atomic_int b;

	/* Double-buffer ring. side toggles 0/1 each time wpos rolls over
	 * FFT_N. analyzer_thread reads the 1-side buffer (the one just
	 * filled), audio_thread writes the 0-side. */
	float ring_l[2][TAP_FFT_N];
	float ring_r[2][TAP_FFT_N];
	atomic_int wpos;
	atomic_int side;          /* 0..1, which buffer is currently being filled */
	atomic_int ready_seq;     /* incremented every full buffer; 0 = never ready */

	/* Latest analyzer output (snapshot taken by control_thread). */
	pthread_mutex_t out_lock;
	int8_t   out_spec[TAP_BINS_OUT];          /* dB, clipped -120..0 */
	int16_t  out_scope[TAP_SCOPE_N * 2];      /* L, R interleaved, S16 from float */
	float    out_rms_dB;
	uint32_t out_seq;                         /* monotonic */
} mixer_tap_t;

/* Init/teardown : zero-state, ready to receive set_tap commands. */
void analyzer_taps_init(mixer_tap_t taps[N_TAPS]);
void analyzer_taps_destroy(mixer_tap_t taps[N_TAPS]);

/* analyzer_thread main loop (started by main() with pthread_create). */
void *analyzer_thread(void *arg);

/* Hot-path helper used by audio_thread. Writes 1 stereo sample into the
 * tap's current ring side and toggles the side when the buffer fills. */
static inline void analyzer_tap_write(mixer_tap_t *t, float l, float r)
{
	int kind = atomic_load_explicit(&t->kind, memory_order_relaxed);
	if (kind == TAP_KIND_NONE)
		return;
	int side = atomic_load_explicit(&t->side, memory_order_relaxed);
	int wpos = atomic_load_explicit(&t->wpos, memory_order_relaxed);
	t->ring_l[side][wpos] = l;
	t->ring_r[side][wpos] = r;
	int next = wpos + 1;
	if (next >= TAP_FFT_N) {
		atomic_store_explicit(&t->side,  side ^ 1, memory_order_release);
		atomic_store_explicit(&t->wpos,  0,        memory_order_relaxed);
		atomic_fetch_add_explicit(&t->ready_seq, 1, memory_order_release);
	} else {
		atomic_store_explicit(&t->wpos, next, memory_order_relaxed);
	}
}

#endif /* __ANALYZER_H__ */
