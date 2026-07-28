// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * uac2_ring — V8.x : isolation USB UAC2 complète (rings SPSC + threads +
 * régulation fill-based + mesure de drift).
 *
 * Ring SPSC dédié pour chaque direction UAC2 (cap + play), alimenté par
 * 1 thread RT dédié qui own le PCM en BLOCKING. Découple totalement
 * l'USB UAC2 du chemin DSP : un blocage USB (msleep tac5212_trigger sur
 * cap, host PipeWire suspend sur play, unplug…) reste confiné dans son
 * thread, n'affecte pas le DSP. audio_thread voit le UAC2 comme une simple
 * lecture « always latest period » (silence si rien), push best-effort.
 * Régulation V8.24 : 4 paliers proportionnels par niveau de ring (inserts/
 * drops étalés via crossfade K=8 — inaudible). Drift mesuré passivement
 * (ppm, EMA) ; correction pilotée par les events du ring cap uniquement.
 *
 * Extraction V14.0 (étape 3, ARCHI_V14_RESTRUCTURATION.md) depuis
 * mixer-pro.c — code déplacé tel quel. Les compteurs diag restent exposés
 * ici tant que les ops get_drift/reset_drift_stats vivent dans le control
 * socket (étape 4) ; audio_thread consomme l'API ring + timing (étape 3b).
 */
#ifndef MIXER_UAC2_RING_H
#define MIXER_UAC2_RING_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "mixer-pro.h"   /* PERIOD_FRAMES */

/* --- dimensions + paliers de régulation (V8.22/V8.24) --- */
#define UAC2_CH            8   /* = N_INPUT_STEMS = N_OUTPUT_UAC2 */
#define UAC2_RING_PERIODS  4
#define UAC2_RING_FRAMES   (PERIOD_FRAMES * UAC2_RING_PERIODS)
#define UAC2_FILL_LOW_HARD  0                              /* +6 inserts */
#define UAC2_FILL_LOW       (1 * PERIOD_FRAMES)            /* 96  +2 inserts */
#define UAC2_FILL_HIGH      (3 * PERIOD_FRAMES)            /* 288 -2 drops */
#define UAC2_FILL_HIGH_HARD (4 * PERIOD_FRAMES)            /* 384 -6 drops */
#define UAC2_FILL_TARGET    (2 * PERIOD_FRAMES)            /* 192 pre-fill */
enum {
	UAC2_MODE_IDLE = 0,
	UAC2_MODE_CORRECT_LOW,   /* cap=insert, play=slow */
	UAC2_MODE_CORRECT_HIGH,  /* cap=drop, play=speed */
};

typedef struct {
	int32_t      buf[UAC2_RING_FRAMES * UAC2_CH];
	atomic_uint  wr;
	atomic_uint  rd;
	atomic_ulong drops;       /* nb de FRAMES droppées (cumul) */
	atomic_ulong drops_evt;   /* nb d'ÉVÉNEMENTS push-full (cumul) */
	atomic_ulong empty_evt;   /* nb d'ÉVÉNEMENTS pop-empty (avail<requested) */
	atomic_ulong xruns;
} uac2_ring_t;

extern uac2_ring_t g_ring_uac2_cap;
extern uac2_ring_t g_ring_uac2_play;

/* --- API ring (SPSC, acquire/release — pattern éprouvé ring DSP) --- */
int  uac2_ring_pop_n(uac2_ring_t *r, int32_t *out, int n);
int  uac2_ring_pop_period(uac2_ring_t *r, int32_t *out);
void uac2_ring_push_n(uac2_ring_t *r, const int32_t *in, int n);
void uac2_ring_push_period(uac2_ring_t *r, const int32_t *in);
/* push 96 frames ATOMIQUE ou drops_evt++ sans troncature (V8.33) */
int  uac2_ring_try_push_period(uac2_ring_t *r, const int32_t *in);
unsigned uac2_ring_fill(uac2_ring_t *r);

/* --- threads (spawnés par main) --- */
void *cap_uac2_thread(void *arg);
void *play_uac2_thread(void *arg);
void *shift_controller_thread(void *arg);

/* --- drift + réglages ASRC (ops + main --no-asrc/--fixed-shift) --- */
extern _Atomic int g_usb_drift_ppm_x100;   /* drift × 100 (0,01 ppm) */
extern _Atomic int g_usb_drift_valid;
extern _Atomic int g_shift_ppm;
extern _Atomic int g_no_asrc;
extern _Atomic int g_shift_fixed;

/* --- compteurs diag (op get_drift / reset_drift_stats — étape 4) --- */
extern _Atomic unsigned long g_dbg_corr_req_insert, g_dbg_corr_req_drop;
extern _Atomic unsigned long g_dbg_corr_app_insert, g_dbg_corr_app_drop;
extern _Atomic unsigned long g_dbg_readi_lt10, g_dbg_readi_10_50;
extern _Atomic unsigned long g_dbg_readi_50_100, g_dbg_readi_ge100;
extern _Atomic unsigned long g_dbg_cc_called, g_dbg_cc_nonzero;
extern _Atomic int           g_dbg_corr_acc_max;
extern _Atomic int g_uac2_cap_warm, g_uac2_play_warm;
extern _Atomic int g_uac2_cap_mode, g_uac2_play_mode;

/* --- V8.32 : timing wr/rd, fenêtre glissante 10 s + min/max persistants --- */
#define TIMING_WINDOW_SEC 10
extern _Atomic uint64_t g_wr_bucket_sum[TIMING_WINDOW_SEC];
extern _Atomic uint32_t g_wr_bucket_cnt[TIMING_WINDOW_SEC];
extern _Atomic uint64_t g_wr_bucket_epoch[TIMING_WINDOW_SEC];
extern _Atomic uint64_t g_rd_bucket_sum[TIMING_WINDOW_SEC];
extern _Atomic uint32_t g_rd_bucket_cnt[TIMING_WINDOW_SEC];
extern _Atomic uint64_t g_rd_bucket_epoch[TIMING_WINDOW_SEC];
extern _Atomic uint32_t g_wr_min_us, g_wr_max_us, g_rd_min_us, g_rd_max_us;
extern struct timespec g_last_wr_ts, g_last_rd_ts;

/* --- V9.1 : histogramme iter + wake jitter (audio_thread écrit) --- */
extern _Atomic unsigned long g_iter_lt18, g_iter_18_22, g_iter_22_30,
			     g_iter_30_50, g_iter_ge50;
extern _Atomic long g_wake_jitter_max_us, g_wake_jitter_sum_us;
extern _Atomic unsigned long g_wake_jitter_count;

/* --- V8.15/V8.16 : dumps raw (ouverts par main via --dump-*) --- */
extern FILE *g_usb_cap_dump;
extern FILE *g_dsp_play_dump;

#endif /* MIXER_UAC2_RING_H */
