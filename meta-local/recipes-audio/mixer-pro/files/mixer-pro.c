// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * V7.0-E6.d — mixer-pro : console de mixage SW style DAW
 *
 * Single-threaded RT loop : capture (3 PCMs) → mixer (sends + bus + master) →
 * playback (3 PCMs). Latence cible < 10 ms end-to-end (2 ms DMA + 6 ms pipeline
 * + 2 ms DMA). Voir mixer-pro.h pour l'architecture détaillée.
 *
 * Contrôle : socket Unix /run/mixer-pro.sock — protocole JSON ligne par ligne.
 *
 *   { "op":"set_send",          "in":<0..25>, "bus":<0..7>, "gain":<float> }
 *   { "op":"set_master",        "src":<0..25>, "out":<0..17>, "gain":<float> }
 *   { "op":"set_input_gain",    "src":<0..25>, "gain":<float> }  (E7.2 strip)
 *   { "op":"set_mute",          "src":<0..25>, "mute":<0|1> }
 *   { "op":"get_strip_routing", "src":<0..25> } → JSON {sends[8], master[18], gain, mute}
 *   { "op":"get_state" }     → réponse JSON multilignes
 *   { "op":"reset" }         → matrix à 0, strip gain à 1
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#include "mixer-pro.h"
#include "effects.h"
#include "analyzer.h"
#include "state.h"    /* V14.0 étape 0 : struct mixer_state + extern g_st */
#include "util.h"     /* V14.0 étape 0 : mlog, pcm_open, pcm_recover, s32↔f */
#include "dsp_bq.h"   /* V14.0 étape 0 : biquads RBJ (eqx_bq, designers) */

/* ============================== State ============================== */
/* struct alsa_pcm + struct mixer_state : déplacées dans state.h (V14.0
 * étape 0). L'instance globale reste définie ICI — state.h ne porte que
 * les types et l'extern. */

struct mixer_state g_st;   /* instance unique — extern dans state.h */

/* V8.1.b — Mesure passive du drift USB ↔ DSP (un seul drift, car même
 * horloge USB host pour cap et play). Le thread cap_uac2_thread compte
 * combien de samples il reçoit par seconde de wall-clock (monotonic),
 * compare à 48000 Hz nominal, déduit le drift en ppm.
 * Smoothed via EMA pour stabilité d'affichage.
 * Pas de correction algorithmique — purement informationnel pour le user. */
static _Atomic int    g_usb_drift_ppm_x100 = 0; /* drift_ppm × 100 = 0.01 ppm precision */
static _Atomic int    g_usb_drift_valid = 0;    /* 0 = pas encore mesuré */

/* V8.2 — Asservissement drift par feedback xrun.
 * shift_ppm = correction courante en ppm, init 0, ajusté ±1 par xrun détecté.
 * Convergence : shift_ppm → drift réel au fur et à mesure des xruns.
 * Application : un drop (lire 97 → moyenner 2 → produire 96) ou insert
 * (lire 95 → moyenner 2 voisins → insérer 1 → produire 96) tous les
 * (1_000_000 / |shift_ppm|) samples. Étalé dans le temps = inaudible.
 *
 * Direction selon thread (un seul shift physique, application opposée) :
 *   cap_uac2 :  shift>0 → drop (read 97), shift<0 → insert (read 95)
 *   play_uac2 : shift>0 → insert (pop 95), shift<0 → drop (pop 97)
 *
 * Détection xrun → shift++ :
 *   cap_uac2 snd_pcm_readi -EPIPE  (overrun ALSA cap, host fast)
 *   play_uac2 snd_pcm_writei -EPIPE (underrun ALSA play, host fast)
 * Détection drop ring play → shift-- :
 *   audio_thread push_n: ring_uac2_play full → drops, host slow
 */
/* V8.6 — un seul g_shift_ppm partagé entre cap et play, mais piloté
 * UNIQUEMENT par les events du ring cap (full ou empty), car le play USB
 * n'est pas toujours consommé par le host (Bitwig ouvre parfois cap seul).
 * Convention :
 *   ring_cap full  ↑ → shift +1 (host fast : cap drops, play inserts)
 *   ring_cap empty ↑ → shift -1 (host slow : cap inserts, play drops)
 * compute_correction inverse le sens pour is_play=1. */
static _Atomic int g_shift_ppm = 0;
/* V8.8 — bypass ASRC complet (test isolement, --no-asrc) */
static _Atomic int g_no_asrc = 0;
/* V8.18 — mode test : si !=0, shift_controller_thread n'écrit plus dans
 * g_shift_ppm. Permet de figer shift à une valeur arbitraire via --fixed-shift
 * pour caractériser la correction expérimentalement. */
static _Atomic int g_shift_fixed = 0;

/* V8.19 — diag ASRC : compte les corrections demandées vs réellement appliquées,
 * ainsi que la distribution des n retournés par readi (pour repérer les
 * mini-bursts qui font no-op smooth_*_middle si n < ASRC_K+2). */
static _Atomic unsigned long g_dbg_corr_req_insert = 0;
static _Atomic unsigned long g_dbg_corr_req_drop   = 0;
static _Atomic unsigned long g_dbg_corr_app_insert = 0;
static _Atomic unsigned long g_dbg_corr_app_drop   = 0;
static _Atomic unsigned long g_dbg_readi_lt10   = 0;
static _Atomic unsigned long g_dbg_readi_10_50  = 0;
static _Atomic unsigned long g_dbg_readi_50_100 = 0;
static _Atomic unsigned long g_dbg_readi_ge100  = 0;
/* V8.21 — debug call de compute_correction depuis cap_uac2_thread */
static _Atomic unsigned long g_dbg_cc_called   = 0;
static _Atomic unsigned long g_dbg_cc_nonzero  = 0;
static _Atomic int           g_dbg_corr_acc_max = 0;
/* V8.22 — régulation fill-based par ring (mode = UAC2_MODE_IDLE défini plus bas) */
static _Atomic int g_uac2_cap_warm   = 0;   /* 1 quand fill cap atteint TARGET */
static _Atomic int g_uac2_play_warm  = 0;   /* 1 quand fill play atteint TARGET */
static _Atomic int g_uac2_cap_mode   = 0;
static _Atomic int g_uac2_play_mode  = 0;

/* V8.32 — Timing avec moyenne glissante 10 sec (10 buckets de 1 sec).
 * Min/max globaux persistants, reset uniquement par reset_drift_stats. */
#define TIMING_WINDOW_SEC 10
static _Atomic uint64_t g_wr_bucket_sum[TIMING_WINDOW_SEC] = {0};
static _Atomic uint32_t g_wr_bucket_cnt[TIMING_WINDOW_SEC] = {0};
static _Atomic uint64_t g_wr_bucket_epoch[TIMING_WINDOW_SEC] = {0};
static _Atomic uint64_t g_rd_bucket_sum[TIMING_WINDOW_SEC] = {0};
static _Atomic uint32_t g_rd_bucket_cnt[TIMING_WINDOW_SEC] = {0};
static _Atomic uint64_t g_rd_bucket_epoch[TIMING_WINDOW_SEC] = {0};
static _Atomic uint32_t g_wr_min_us = UINT32_MAX;
static _Atomic uint32_t g_wr_max_us = 0;
static _Atomic uint32_t g_rd_min_us = UINT32_MAX;
static _Atomic uint32_t g_rd_max_us = 0;

/* V9.1 — instrumentation jitter audio_thread :
 *   - Histogramme prof_iter_us en 5 buckets (cible <1.8 ms = 95%+ idéal)
 *   - Wake-up jitter : retard entre t_next ABSTIME et reprise effective
 *   - Outlier log si iter > 3 ms : breakdown wake/cap/mix/push
 */
static _Atomic unsigned long g_iter_lt18  = 0;  /* < 1.8 ms */
static _Atomic unsigned long g_iter_18_22 = 0;  /* 1.8 ms - 2.2 ms (cible) */
static _Atomic unsigned long g_iter_22_30 = 0;  /* 2.2 ms - 3 ms */
static _Atomic unsigned long g_iter_30_50 = 0;  /* 3 ms - 5 ms */
static _Atomic unsigned long g_iter_ge50  = 0;  /* > 5 ms (très bad) */
static _Atomic long g_wake_jitter_max_us  = 0;
static _Atomic long g_wake_jitter_sum_us  = 0;
static _Atomic unsigned long g_wake_jitter_count = 0;
static struct timespec  g_last_wr_ts = {0};
static struct timespec  g_last_rd_ts = {0};
/* V8.15 — dump raw USB cap data après readi, avant tout traitement.
 * Permet de voir ce que l'USB livre exactement. */
static FILE *g_usb_cap_dump = NULL;
/* V8.16 — dump play_dsp_buf après matrix mix, juste avant writei vers DSP play. */
static FILE *g_dsp_play_dump = NULL;

/* Forward defines pour helpers ci-dessous (vraies définitions plus bas) */
#ifndef UAC2_CH
#define UAC2_CH            8   /* = N_INPUT_STEMS = N_OUTPUT_UAC2 */
#define UAC2_RING_PERIODS  4
#define UAC2_RING_FRAMES   (PERIOD_FRAMES * UAC2_RING_PERIODS)
/* V8.22 — Régulation par niveau (fill-based) du ring USB
 * V8.24 — 4 paliers proportionnels (sans machine d'états) :
 *   fill ≤ 0       → +6 inserts (input_n = 90)
 *   fill ≤ 96      → +2 inserts (input_n = 94)
 *   96 < f < 288   → no correction (input_n = 96)
 *   fill ≥ 288     → -2 drops    (input_n = 98)
 *   fill ≥ 384     → -6 drops    (input_n = 102)
 */
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
#endif

/* V8.7 — Helpers drop/insert avec crossfade linéaire sur K=8 samples.
 * Au lieu de modifier 1 sample isolé, on time-compresse (drop) ou time-
 * stretche (insert) une zone de K samples au milieu du buffer via
 * interpolation linéaire. La correction de 1 sample est étalée sur
 * ~170 µs (8 frames à 48 kHz) → bien moins audible que la modif d'un
 * unique sample.
 *
 * DROP : K=8 input samples → K-1=7 output samples, même time-span.
 *        Position dans input : pos_i = i × (K-1)/(K-2) = i × 7/6.
 *        Boundaries préservés (i=0 et i=K-2 tombent sur entiers).
 *
 * INSERT : K=8 input samples → K+1=9 output samples, même time-span.
 *          Position dans input : pos_i = i × (K-1)/K = i × 7/8.
 *          Boundaries préservés. */
#define ASRC_K  8

static int smooth_drop_middle(int32_t *buf, int n)
{
	if (n < ASRC_K + 2) return n;   /* trop court : no-op */
	int M = (n - ASRC_K) / 2;

	/* Compute K-1=7 output samples via linear interp.
	 * Formule : num = i × (K-1), den = K-2
	 *           idx = num/den, frac = num % den
	 *           out = ((den-frac)×in[idx] + frac×in[idx+1]) / den */
	int32_t tmp[(ASRC_K - 1) * UAC2_CH];
	const int den = ASRC_K - 2;     /* 6 */
	for (int i = 0; i < ASRC_K - 1; i++) {
		int num  = i * (ASRC_K - 1);   /* i × 7 */
		int idx  = num / den;
		int frac = num - idx * den;
		for (int ch = 0; ch < UAC2_CH; ch++) {
			int64_t a = buf[(M + idx)     * UAC2_CH + ch];
			int64_t b = buf[(M + idx + 1) * UAC2_CH + ch];
			tmp[i * UAC2_CH + ch] =
			    (int32_t)(((den - frac) * a + frac * b) / den);
		}
	}

	/* Shift le tail à gauche (suppression d'1 frame entre M+K-1 et M+K) */
	if (n - M - ASRC_K > 0) {
		memmove(&buf[(M + ASRC_K - 1) * UAC2_CH],
		        &buf[(M + ASRC_K)     * UAC2_CH],
		        (n - M - ASRC_K) * UAC2_CH * sizeof(int32_t));
	}
	/* Écrit tmp à la position M */
	memcpy(&buf[M * UAC2_CH], tmp,
	       (ASRC_K - 1) * UAC2_CH * sizeof(int32_t));
	return n - 1;
}

static int smooth_insert_middle(int32_t *buf, int n)
{
	if (n < ASRC_K + 2) return n;
	int M = (n - ASRC_K) / 2;

	/* Compute K+1=9 output samples via linear interp.
	 * num = i × (K-1), den = K  →  pos = i × 7/8 */
	int32_t tmp[(ASRC_K + 1) * UAC2_CH];
	const int den = ASRC_K;         /* 8 */
	for (int i = 0; i <= ASRC_K; i++) {
		int num  = i * (ASRC_K - 1);   /* i × 7 */
		int idx  = num / den;
		int frac = num - idx * den;
		for (int ch = 0; ch < UAC2_CH; ch++) {
			int64_t a = buf[(M + idx)     * UAC2_CH + ch];
			/* protection OOB sur le dernier i où frac=0 */
			int64_t b = (frac == 0) ? a
			          : (int64_t)buf[(M + idx + 1) * UAC2_CH + ch];
			tmp[i * UAC2_CH + ch] =
			    (int32_t)(((den - frac) * a + frac * b) / den);
		}
	}

	/* Shift le tail à droite (insertion d'1 frame entre M+K-1 et M+K) */
	memmove(&buf[(M + ASRC_K + 1) * UAC2_CH],
	        &buf[(M + ASRC_K)     * UAC2_CH],
	        (n - M - ASRC_K) * UAC2_CH * sizeof(int32_t));
	/* Écrit tmp à la position M */
	memcpy(&buf[M * UAC2_CH], tmp,
	       (ASRC_K + 1) * UAC2_CH * sizeof(int32_t));
	return n + 1;
}

/* V8.6 — Un seul shift_ppm partagé (piloté par cap events seulement).
 * Sign inversé pour play : si shift>0 (host fast) → cap drop, play insert. */
static int compute_correction(int *samples_acc, int is_play)
{
	if (atomic_load(&g_no_asrc)) return 0;   /* V8.8 — bypass complet */
	int shift = atomic_load(&g_shift_ppm);
	if (shift == 0) return 0;
	int interval = 1000000 / (shift > 0 ? shift : -shift);
	if (*samples_acc < interval) return 0;
	*samples_acc -= interval;
	if (is_play)
		return (shift > 0) ? -1 : +1;   /* play : insert si shift>0 */
	else
		return (shift > 0) ? +1 : -1;   /* cap  : drop   si shift>0 */
}

/* ============================== V8.1 UAC2 ISOLATION =================
 *
 * Ring SPSC dédié pour chaque direction UAC2 (cap + play), alimenté par
 * 1 thread RT dédié qui own le PCM en BLOCKING. Découple totalement
 * l'USB UAC2 du chemin DSP : un blocage USB (msleep tac5212_trigger
 * sur cap, host PipeWire suspend sur play, unplug...) reste confiné
 * dans son thread, n'affecte pas le DSP.
 *
 * Capacité : 8 periods × 96 frames × 8 ch × 4 B = 24 KB par direction,
 * soit 16 ms de tolérance jitter avant drop. Indices SPSC atomic 32-bit,
 * acquire/release ordering (pattern déjà éprouvé sur ring DSP).
 *
 * audio_thread voit le UAC2 comme une simple lecture "always latest period"
 * (silence si pas de samples prêts) — il n'attend plus jamais sur USB.
 * Idem pour play : push best-effort, drop si ring full.
 */
#define UAC2_CH            8   /* = N_INPUT_STEMS = N_OUTPUT_UAC2 */

typedef struct {
	int32_t      buf[UAC2_RING_FRAMES * UAC2_CH];
	atomic_uint  wr;
	atomic_uint  rd;
	atomic_ulong drops;       /* nb de FRAMES droppées (cumul) */
	atomic_ulong drops_evt;   /* nb d'ÉVÉNEMENTS push-full (cumul) */
	atomic_ulong empty_evt;   /* nb d'ÉVÉNEMENTS pop-empty (avail<requested) */
	atomic_ulong xruns;
} uac2_ring_t;

static uac2_ring_t g_ring_uac2_cap;
static uac2_ring_t g_ring_uac2_play;

/* Pop N frames du ring, ou silence si moins disponibles. Retourne nb pop. */
static int uac2_ring_pop_n(uac2_ring_t *r, int32_t *out, int n)
{
	unsigned wi = atomic_load_explicit(&r->wr, memory_order_acquire);
	unsigned ri = atomic_load_explicit(&r->rd, memory_order_relaxed);
	unsigned avail = wi - ri;
	if ((int)avail < n) {
		atomic_fetch_add(&r->empty_evt, 1);   /* event pop-empty */
		/* Sous-flow : silence pour combler */
		memset(out, 0, n * UAC2_CH * sizeof(int32_t));
		if (avail == 0) return 0;
		/* Copie ce qu'on a, pad le reste avec zeros */
		for (unsigned f = 0; f < avail; f++) {
			unsigned slot = (ri + f) % UAC2_RING_FRAMES;
			memcpy(&out[f * UAC2_CH], &r->buf[slot * UAC2_CH],
			       UAC2_CH * sizeof(int32_t));
		}
		atomic_store_explicit(&r->rd, ri + avail, memory_order_release);
		return (int)avail;
	}
	for (int f = 0; f < n; f++) {
		unsigned slot = (ri + f) % UAC2_RING_FRAMES;
		memcpy(&out[f * UAC2_CH], &r->buf[slot * UAC2_CH],
		       UAC2_CH * sizeof(int32_t));
	}
	atomic_store_explicit(&r->rd, ri + (unsigned)n, memory_order_release);
	return n;
}

/* Pop 1 period dans `out`. Renvoie 1 si succès, 0 si ring vide (out zeroed).
 * V8.22 : pre-fill — au démarrage, on retourne des zéros tant que le ring
 * n'a pas atteint UAC2_FILL_TARGET (192). Une fois armé, on pop normalement. */
static int uac2_ring_pop_period(uac2_ring_t *r, int32_t *out)
{
	unsigned wi = atomic_load_explicit(&r->wr, memory_order_acquire);
	unsigned ri = atomic_load_explicit(&r->rd, memory_order_relaxed);
	unsigned avail = wi - ri;   /* unsigned wrap OK */

	/* V8.22 — pre-fill : si ring CAP, attendre fill ≥ TARGET avant de pop. */
	if (r == &g_ring_uac2_cap &&
	    !atomic_load_explicit(&g_uac2_cap_warm, memory_order_relaxed)) {
		if (avail < UAC2_FILL_TARGET) {
			atomic_fetch_add(&r->empty_evt, 1);
			memset(out, 0, PERIOD_FRAMES * UAC2_CH * sizeof(int32_t));
			return 0;
		}
		atomic_store_explicit(&g_uac2_cap_warm, 1, memory_order_relaxed);
	}

	if (avail < PERIOD_FRAMES) {
		atomic_fetch_add(&r->empty_evt, 1);   /* event pop-empty */
		memset(out, 0, PERIOD_FRAMES * UAC2_CH * sizeof(int32_t));
		return 0;
	}

	for (unsigned f = 0; f < PERIOD_FRAMES; f++) {
		unsigned slot = (ri + f) % UAC2_RING_FRAMES;
		memcpy(&out[f * UAC2_CH], &r->buf[slot * UAC2_CH],
		       UAC2_CH * sizeof(int32_t));
	}
	/* V8.32 — bucket courant + min/max global persistants */
	if (r == &g_ring_uac2_cap) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (g_last_rd_ts.tv_sec != 0) {
			uint64_t dt_us =
			    (uint64_t)(now.tv_sec - g_last_rd_ts.tv_sec) * 1000000ULL +
			    (uint64_t)(now.tv_nsec - g_last_rd_ts.tv_nsec) / 1000ULL;
			if (dt_us > 0 && dt_us <= 100000) {
				uint64_t sec = (uint64_t)now.tv_sec;
				int b = (int)(sec % TIMING_WINDOW_SEC);
				if (atomic_load_explicit(&g_rd_bucket_epoch[b],
				    memory_order_relaxed) != sec) {
					atomic_store_explicit(&g_rd_bucket_sum[b], 0, memory_order_relaxed);
					atomic_store_explicit(&g_rd_bucket_cnt[b], 0, memory_order_relaxed);
					atomic_store_explicit(&g_rd_bucket_epoch[b], sec, memory_order_relaxed);
				}
				atomic_fetch_add_explicit(&g_rd_bucket_sum[b], dt_us, memory_order_relaxed);
				atomic_fetch_add_explicit(&g_rd_bucket_cnt[b], 1, memory_order_relaxed);
				uint32_t cur_min = atomic_load_explicit(&g_rd_min_us, memory_order_relaxed);
				if ((uint32_t)dt_us < cur_min)
					atomic_store_explicit(&g_rd_min_us, (uint32_t)dt_us, memory_order_relaxed);
				uint32_t cur_max = atomic_load_explicit(&g_rd_max_us, memory_order_relaxed);
				if ((uint32_t)dt_us > cur_max)
					atomic_store_explicit(&g_rd_max_us, (uint32_t)dt_us, memory_order_relaxed);
			}
		}
		g_last_rd_ts = now;
	}
	atomic_store_explicit(&r->rd, ri + PERIOD_FRAMES,
			      memory_order_release);
	return 1;
}

/* Push n frames depuis `in`. Si ring full, drop les NOUVEAUX samples qui
 * ne tiennent pas (au lieu d'avancer rd côté producteur, ce qui violait
 * le contrat SPSC et causait des race conditions avec le consumer).
 * V8.9 : producer NE TOUCHE PLUS rd.
 * V8.29 : NON-UTILISÉE pour cap (cf uac2_ring_push_period_atomic). */
static void uac2_ring_push_n(uac2_ring_t *r, const int32_t *in, int n)
{
	unsigned wi = atomic_load_explicit(&r->wr, memory_order_relaxed);
	unsigned ri = atomic_load_explicit(&r->rd, memory_order_acquire);
	unsigned used = wi - ri;
	unsigned un = (unsigned)n;
	unsigned free_space = UAC2_RING_FRAMES - used;

	if (un > free_space) {
		/* Ring full → on jette les nouveaux samples qui débordent.
		 * Le consumer reste protégé : il continue de lire les anciens. */
		atomic_fetch_add(&r->drops, un - free_space);
		atomic_fetch_add(&r->drops_evt, 1);
		un = free_space;
	}
	if (un == 0)
		return;

	for (unsigned f = 0; f < un; f++) {
		unsigned slot = (wi + f) % UAC2_RING_FRAMES;
		memcpy(&r->buf[slot * UAC2_CH], &in[f * UAC2_CH],
		       UAC2_CH * sizeof(int32_t));
	}
	atomic_store_explicit(&r->wr, wi + un, memory_order_release);
}

/* Push 1 period (96 frames). Wrapper sur uac2_ring_push_n pour audio_thread. */
static void uac2_ring_push_period(uac2_ring_t *r, const int32_t *in)
{
	uac2_ring_push_n(r, in, PERIOD_FRAMES);
}

/* V8.29 — Push ATOMIQUE de 1 period (96 frames). Retourne 1 si push OK,
 * 0 si ring plein (free_space < 96). Dans ce cas, RIEN n'est écrit, wr
 * ne bouge pas, et drops_evt s'incrémente pour comptage. Le caller doit
 * réessayer plus tard avec les MÊMES samples (pas de troncature). */
static int uac2_ring_try_push_period(uac2_ring_t *r, const int32_t *in)
{
	unsigned wi = atomic_load_explicit(&r->wr, memory_order_relaxed);
	unsigned ri = atomic_load_explicit(&r->rd, memory_order_acquire);
	unsigned used = wi - ri;
	unsigned free_space = UAC2_RING_FRAMES - used;

	if (free_space < PERIOD_FRAMES) {
		atomic_fetch_add(&r->drops_evt, 1);
		return 0;
	}
	for (unsigned f = 0; f < PERIOD_FRAMES; f++) {
		unsigned slot = (wi + f) % UAC2_RING_FRAMES;
		memcpy(&r->buf[slot * UAC2_CH], &in[f * UAC2_CH],
		       UAC2_CH * sizeof(int32_t));
	}
	/* V8.32 — Timing : bucket courant (mod TIMING_WINDOW_SEC), update sum+cnt.
	 * Skip si dt > 100 ms (recover anormal). Min/max globaux persistants. */
	if (r == &g_ring_uac2_cap) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (g_last_wr_ts.tv_sec != 0) {
			uint64_t dt_us =
			    (uint64_t)(now.tv_sec - g_last_wr_ts.tv_sec) * 1000000ULL +
			    (uint64_t)(now.tv_nsec - g_last_wr_ts.tv_nsec) / 1000ULL;
			if (dt_us > 0 && dt_us <= 100000) {
				uint64_t sec = (uint64_t)now.tv_sec;
				int b = (int)(sec % TIMING_WINDOW_SEC);
				if (atomic_load_explicit(&g_wr_bucket_epoch[b],
				    memory_order_relaxed) != sec) {
					atomic_store_explicit(&g_wr_bucket_sum[b], 0, memory_order_relaxed);
					atomic_store_explicit(&g_wr_bucket_cnt[b], 0, memory_order_relaxed);
					atomic_store_explicit(&g_wr_bucket_epoch[b], sec, memory_order_relaxed);
				}
				atomic_fetch_add_explicit(&g_wr_bucket_sum[b], dt_us, memory_order_relaxed);
				atomic_fetch_add_explicit(&g_wr_bucket_cnt[b], 1, memory_order_relaxed);
				uint32_t cur_min = atomic_load_explicit(&g_wr_min_us, memory_order_relaxed);
				if ((uint32_t)dt_us < cur_min)
					atomic_store_explicit(&g_wr_min_us, (uint32_t)dt_us, memory_order_relaxed);
				uint32_t cur_max = atomic_load_explicit(&g_wr_max_us, memory_order_relaxed);
				if ((uint32_t)dt_us > cur_max)
					atomic_store_explicit(&g_wr_max_us, (uint32_t)dt_us, memory_order_relaxed);
			}
		}
		g_last_wr_ts = now;
	}
	atomic_store_explicit(&r->wr, wi + PERIOD_FRAMES, memory_order_release);
	return 1;
}

/* Renvoie le nb de frames actuellement dans le ring (utilisé pour
 * backpressure côté producteur). Lecture relaxed des deux indices :
 * le résultat est conservatif (un peu sous-estimé) ce qui est OK
 * pour décider d'attendre. */
static unsigned uac2_ring_fill(uac2_ring_t *r)
{
	unsigned wi = atomic_load_explicit(&r->wr, memory_order_relaxed);
	unsigned ri = atomic_load_explicit(&r->rd, memory_order_relaxed);
	return wi - ri;   /* unsigned wrap OK */
}

/* V8.6 — shift_ppm piloté uniquement par les events du ring CAP.
 * play full/empty restent comptés mais n'affectent plus shift, car le play
 * USB peut être non consommé (Bitwig ouvre cap sans ouvrir play). */
static void *shift_controller_thread(void *arg)
{
	(void)arg;
	unsigned long last_cap_full  = atomic_load(&g_ring_uac2_cap.drops_evt);
	unsigned long last_cap_empty = atomic_load(&g_ring_uac2_cap.empty_evt);
	mlog("shift_controller_thread : tick 100 ms, source = cap events only");

	while (atomic_load(&g_st.running)) {
		usleep(100000);   /* 100 ms */

		/* V8.18 — mode test : shift figé par --fixed-shift, le contrôleur
		 * ne touche plus à g_shift_ppm. */
		if (atomic_load(&g_shift_fixed))
			continue;

		unsigned long cap_full  = atomic_load(&g_ring_uac2_cap.drops_evt);
		unsigned long cap_empty = atomic_load(&g_ring_uac2_cap.empty_evt);

		/* V8.11 — delta complet (pas boolean) pour capturer les trains
		 * d'events. 50 empty en 100 ms = shift -= 50 (vs -1 avant).
		 * V8.12 — si compteur a régressé (reset_drift_stats), on
		 * resynchronise last_* sans appliquer de delta. */
		int delta = 0;
		if (cap_full  >= last_cap_full)
			delta += (int)(cap_full  - last_cap_full);
		if (cap_empty >= last_cap_empty)
			delta -= (int)(cap_empty - last_cap_empty);

		last_cap_full  = cap_full;
		last_cap_empty = cap_empty;

		if (delta != 0) {
			int cur = atomic_load(&g_shift_ppm);
			atomic_store(&g_shift_ppm, cur + delta);
		}
	}
	mlog("shift_controller_thread exiting");
	return NULL;
}

/* V8.13 — Thread cap UAC2 : NONBLOCK, lit ce qui est dispo, push variable
 * N au ring avec correction ASRC quand besoin. Plus de readi(96) imposé
 * — on suit le pace naturel des bursts USB iso. */
static void *cap_uac2_thread(void *arg)
{
	(void)arg;
	struct sched_param sp = { .sched_priority = RT_PRIO_UAC2_CAP };
	(void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
	/* V9.0 — pin sur core 3 (cap+play UAC2 partagent, séparés du DSP path) */
	{
		cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(CPU_UAC2_CAP, &cs);
		pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
	}
	mlog("cap_uac2_thread : SCHED_FIFO prio %d core %d (NONBLOCK)",
	     RT_PRIO_UAC2_CAP, CPU_UAC2_CAP);

	/* V8.20 — Buffer accumulateur : on push TOUJOURS par bloc EXACT de
	 * PERIOD_FRAMES (96). readi peut retourner N variable (typique 50-100) ;
	 * on accumule jusqu'à pouvoir push 1 period entière. Capacité = 4 periods
	 * + 1 frame de marge pour insert. */
	int32_t buf_acc[(BUFFER_FRAMES + 1) * UAC2_CH];
	int acc_n = 0;

	/* Start en BLOCKING (sync initiale), puis passe en NONBLOCK */
	snd_pcm_nonblock(g_st.cap_uac2.pcm, 0);
	while (atomic_load(&g_st.running)) {
		int err = snd_pcm_start(g_st.cap_uac2.pcm);
		if (err == 0 || err == -EBADFD) break;
		mlog("cap_uac2_thread: start retry: %s", snd_strerror(err));
		snd_pcm_recover(g_st.cap_uac2.pcm, err, 1);
		usleep(100000);
	}

	/* V8.26 — Mesure drift précise via HW htstamp.
	 *   drift_samples = appl_ptr cumulé (somme des r returned par readi)
	 *   drift_hw_pos_0 = hw_pos au dernier snapshot
	 *   drift_t0 = htstamp au dernier snapshot (audio clock)
	 * Init drift_t0.tv_sec=0 → premier snapshot servira de référence. */
	struct timespec drift_t0 = { 0 };
	uint64_t drift_samples = 0;
	uint64_t drift_hw_pos_0 = 0;
	float drift_ppm_ema = 0.0f;
	int corr_samples_acc = 0;
	/* V8.30 — log periodic des stats timing */
	struct timespec last_log_ts = { 0 };

	snd_pcm_nonblock(g_st.cap_uac2.pcm, 1);

	while (atomic_load(&g_st.running)) {
		snd_pcm_sframes_t avail = snd_pcm_avail_update(g_st.cap_uac2.pcm);
		if (avail < 0 && avail != -EAGAIN) {
			atomic_fetch_add(&g_ring_uac2_cap.xruns, 1);
			snd_pcm_recover(g_st.cap_uac2.pcm, (int)avail, 1);
			snd_pcm_start(g_st.cap_uac2.pcm);
			drift_t0.tv_sec = 0; drift_t0.tv_nsec = 0; drift_hw_pos_0 = 0; g_last_wr_ts.tv_sec = 0;
			drift_samples = 0;
			acc_n = 0;
			usleep(200);
			continue;
		}
		if (avail <= 0) {
			usleep(200);
			goto cap_drift_calc;
		}

		/* Lire dans le slot libre du buffer accumulateur */
		int space = BUFFER_FRAMES - acc_n;
		int n_to_read = (int)avail < space ? (int)avail : space;
		if (n_to_read <= 0) goto cap_drift_calc; /* acc plein, attendre push */

		snd_pcm_sframes_t r = snd_pcm_readi(g_st.cap_uac2.pcm,
		    buf_acc + acc_n * UAC2_CH, n_to_read);
		if (r < 0 && r != -EAGAIN) {
			atomic_fetch_add(&g_ring_uac2_cap.xruns, 1);
			snd_pcm_recover(g_st.cap_uac2.pcm, r, 1);
			snd_pcm_start(g_st.cap_uac2.pcm);
			drift_t0.tv_sec = 0; drift_t0.tv_nsec = 0; drift_hw_pos_0 = 0; g_last_wr_ts.tv_sec = 0;
			drift_samples = 0;
			acc_n = 0;
			continue;
		}
		if (r <= 0) goto cap_drift_calc;
		acc_n += (int)r;
		drift_samples += (uint64_t)r;

		/* V8.26 — ASRC piloté par drift mesuré via htstamp. Correction ±2
		 * samples par push quand shift_ppm requiert. Push par bloc 96. */
		while (acc_n >= PERIOD_FRAMES + 2) {
			int correction = compute_correction(&corr_samples_acc, 0);
			int input_n = PERIOD_FRAMES + (correction * 2); /* 94, 96 ou 98 */
			corr_samples_acc += input_n;
			int diff = input_n - PERIOD_FRAMES;
			int32_t period_buf[PERIOD_FRAMES * UAC2_CH];
			atomic_store_explicit(&g_uac2_cap_mode,
			                      (correction == -1) ? 1 :
			                      (correction == +1) ? 2 : 0,
			                      memory_order_relaxed);

			if (diff < 0) {
				/* Insert |diff| samples (1 toutes les N positions) :
				 *   |diff|=2 → 1 insert toutes 48 outputs, pos 23, 71? Non : on
				 *     prend l'algo manuel à pos 92/94 (= raccord clean V8.22).
				 *   |diff|=6 → 1 insert tous les 16 outputs.
				 *
				 * Pour rester cohérent avec V8.22 (raccord clean), on traite
				 * les 2 cas séparément. */
				if (diff == -2) {
					/* Insert 94 → 96 (raccord clean : out[95] = in[93]) */
					memcpy(period_buf, buf_acc,
					       92 * UAC2_CH * sizeof(int32_t));
					for (int ch = 0; ch < UAC2_CH; ch++) {
						int64_t a = buf_acc[91 * UAC2_CH + ch];
						int64_t b = buf_acc[92 * UAC2_CH + ch];
						period_buf[92 * UAC2_CH + ch] = (int32_t)((a + b) / 2);
					}
					memcpy(period_buf + 93 * UAC2_CH,
					       buf_acc + 92 * UAC2_CH,
					       UAC2_CH * sizeof(int32_t));
					for (int ch = 0; ch < UAC2_CH; ch++) {
						int64_t a = buf_acc[92 * UAC2_CH + ch];
						int64_t b = buf_acc[93 * UAC2_CH + ch];
						period_buf[94 * UAC2_CH + ch] = (int32_t)((a + b) / 2);
					}
					memcpy(period_buf + 95 * UAC2_CH,
					       buf_acc + 93 * UAC2_CH,
					       UAC2_CH * sizeof(int32_t));
				} else {
					/* diff = -6 : Insert 90 → 96. 6 inserts répartis aux
					 * positions output 14, 29, 44, 59, 74, 89 (espacement 15).
					 * out[95] = in[89] = raccord clean. */
					int in_idx = 0;
					int inserted = 0;
					int insert_positions[6] = {14, 29, 44, 59, 74, 89};
					int next_ip = 0;
					for (int out_idx = 0; out_idx < PERIOD_FRAMES; out_idx++) {
						if (next_ip < 6 && out_idx == insert_positions[next_ip]) {
							for (int ch = 0; ch < UAC2_CH; ch++) {
								int64_t a = buf_acc[(in_idx - 1) * UAC2_CH + ch];
								int64_t b = buf_acc[in_idx * UAC2_CH + ch];
								period_buf[out_idx * UAC2_CH + ch] =
								    (int32_t)((a + b) / 2);
							}
							inserted++;
							next_ip++;
						} else {
							memcpy(period_buf + out_idx * UAC2_CH,
							       buf_acc + in_idx * UAC2_CH,
							       UAC2_CH * sizeof(int32_t));
							in_idx++;
						}
					}
				}
				atomic_fetch_add(&g_dbg_corr_req_insert, 1);
				atomic_fetch_add(&g_dbg_corr_app_insert, 1);
			} else if (diff > 0) {
				if (diff == +2) {
					/* Drop 98 → 96 (fusion 3-en-1, raccord clean) */
					memcpy(period_buf, buf_acc,
					       92 * UAC2_CH * sizeof(int32_t));
					for (int ch = 0; ch < UAC2_CH; ch++) {
						int64_t a = buf_acc[92 * UAC2_CH + ch];
						int64_t b = buf_acc[93 * UAC2_CH + ch];
						int64_t c = buf_acc[94 * UAC2_CH + ch];
						period_buf[92 * UAC2_CH + ch] = (int32_t)((a + b + c) / 3);
					}
					memcpy(period_buf + 93 * UAC2_CH,
					       buf_acc + 95 * UAC2_CH,
					       3 * UAC2_CH * sizeof(int32_t));
				} else {
					/* diff = +6 : Drop 102 → 96. 6 fusions 2-en-1 aux positions
					 * output 14, 30, 46, 62, 78, 94 (espacement 16). */
					int in_idx = 0;
					int drop_positions[6] = {14, 30, 46, 62, 78, 94};
					int next_dp = 0;
					for (int out_idx = 0; out_idx < PERIOD_FRAMES; out_idx++) {
						if (next_dp < 6 && out_idx == drop_positions[next_dp]) {
							for (int ch = 0; ch < UAC2_CH; ch++) {
								int64_t a = buf_acc[in_idx * UAC2_CH + ch];
								int64_t b = buf_acc[(in_idx + 1) * UAC2_CH + ch];
								period_buf[out_idx * UAC2_CH + ch] =
								    (int32_t)((a + b) / 2);
							}
							in_idx += 2;
							next_dp++;
						} else {
							memcpy(period_buf + out_idx * UAC2_CH,
							       buf_acc + in_idx * UAC2_CH,
							       UAC2_CH * sizeof(int32_t));
							in_idx++;
						}
					}
				}
				atomic_fetch_add(&g_dbg_corr_req_drop, 1);
				atomic_fetch_add(&g_dbg_corr_app_drop, 1);
			} else {
				memcpy(period_buf, buf_acc,
				       PERIOD_FRAMES * UAC2_CH * sizeof(int32_t));
			}

			/* Histo input_n consommé */
			if (input_n < 10) atomic_fetch_add(&g_dbg_readi_lt10, 1);
			else if (input_n < 50) atomic_fetch_add(&g_dbg_readi_10_50, 1);
			else if (input_n < 100) atomic_fetch_add(&g_dbg_readi_50_100, 1);
			else atomic_fetch_add(&g_dbg_readi_ge100, 1);

			/* V8.29 — Push atomique de 96 frames. Si ring plein, on
			 * SORT du while interne SANS shift acc, et on retentera au
			 * prochain readi. Pas de troncature, pas de perte. */
			if (!uac2_ring_try_push_period(&g_ring_uac2_cap,
			                               period_buf))
				break;
			atomic_fetch_add(&g_dbg_cc_called, 1);

			/* Shift acc : on a consommé input_n frames */
			if (acc_n - input_n > 0) {
				memmove(buf_acc,
				        buf_acc + input_n * UAC2_CH,
				        (acc_n - input_n) * UAC2_CH *
				        sizeof(int32_t));
			}
			acc_n -= input_n;
		}

	cap_drift_calc:
		/* V8.26 — Mesure drift via CLOCK_MONOTONIC + appl_ptr + avail.
		 *   hw_pos = drift_samples (appl_ptr cumulé) + avail courant
		 *          = total frames livrés par USB host depuis start
		 * (f_uac2 gadget ne supporte pas audio_htstamp HW.) */
		{
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			snd_pcm_sframes_t avail_now = snd_pcm_avail(g_st.cap_uac2.pcm);
			if (avail_now < 0) avail_now = 0;
			uint64_t hw_pos_now = drift_samples + (uint64_t)avail_now;

			if (drift_t0.tv_sec == 0) {
				drift_t0 = now;
				drift_hw_pos_0 = hw_pos_now;
			} else {
				double dt_sec =
				    (double)(now.tv_sec  - drift_t0.tv_sec) +
				    (double)(now.tv_nsec - drift_t0.tv_nsec) / 1e9;
				if (dt_sec >= 10.0) {
					uint64_t df = hw_pos_now - drift_hw_pos_0;
					double rate = (double)df / dt_sec;
					double ppm  = (rate - (double)SAMPLE_RATE)
					              / (double)SAMPLE_RATE * 1e6;
					/* V8.27 — Garde-fou rate dans ±1%. */
					int rate_ok = (rate > SAMPLE_RATE * 0.99 &&
					               rate < SAMPLE_RATE * 1.01);
					/* V8.28 — clamp drift mesuré à ±500 ppm (drift physique
					 * 2 quartz commerciaux ≤ 200 ppm en pratique). */
					int ppm_ok = (ppm > -500.0 && ppm < 500.0);
					if (rate_ok && ppm_ok) {
						if (atomic_load(&g_usb_drift_valid)) {
							drift_ppm_ema = 0.9f * drift_ppm_ema +
							                0.1f * (float)ppm;
						} else {
							drift_ppm_ema = (float)ppm;
						}
						atomic_store(&g_usb_drift_ppm_x100,
						             (int)(drift_ppm_ema * 100.0f));
						atomic_store(&g_usb_drift_valid, 1);
						if (!atomic_load(&g_shift_fixed)) {
							int new_shift = (int)(drift_ppm_ema +
							    (drift_ppm_ema >= 0 ? 0.5f : -0.5f));
							/* Clamp final à ±500 ppm */
							if (new_shift > 500) new_shift = 500;
							if (new_shift < -500) new_shift = -500;
							atomic_store(&g_shift_ppm, new_shift);
						}
					} else {
						/* Mesure aberrante : ne pas update EMA, garder shift
						 * à sa valeur actuelle (ne pas reset à 0 pour ne pas
						 * faire osciller l'ASRC). */
						atomic_store(&g_usb_drift_valid, 0);
					}
					drift_t0 = now;
					drift_hw_pos_0 = hw_pos_now;
				}
			}
		}
		/* V8.32 — log periodic toutes les 5 sec : min/max globaux + avg 10s */
		{
			struct timespec n2;
			clock_gettime(CLOCK_MONOTONIC, &n2);
			if (n2.tv_sec - last_log_ts.tv_sec >= 5) {
				uint64_t sec = (uint64_t)n2.tv_sec;
				uint64_t wr_sm = 0, rd_sm = 0;
				uint32_t wr_n2 = 0, rd_n2 = 0;
				for (int k = 0; k < TIMING_WINDOW_SEC; k++) {
					uint64_t e = atomic_load_explicit(&g_wr_bucket_epoch[k], memory_order_relaxed);
					if (e != 0 && sec - e < TIMING_WINDOW_SEC) {
						wr_sm += atomic_load_explicit(&g_wr_bucket_sum[k], memory_order_relaxed);
						wr_n2 += atomic_load_explicit(&g_wr_bucket_cnt[k], memory_order_relaxed);
					}
					e = atomic_load_explicit(&g_rd_bucket_epoch[k], memory_order_relaxed);
					if (e != 0 && sec - e < TIMING_WINDOW_SEC) {
						rd_sm += atomic_load_explicit(&g_rd_bucket_sum[k], memory_order_relaxed);
						rd_n2 += atomic_load_explicit(&g_rd_bucket_cnt[k], memory_order_relaxed);
					}
				}
				uint32_t wr_mn = atomic_load(&g_wr_min_us);
				uint32_t wr_mx = atomic_load(&g_wr_max_us);
				uint32_t rd_mn = atomic_load(&g_rd_min_us);
				uint32_t rd_mx = atomic_load(&g_rd_max_us);
				if (wr_mn == UINT32_MAX) wr_mn = 0;
				if (rd_mn == UINT32_MAX) rd_mn = 0;
				unsigned fill = uac2_ring_fill(&g_ring_uac2_cap);
				unsigned long cee = atomic_load(&g_ring_uac2_cap.empty_evt);
				unsigned long cfe = atomic_load(&g_ring_uac2_cap.drops_evt);
				mlog("TIMING wr[push] min=%u max=%u avg10s=%u us | rd[pop] min=%u max=%u avg10s=%u us | fill=%u cap_empty=%lu cap_full=%lu",
				     wr_mn, wr_mx, wr_n2 ? (uint32_t)(wr_sm/wr_n2) : 0,
				     rd_mn, rd_mx, rd_n2 ? (uint32_t)(rd_sm/rd_n2) : 0,
				     fill, cee, cfe);
				last_log_ts = n2;
			}
		}
	}
	mlog("cap_uac2_thread exiting");
	return NULL;
}

/* V8.22 — Thread play UAC2 : régulation symétrique par fill du ring play.
 * Producer = audio_thread (push 96 fixe). Consumer = ce thread (pop input_n
 * variable du ring, writei 96 fixe à USB).
 *   - fill ≤ 0   → SLOWING (pop 94, write 96 avec 2 inserts) → ring vide moins
 *   - fill ≥ 384 → SPEEDING (pop 98, write 96 avec 2 drops)  → ring vide plus
 *   - sortie à fill = 192 (TARGET)
 * Pre-fill : tant que ring play < TARGET (192), on writei zéros à USB
 * (sinon underrun USB côté host). */
static void *play_uac2_thread(void *arg)
{
	(void)arg;
	struct sched_param sp = { .sched_priority = RT_PRIO_UAC2_PLAY };
	(void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
	/* V9.0 — pin sur core 3 (même core que cap_uac2_thread) */
	{
		cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(CPU_UAC2_PLAY, &cs);
		pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
	}
	mlog("play_uac2_thread : SCHED_FIFO prio %d core %d (V8.22 fill-based)",
	     RT_PRIO_UAC2_PLAY, CPU_UAC2_PLAY);

	int32_t pop_buf[(PERIOD_FRAMES + 6) * UAC2_CH]; /* V8.24 max input_n = 102 */
	int32_t period_buf[PERIOD_FRAMES * UAC2_CH];

	/* Prefill USB output : 3 periods de silence (BLOCKING). */
	snd_pcm_nonblock(g_st.play_uac2.pcm, 0);
	memset(period_buf, 0, sizeof(period_buf));
	for (int prime = 0; prime < N_PERIODS - 1; prime++) {
		snd_pcm_sframes_t r = snd_pcm_writei(g_st.play_uac2.pcm,
						     period_buf, PERIOD_FRAMES);
		if (r < 0) snd_pcm_recover(g_st.play_uac2.pcm, r, 1);
	}
	snd_pcm_nonblock(g_st.play_uac2.pcm, 1);

	while (atomic_load(&g_st.running)) {
		snd_pcm_sframes_t pavail =
		    snd_pcm_avail_update(g_st.play_uac2.pcm);
		if (pavail < 0) {
			atomic_fetch_add(&g_ring_uac2_play.xruns, 1);
			snd_pcm_recover(g_st.play_uac2.pcm,
			                (int)pavail, 1);
			usleep(200);
			continue;
		}
		if (pavail < PERIOD_FRAMES) {
			usleep(200);
			continue;
		}

		unsigned fill = uac2_ring_fill(&g_ring_uac2_play);

		/* Pre-fill : tant que ring play n'a pas atteint TARGET,
		 * on writei zéros à USB pour ne pas underrun le host. */
		if (!atomic_load_explicit(&g_uac2_play_warm,
		                          memory_order_relaxed)) {
			if (fill < UAC2_FILL_TARGET) {
				memset(period_buf, 0, sizeof(period_buf));
				snd_pcm_writei(g_st.play_uac2.pcm,
				               period_buf, PERIOD_FRAMES);
				atomic_fetch_add(&g_ring_uac2_play.empty_evt, 1);
				continue;
			}
			atomic_store_explicit(&g_uac2_play_warm, 1,
			                      memory_order_relaxed);
		}

		/* V8.25 — Sans correction. pop/write 96 fixe. */
		int input_n = PERIOD_FRAMES;
		atomic_store_explicit(&g_uac2_play_mode, 0,
		                      memory_order_relaxed);

		/* Si ring contient moins que input_n demandé, pop ce qu'il y a,
		 * pad zéros le reste. */
		int n = uac2_ring_pop_n(&g_ring_uac2_play, pop_buf, input_n);
		if (n < input_n) {
			memset(pop_buf + n * UAC2_CH, 0,
			       (input_n - n) * UAC2_CH * sizeof(int32_t));
		}

		/* V8.24 — Build period_buf 96 frames depuis input_n (5 cas) */
		int diff = input_n - PERIOD_FRAMES;
		if (diff == -2) {
			/* Insert 94 → 96 (raccord clean) */
			memcpy(period_buf, pop_buf,
			       92 * UAC2_CH * sizeof(int32_t));
			for (int ch = 0; ch < UAC2_CH; ch++) {
				int64_t a = pop_buf[91 * UAC2_CH + ch];
				int64_t b = pop_buf[92 * UAC2_CH + ch];
				period_buf[92 * UAC2_CH + ch] = (int32_t)((a + b) / 2);
			}
			memcpy(period_buf + 93 * UAC2_CH, pop_buf + 92 * UAC2_CH,
			       UAC2_CH * sizeof(int32_t));
			for (int ch = 0; ch < UAC2_CH; ch++) {
				int64_t a = pop_buf[92 * UAC2_CH + ch];
				int64_t b = pop_buf[93 * UAC2_CH + ch];
				period_buf[94 * UAC2_CH + ch] = (int32_t)((a + b) / 2);
			}
			memcpy(period_buf + 95 * UAC2_CH, pop_buf + 93 * UAC2_CH,
			       UAC2_CH * sizeof(int32_t));
		} else if (diff == -6) {
			/* Insert 90 → 96 : 6 inserts aux positions 14, 29, 44, 59, 74, 89 */
			int in_idx = 0;
			int insert_positions[6] = {14, 29, 44, 59, 74, 89};
			int next_ip = 0;
			for (int out_idx = 0; out_idx < PERIOD_FRAMES; out_idx++) {
				if (next_ip < 6 && out_idx == insert_positions[next_ip]) {
					for (int ch = 0; ch < UAC2_CH; ch++) {
						int64_t a = pop_buf[(in_idx - 1) * UAC2_CH + ch];
						int64_t b = pop_buf[in_idx * UAC2_CH + ch];
						period_buf[out_idx * UAC2_CH + ch] =
						    (int32_t)((a + b) / 2);
					}
					next_ip++;
				} else {
					memcpy(period_buf + out_idx * UAC2_CH,
					       pop_buf + in_idx * UAC2_CH,
					       UAC2_CH * sizeof(int32_t));
					in_idx++;
				}
			}
		} else if (diff == +2) {
			/* Drop 98 → 96 (fusion 3-en-1) */
			memcpy(period_buf, pop_buf,
			       92 * UAC2_CH * sizeof(int32_t));
			for (int ch = 0; ch < UAC2_CH; ch++) {
				int64_t a = pop_buf[92 * UAC2_CH + ch];
				int64_t b = pop_buf[93 * UAC2_CH + ch];
				int64_t c = pop_buf[94 * UAC2_CH + ch];
				period_buf[92 * UAC2_CH + ch] = (int32_t)((a + b + c) / 3);
			}
			memcpy(period_buf + 93 * UAC2_CH, pop_buf + 95 * UAC2_CH,
			       3 * UAC2_CH * sizeof(int32_t));
		} else if (diff == +6) {
			/* Drop 102 → 96 : 6 fusions 2-en-1 aux positions 14, 30, 46, 62, 78, 94 */
			int in_idx = 0;
			int drop_positions[6] = {14, 30, 46, 62, 78, 94};
			int next_dp = 0;
			for (int out_idx = 0; out_idx < PERIOD_FRAMES; out_idx++) {
				if (next_dp < 6 && out_idx == drop_positions[next_dp]) {
					for (int ch = 0; ch < UAC2_CH; ch++) {
						int64_t a = pop_buf[in_idx * UAC2_CH + ch];
						int64_t b = pop_buf[(in_idx + 1) * UAC2_CH + ch];
						period_buf[out_idx * UAC2_CH + ch] =
						    (int32_t)((a + b) / 2);
					}
					in_idx += 2;
					next_dp++;
				} else {
					memcpy(period_buf + out_idx * UAC2_CH,
					       pop_buf + in_idx * UAC2_CH,
					       UAC2_CH * sizeof(int32_t));
					in_idx++;
				}
			}
		} else {
			memcpy(period_buf, pop_buf,
			       PERIOD_FRAMES * UAC2_CH * sizeof(int32_t));
		}

		snd_pcm_sframes_t w = snd_pcm_writei(g_st.play_uac2.pcm,
		                                     period_buf, PERIOD_FRAMES);
		if (w < 0 && w != -EAGAIN) {
			atomic_fetch_add(&g_ring_uac2_play.xruns, 1);
			snd_pcm_recover(g_st.play_uac2.pcm, w, 1);
		}
	}
	mlog("play_uac2_thread exiting");
	return NULL;
}

/* E7.5 — analyzer taps. Visibility :
 *   - mixer-pro.c owns the storage (g_taps).
 *   - analyzer.c reads via the extern'd pointer + run flag.
 *   - control thread reads/writes the per-tap config and snapshots the
 *     analyzer output for the JSON wire (op:get_meters embeds analyzer[]).
 */
mixer_tap_t g_taps[N_TAPS];
mixer_tap_t *g_taps_for_analyzer = g_taps;
atomic_int   g_running_flag_for_analyzer;

/* Options command-line : skip une ou plusieurs paires PCMs (pratique en dev
 * quand le host PC USB est absent ou que l'aloop n'est pas chargée).
 * Quand un input est skipped, les samples correspondants sont à 0.
 * Quand un output est skipped, on n'écrit rien (la matrix master ignore
 * silencieusement les outputs concernés).
 */
static int g_skip_uac2  = 0;
static int g_skip_phone = 0;

/* V9.3.5 : persistence presets debounced. atomic flag, set par
 * set_fx_engine/set_fx_param. Thread écrit JSON 1s après dernière modif. */
static atomic_int g_presets_dirty = 0;
#define PRESETS_PATH "/var/lib/mixer-pro/presets.json"

/* V9.5.21 — remap des 8 mics DSP : in_block[i] = slot TDM g_mic_map[i].
 * Défaut identité (0..7). Corrige un ordre de slots/câblage TAC ≠ M1..M8. */
static atomic_int g_mic_map[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

/* V9.5.21 — gain de sortie par strip OUT (×1000, milli-linéaire). Trim final
 * appliqué après l'insert, avant interleave. Défaut 1000 (= ×1.0). Initialisé
 * dans main() (zero-init = silence sinon). */
static atomic_int g_out_gain_m[N_OUTPUT_TOTAL];
/* gain de sortie LISSÉ, écrit uniquement par l'audio_thread (critic dfeb668d :
 * appliquer la cible brute par pas de 0.5 dB = zipper noise audible).
 * alpha 1/16 par période 2 ms → tau ≈ 32 ms. */
static float g_out_gain_cur[N_OUTPUT_TOTAL];

/* V9.5.21b — copie de la spec insert (set_insert) pour persistance : la
 * chaîne mastering + le mode assistant + le routage étaient PERDUS à chaque
 * reboot (re-setup manuel). Protégée par g_st.target_lock (écrite dans le
 * handler set_insert, lue par save_mixer_state). */
static char g_insert_spec_engine[FX_CHAIN_MAX][32];
static char g_insert_spec_uri[FX_CHAIN_MAX][256];
static int  g_insert_spec_n = 0;

/* V9.4 — insert mastering : chaîne de N plugins sur out_0+out_1 DSP.
 * g_insert_active = 0 : bypass total, mix_block out directement vers convert.
 * g_insert_active = 1 : g_insert_chain.process_block sur out_block[0..1].
 * Init/swap protégé par target_lock (cohérent avec mix_block). */
static fx_engine_t g_insert_chain;
static atomic_int  g_insert_active = 0;
/* V13-SCENES : bypass runtime du mastering (chaîne conservée chaude) */
static atomic_int  g_insert_bypass = 0;

/* V13-SCENES : profils complets (définis après save_state_to) */
#define SCENE_SLOTS 6
#define SCENE_DIR   "/var/lib/mixer-pro/scenes"
static void save_state_to(const char *path);
static int  scene_apply(const char *path);
/* V9.5.12 — état Mixer Assistant (consommé par daemon mixer-ml-inference
 * via socket get_assistant). mixer-pro ne fait PAS d'inférence TFLite
 * (process séparé pour éviter conflit galcore + audio_thread RT99).
 *  - mode  : 0=passthrough, 1=mastering
 *  - source: 0=HW IN, 1=USB IN
 */
static _Atomic int g_assistant_mode   = 0;
static _Atomic int g_assistant_source = 0;
/* g_no_asrc déclaré plus haut près de g_shift_ppm */

/* Logging (mlog) + ALSA helpers (pcm_open, pcm_recover) + conversions
 * s32↔float : déplacés dans util.c/util.h (V14.0 étape 0). */

/* ============================== Mixer core ========================= */

/* ================= V12-SMP — sampleur (page PADS) =================
 * WAVs de /var/lib/ala/samples préchargés en RAM (control thread),
 * lus one-shot dans les tranches P1/P2 (in_block[16/17], mortes en
 * skip_phone). Publication sous target_lock ; libération DIFFÉRÉE des
 * anciens buffers (purge au reload suivant — jamais de free d'un
 * buffer potentiellement lu par l'audio). ARCHI_V12_SAMPLER.md. */
#define SMP_SLOTS 16
#define SMP_DIR "/var/lib/ala/samples"
#define SMP_MAX_TOTAL (256u * 1024u * 1024u)   /* plafond RAM (critic) */

struct smp_slot {
	char name[64];
	float *buf;              /* stéréo entrelacé LR, 48 kHz */
	uint32_t frames;
	_Atomic int playing;
	_Atomic uint32_t pos;
	float gain;
};
static struct smp_slot g_smp[SMP_SLOTS];
static float *g_smp_defer[SMP_SLOTS];
static int g_smp_defer_n;
static size_t g_smp_total;

/* Parseur WAV minimal : PCM 16/24/32 ou float32, mono→dup ou stéréo,
 * 48 kHz exigé. Retourne buffer float stéréo malloc'é (control thread). */
static float *smp_load_wav(const char *path, uint32_t *out_frames)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t h[12];
	if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) ||
	    memcmp(h + 8, "WAVE", 4)) {
		fclose(f);
		return NULL;
	}
	uint16_t fmt = 0, ch = 0, bits = 0;
	uint32_t rate = 0, data_len = 0;
	long data_off = -1;
	uint8_t ck[8];
	while (fread(ck, 1, 8, f) == 8) {
		uint32_t len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
		if (!memcmp(ck, "fmt ", 4)) {
			uint8_t b[16];
			if (len < 16 || fread(b, 1, 16, f) != 16)
				break;
			fmt  = b[0] | b[1] << 8;
			ch   = b[2] | b[3] << 8;
			rate = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
			bits = b[14] | b[15] << 8;
			if (len > 16)
				fseek(f, len - 16, SEEK_CUR);
		} else if (!memcmp(ck, "data", 4)) {
			data_off = ftell(f);
			data_len = len;
			fseek(f, (len + 1) & ~1u, SEEK_CUR);
		} else {
			fseek(f, (len + 1) & ~1u, SEEK_CUR);
		}
	}
	if (data_off < 0 || rate != 48000 || ch < 1 || ch > 2 ||
	    !((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) ||
	      (fmt == 3 && bits == 32))) {
		mlog("smp: %s rejeté (fmt=%u ch=%u rate=%u bits=%u — 48k PCM/f32 requis)",
		     path, fmt, ch, rate, bits);
		fclose(f);
		return NULL;
	}
	const uint32_t bpf = ch * bits / 8;
	uint32_t frames = data_len / bpf;
	if ((size_t)frames * 8 + g_smp_total > SMP_MAX_TOTAL) {
		mlog("smp: %s rejeté (plafond RAM %u Mo atteint)",
		     path, SMP_MAX_TOTAL >> 20);
		fclose(f);
		return NULL;
	}
	uint8_t *raw = malloc(data_len);
	float *out = malloc((size_t)frames * 2 * sizeof(float));
	if (!raw || !out) {
		free(raw); free(out); fclose(f);
		return NULL;
	}
	fseek(f, data_off, SEEK_SET);
	if (fread(raw, 1, data_len, f) != data_len) {
		free(raw); free(out); fclose(f);
		return NULL;
	}
	fclose(f);
	for (uint32_t i = 0; i < frames; i++) {
		float l = 0, r = 0;
		for (int c = 0; c < ch; c++) {
			const uint8_t *p = raw + (size_t)i * bpf + c * bits / 8;
			float v;
			if (fmt == 3) {
				memcpy(&v, p, 4);
			} else if (bits == 16) {
				v = (int16_t)(p[0] | p[1] << 8) / 32768.0f;
			} else if (bits == 24) {
				int32_t s = (p[0] << 8 | p[1] << 16 |
					     (uint32_t)p[2] << 24);
				v = (s >> 8) / 8388608.0f;
			} else {
				int32_t s = p[0] | p[1] << 8 | p[2] << 16 |
					    (uint32_t)p[3] << 24;
				v = s / 2147483648.0f;
			}
			if (c == 0) l = v;
			r = v;
		}
		if (ch == 1)
			r = l;
		out[i * 2] = l;
		out[i * 2 + 1] = r;
	}
	free(raw);
	*out_frames = frames;
	return out;
}

/* Scan du répertoire (tri alpha → slots). Appelé au démarrage (avant
 * threads) et par sampler_reload (control thread, publie sous lock). */
static int smp_name_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void smp_scan(int locked)
{
	DIR *d = opendir(SMP_DIR);
	char *names[128];
	int n = 0;
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) && n < 128) {
			size_t l = strlen(e->d_name);
			if (l > 4 && !strcasecmp(e->d_name + l - 4, ".wav"))
				names[n++] = strdup(e->d_name);
		}
		closedir(d);
	}
	qsort(names, n, sizeof(char *), smp_name_cmp);

	/* charge hors lock (IO + malloc), publie sous lock */
	float *bufs[SMP_SLOTS] = {0};
	uint32_t frs[SMP_SLOTS] = {0};
	char nms[SMP_SLOTS][64] = {{0}};
	g_smp_total = 0;
	for (int i = 0; i < n && i < SMP_SLOTS; i++) {
		char path[512];
		snprintf(path, sizeof(path), SMP_DIR "/%s", names[i]);
		bufs[i] = smp_load_wav(path, &frs[i]);
		if (bufs[i]) {
			g_smp_total += (size_t)frs[i] * 8;
			snprintf(nms[i], sizeof(nms[i]), "%s", names[i]);
			nms[i][strcspn(nms[i], ".")] = 0;   /* sans extension */
		}
	}
	for (int i = 0; i < n; i++)
		free(names[i]);

	if (locked)
		pthread_mutex_lock(&g_st.target_lock);
	/* purge différée du round PRÉCÉDENT (plus personne ne les lit) */
	for (int i = 0; i < g_smp_defer_n; i++)
		free(g_smp_defer[i]);
	g_smp_defer_n = 0;
	for (int i = 0; i < SMP_SLOTS; i++) {
		atomic_store(&g_smp[i].playing, 0);
		atomic_store(&g_smp[i].pos, 0);
		if (g_smp[i].buf)
			g_smp_defer[g_smp_defer_n++] = g_smp[i].buf;
		g_smp[i].buf = bufs[i];
		g_smp[i].frames = frs[i];
		g_smp[i].gain = 1.0f;
		snprintf(g_smp[i].name, sizeof(g_smp[i].name), "%s", nms[i]);
	}
	if (locked)
		pthread_mutex_unlock(&g_st.target_lock);
	int loaded = 0;
	for (int i = 0; i < SMP_SLOTS; i++)
		if (g_smp[i].buf)
			loaded++;
	mlog("smp: %d samples chargés (%zu Ko)", loaded, g_smp_total >> 10);
}

/* ============ V12-LOOP-PRO — loopstation multipiste (RC-505) ============
 * LOOP_TRACKS pistes indépendantes, chacune = 1 couche discrète : voie
 * source sélectionnable, mute/clear individuels. Horloge maître partagée
 * (g_master_len + g_lpos), posée par la 1re piste enregistrée ; les pistes
 * suivantes s'enregistrent alignées (un tour complet) → phase garantie.
 * Restitution additionnée dans P1/P2 (in_block[16/17], comme le sampleur).
 * Buffers alloués au démarrage, jamais en RT. memset au rec-arm d'une piste
 * VIDE (control thread, non lue par l'audio) → aucun glitch, pas de undo.
 * ARCHI_V12_LOOPER_PRO.md. */
#define LOOP_TRACKS      6
#define LOOP_MAX_FRAMES  (40u * 48000u)   /* 40 s/piste — 6×40s stéréo = 88 MiB */
/* ============ V13.3 : LIEN STÉRÉO de paires de tranches ============
 * Paires fixes (2k, 2k+1) sur les 16 tranches réelles. Une paire liée :
 * les écritures fader/mute/gate/comp/automix sur UNE tranche s'appliquent
 * aux DEUX (miroir dans les handlers socket — jamais dans l'audio).
 * Les sends ne sont PAS miroirés (pattern stéréo posé par les GUIs).
 * Voir docs/ARCHI/ARCHI_V13.3_STEREO_LINK.md */
#define N_LINK_PAIRS 8
static _Atomic int g_link[N_LINK_PAIRS];
static inline int link_partner(int src)
{
	if (src < 0 || src >= 2 * N_LINK_PAIRS)
		return -1;
	return atomic_load_explicit(&g_link[src / 2],
				    memory_order_relaxed) ? (src ^ 1) : -1;
}

/* V13.2 : TR_ARMED = REC quantifié — la piste attend le prochain début de
 * boucle maître pour passer en REC (un tour exact puis PLAY, couture ≤ 1
 * période). Demande utilisateur 2026-07-10. */
enum { TR_EMPTY, TR_REC, TR_PLAY, TR_ARMED };
static const char *const TR_NAMES[] = { "empty", "rec", "play", "armed" };

struct loop_track {
	float           *buf;        /* stéréo entrelacé LR, LOOP_MAX_FRAMES*2 */
	_Atomic uint32_t len;        /* frames (= master_len une fois posée), 0=vide */
	_Atomic int      state;      /* TR_EMPTY / TR_REC / TR_PLAY */
	_Atomic int      muted;      /* 1 = couche désactivée (conservée) */
	uint32_t         rec_head;   /* écriture piste maître (REC libre) */
	_Atomic uint32_t rec_start;  /* g_lpos capturé par l'audio au 1er bloc REC aligné */
	uint32_t         rec_done;   /* frames enregistrées ce tour (piste alignée) */
	int              src_a, src_b; /* voies source (-1 : mono → dup) */
	float            gain;
	_Atomic uint32_t peak;       /* crête VU (maj en lecture) */
};
static struct loop_track g_tr[LOOP_TRACKS];
static _Atomic uint32_t  g_master_len;  /* 0 tant qu'aucune piste posée */
static _Atomic uint32_t  g_lpos;        /* position globale (frames) */
static _Atomic int       g_loop_run;    /* transport global (0=stop, 1=play) */
static _Atomic uint32_t  g_loop_mpeak;  /* crête master (somme des pistes) */
#define REC_START_NONE 0xFFFFFFFFu

/* Rendu (audio_thread, SOUS target_lock, après le convert S32→float) */
static void loop_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	const int P = N_INPUT_MICS + N_INPUT_STEMS;   /* P1 = 16 */
	uint32_t mlen = atomic_load_explicit(&g_master_len, memory_order_acquire);
	uint32_t lpos = atomic_load_explicit(&g_lpos, memory_order_relaxed);
	int run = atomic_load_explicit(&g_loop_run, memory_order_relaxed);
	int any_rec = 0, any_play = 0;

	/* V12-VU : les pistes s'additionnent dans sl/sr (BSS), ajoutés en un
	 * passage dans P1/P2 après la boucle → crête MASTER looper mesurable
	 * (somme des pistes seules, pas polluée par sampleur/expandeur). */
	static float sl[PERIOD_FRAMES], sr[PERIOD_FRAMES];
	memset(sl, 0, sizeof(sl));
	memset(sr, 0, sizeof(sr));

	/* V13.2 : premier bloc d'un nouveau tour de boucle (lpos vient de
	 * wrapper → ∈ [0, PERIOD_FRAMES)) : les pistes ARMÉES démarrent ici. */
	const int boundary = (run && mlen && lpos < PERIOD_FRAMES);

	for (int t = 0; t < LOOP_TRACKS; t++) {
		struct loop_track *tr = &g_tr[t];
		int st = atomic_load_explicit(&tr->state, memory_order_acquire);
		if (st == TR_ARMED) {
			if (!boundary)
				continue;
			atomic_store_explicit(&tr->state, TR_REC,
					      memory_order_release);
			st = TR_REC;   /* rec_start capturé ci-dessous (≈0) */
		}
		if (st != TR_REC && st != TR_PLAY)
			continue;
		if (!tr->buf)
			continue;

		const int sa = tr->src_a;
		const int sb = tr->src_b >= 0 ? tr->src_b : tr->src_a;
		const float ga = g_st.input_gain[sa] * g_st.automix_gain[sa]
				 * g_st.keeper_gain[sa];
		const float gb = g_st.input_gain[sb] * g_st.automix_gain[sb]
				 * g_st.keeper_gain[sb];

		if (st == TR_REC) {
			any_rec = 1;
			/* V12-VU : crête de l'ENTRÉE enregistrée (visible en REC) */
			float rpk = 0.0f;
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float a = in_block[sa][f] * ga, b = in_block[sb][f] * gb;
				a = a < 0 ? -a : a; b = b < 0 ? -b : b;
				if (b > a) a = b;
				if (a > rpk) rpk = a;
			}
			atomic_store_explicit(&tr->peak,
					      (uint32_t)(rpk * 2147483647.0f),
					      memory_order_relaxed);
			if (mlen == 0) {
				/* piste MAÎTRE : REC libre → définit master_len */
				uint32_t h = tr->rec_head;
				for (int f = 0; f < PERIOD_FRAMES && h < LOOP_MAX_FRAMES; f++, h++) {
					tr->buf[(size_t)h * 2]     = in_block[sa][f] * ga;
					tr->buf[(size_t)h * 2 + 1] = in_block[sb][f] * gb;
				}
				tr->rec_head = h;
				if (h >= LOOP_MAX_FRAMES) {   /* plafond → fige */
					atomic_store_explicit(&tr->len, h, memory_order_release);
					atomic_store(&tr->state, TR_PLAY);
					atomic_store(&g_master_len, h);
					atomic_store(&g_lpos, 0);
					atomic_store(&g_loop_run, 1);
					mlen = h; lpos = 0; run = 1;
				}
			} else {
				/* piste ALIGNÉE : écrit à (rec_start+rec_done)%mlen,
				 * un tour complet puis PLAY. rec_start capturé ICI
				 * (audio) au 1er bloc → pas de décalage socket. */
				uint32_t rs = atomic_load_explicit(&tr->rec_start,
								   memory_order_relaxed);
				if (rs == REC_START_NONE) {
					rs = lpos;
					atomic_store_explicit(&tr->rec_start, rs,
							      memory_order_relaxed);
				}
				uint32_t d = tr->rec_done;
				for (int f = 0; f < PERIOD_FRAMES && d < mlen; f++, d++) {
					uint32_t idx = (rs + d) % mlen;
					tr->buf[(size_t)idx * 2]     = in_block[sa][f] * ga;
					tr->buf[(size_t)idx * 2 + 1] = in_block[sb][f] * gb;
				}
				tr->rec_done = d;
				if (d >= mlen) {   /* tour complet → couche posée */
					atomic_store_explicit(&tr->len, mlen,
							      memory_order_release);
					atomic_store(&tr->state, TR_PLAY);
				}
			}
			continue;   /* une piste en REC ne se relit pas ce bloc */
		}

		/* TR_PLAY : lecture additionnée dans sl/sr si non-mutée */
		uint32_t len = atomic_load_explicit(&tr->len, memory_order_relaxed);
		if (!len || !run || atomic_load_explicit(&tr->muted, memory_order_relaxed)) {
			atomic_store_explicit(&tr->peak, 0, memory_order_relaxed);
			continue;
		}
		any_play = 1;
		const float g = tr->gain;
		uint32_t pk = 0;
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			uint32_t idx = (lpos + f) % len;
			float l = tr->buf[(size_t)idx * 2];
			float r = tr->buf[(size_t)idx * 2 + 1];
			sl[f] += l * g;
			sr[f] += r * g;
			float a = l < 0 ? -l : l, b = r < 0 ? -r : r;
			if (a > b) b = a;
			uint32_t v = (uint32_t)(b * g * 2147483647.0f);
			if (v > pk) pk = v;
		}
		atomic_store_explicit(&tr->peak, pk, memory_order_relaxed);
	}

	/* V12-VU : ajout de la somme dans P1/P2 + crête MASTER looper */
	if (any_play) {
		float mpk = 0.0f;
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			in_block[P][f]     += sl[f];
			in_block[P + 1][f] += sr[f];
			float a = sl[f] < 0 ? -sl[f] : sl[f];
			float b = sr[f] < 0 ? -sr[f] : sr[f];
			if (b > a) a = b;
			if (a > mpk) mpk = a;
		}
		atomic_store_explicit(&g_loop_mpeak,
				      (uint32_t)(mpk * 2147483647.0f),
				      memory_order_relaxed);
	} else {
		atomic_store_explicit(&g_loop_mpeak, 0, memory_order_relaxed);
	}

	/* Avance g_lpos UNE fois par bloc (partagée par toutes les pistes) */
	if (run && mlen) {
		atomic_store_explicit(&g_lpos, (lpos + PERIOD_FRAMES) % mlen,
				      memory_order_relaxed);
	} else if (mlen == 0 && any_rec) {
		/* piste maître en cours d'enreg : rien à avancer (rec_head local) */
	}
}

/* Rendu (audio_thread, SOUS target_lock, après le convert S32→float) */
static void smp_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	const int L = N_INPUT_MICS + N_INPUT_STEMS;     /* P1 = 16 */
	for (int sl = 0; sl < SMP_SLOTS; sl++) {
		struct smp_slot *s = &g_smp[sl];
		if (!atomic_load_explicit(&s->playing, memory_order_acquire))
			continue;
		const float *b = s->buf;
		if (!b) {
			atomic_store(&s->playing, 0);
			continue;
		}
		uint32_t pos = atomic_load_explicit(&s->pos,
						    memory_order_relaxed);
		uint32_t left = s->frames > pos ? s->frames - pos : 0;
		uint32_t n = left < PERIOD_FRAMES ? left : PERIOD_FRAMES;
		const float g = s->gain;
		for (uint32_t f = 0; f < n; f++) {
			in_block[L][f]     += b[(size_t)(pos + f) * 2] * g;
			in_block[L + 1][f] += b[(size_t)(pos + f) * 2 + 1] * g;
		}
		pos += n;
		if (pos >= s->frames) {
			atomic_store(&s->playing, 0);
			atomic_store(&s->pos, 0);
		} else {
			atomic_store_explicit(&s->pos, pos,
					      memory_order_relaxed);
		}
	}
}

/* Lin-ramp 64-frame entre current et target. Appelé chaque frame. */
static void smooth_gains(void)
{
	/* Approche simple : à chaque frame, current += (target - current) / 64.
	 * Asymptotique mais converge rapidement (~1.3 ms à 99.9 %).
	 */
	const float alpha = 1.0f / (float)GAIN_RAMP_FRAMES;

	for (int i = 0; i < N_INPUT_TOTAL; i++)
		for (int b = 0; b < N_BUS_FX_CH; b++)
			g_st.send_gain[i][b] +=
				alpha * (g_st.send_target[i][b] - g_st.send_gain[i][b]);

	for (int s = 0; s < N_INPUT_TOTAL; s++)
		for (int o = 0; o < N_OUTPUT_TOTAL; o++)
			g_st.master_gain[s][o] +=
				alpha * (g_st.master_target[s][o] - g_st.master_gain[s][o]);

	for (int b = 0; b < N_BUS_FX_CH; b++)
		g_st.fx_bus_gain[b] +=
			alpha * (g_st.fx_bus_target[b] - g_st.fx_bus_gain[b]);

	for (int i = 0; i < N_INPUT_TOTAL; i++)
		g_st.input_gain[i] +=
			alpha * (g_st.input_target[i] - g_st.input_gain[i]);

	/* V12-AMX : slew des auto-gains vers la cible Dugan (resp_ms).
	 * alpha_amx par BLOC (smooth_gains est appelé par bloc de 2 ms).
	 * automix OFF ⇒ retour en douceur vers 1.0 (chemin d'origine). */
	{
		const float alpha_amx = 1.0f - expf(-2.0f /
			(g_st.automix_resp_ms > 1.0f ? g_st.automix_resp_ms : 1.0f));
		for (int i = 0; i < N_INPUT_TOTAL; i++) {
			const float tgt = g_st.automix_on
					  ? g_st.automix_gtarget[i] : 1.0f;
			g_st.automix_gain[i] += alpha_amx *
				(tgt - g_st.automix_gain[i]);
		}
	}

	/* V13-BANDMIX : slew TRÈS lent du keeper (τ ≈ 2 s → ~0,5 dB/s pour
	 * les petites corrections — incapable de pomper). */
	{
		const float alpha_k = 0.001f;   /* 1-exp(-2ms/2000ms) */
		for (int i = 0; i < N_INPUT_TOTAL; i++)
			g_st.keeper_gain[i] += alpha_k *
				(g_st.keeper_target[i] - g_st.keeper_gain[i]);
	}

	/* V13.9 — BALANCE AUTO : slew du gain de présence (τ ≈ 2 s, comme le
	 * keeper — le dB/tick de la boucle 1 Hz fixe déjà la vitesse macro). */
	{
		const float alpha_p = 0.001f;
		for (int i = 0; i < N_INPUT_TOTAL; i++)
			g_st.presence_gain[i] += alpha_p *
				(g_st.presence_target[i] - g_st.presence_gain[i]);
	}
}

/* ========= V12-EXP — expandeur/gate par tranche (16 voies réelles) =========
 * Downward expander in-place sur in_block[0..15], AVANT smp/loop/automix/
 * mix : le gate s'applique à tout l'aval (sends, master, looper, automix,
 * tap NPU) — une seule vérité du signal de tranche. P1/P2 exclues.
 * Enveloppe crête par bloc (2 ms), coefs attack/release PRÉCALCULÉS à la
 * config (jamais d'expf en RT), hold anti-chatter (granularité 1 bloc),
 * rampe de gain linéaire intra-bloc (zipper-free), GR publié en atomic
 * pour la GUI. off = zéro coût. ARCHI_V12_EXPANDER.md. */
#define N_EXP_CH (N_INPUT_MICS + N_INPUT_STEMS)   /* 16 */

struct exp_ch {
	int   on;
	float thr_db, ratio, range_db;    /* config user (dB, pente) */
	float atk_ms, rel_ms, hold_ms;    /* config user (pour get/save) */
	float thr_lin, ka, kr;            /* précalc (control thread) */
	int   hold_blocks;                /* précalc : hold_ms / 2 ms */
	float env, gain;                  /* état audio (crête lissée, gain lin) */
	int   hold_cnt;
	_Atomic uint32_t gr_mdb;          /* réduction courante en milli-dB (GUI) */
};
static struct exp_ch g_exp[N_EXP_CH];

/* Précalculs — control thread (handler socket / load state), SOUS
 * target_lock quand le daemon tourne. Valide et clampe les plages. */
static void exp_configure(int src, int on, float thr_db, float ratio,
			  float atk_ms, float rel_ms, float range_db,
			  float hold_ms)
{
	if (src < 0 || src >= N_EXP_CH)
		return;
	struct exp_ch *e = &g_exp[src];
	if (thr_db < -80.0f) thr_db = -80.0f;
	if (thr_db > 0.0f)   thr_db = 0.0f;
	if (ratio < 1.0f)    ratio = 1.0f;
	if (ratio > 20.0f)   ratio = 20.0f;
	if (atk_ms < 0.5f)   atk_ms = 0.5f;
	if (atk_ms > 100.0f) atk_ms = 100.0f;
	if (rel_ms < 5.0f)   rel_ms = 5.0f;
	if (rel_ms > 1000.0f) rel_ms = 1000.0f;
	if (range_db < 0.0f)  range_db = 0.0f;
	if (range_db > 80.0f) range_db = 80.0f;
	if (hold_ms < 0.0f)   hold_ms = 0.0f;
	if (hold_ms > 500.0f) hold_ms = 500.0f;
	e->thr_db = thr_db;  e->ratio = ratio;  e->range_db = range_db;
	e->atk_ms = atk_ms;  e->rel_ms = rel_ms; e->hold_ms = hold_ms;
	e->thr_lin = powf(10.0f, thr_db / 20.0f);
	e->ka = 1.0f - expf(-2.0f / atk_ms);
	e->kr = 1.0f - expf(-2.0f / rel_ms);
	e->hold_blocks = (int)(hold_ms / 2.0f);
	e->on = on ? 1 : 0;
	if (!e->on) {   /* off : état neutre, aucun résidu à la réactivation */
		e->env = 0.0f; e->gain = 1.0f; e->hold_cnt = 0;
		atomic_store_explicit(&e->gr_mdb, 0, memory_order_relaxed);
	}
}

/* Rendu (audio_thread, SOUS target_lock, juste après le convert S32→float) */
static void exp_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	for (int i = 0; i < N_EXP_CH; i++) {
		struct exp_ch *e = &g_exp[i];
		if (!e->on)
			continue;

		/* 1. crête du bloc */
		float p = 0.0f;
		const float *x = in_block[i];
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			float v = x[f] < 0 ? -x[f] : x[f];
			if (v > p) p = v;
		}
		/* 2. enveloppe asymétrique (coefs précalculés) */
		e->env += (p > e->env ? e->ka : e->kr) * (p - e->env);

		/* 3. hold anti-chatter */
		if (e->env >= e->thr_lin)
			e->hold_cnt = e->hold_blocks;
		else if (e->hold_cnt > 0)
			e->hold_cnt--;

		/* 4. gain cible */
		float g_db = 0.0f;
		if (e->env < e->thr_lin && e->hold_cnt == 0) {
			float env_db = 20.0f * log10f(e->env + 1e-10f);
			g_db = (env_db - e->thr_db) * (e->ratio - 1.0f);
			if (g_db < -e->range_db)
				g_db = -e->range_db;
		}
		float gt = (g_db >= 0.0f) ? 1.0f : powf(10.0f, g_db / 20.0f);
		atomic_store_explicit(&e->gr_mdb,
				      (uint32_t)(-g_db * 1000.0f),
				      memory_order_relaxed);

		/* 5. rampe linéaire gain_prev → gain (zipper-free) */
		float g0 = e->gain;
		float step = (gt - g0) / (float)PERIOD_FRAMES;
		float *y = in_block[i];
		float g = g0;
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			g += step;
			y[f] *= g;
		}
		e->gain = gt;
	}
}

/* ========= V13-COMP — compresseur natif par tranche (16 voies) =========
 * Downward compressor in-place sur in_block[0..15], APRÈS le gate
 * (exp_render) : ordre console standard gate→comp. Même patron RT que le
 * gate : enveloppe crête/bloc 2 ms, coefs précalculés au set, rampe de
 * gain intra-bloc, GR atomic, off = zéro coût. Prérequis de l'assistant
 * V13-BANDMIX (les tranches USB n'ont pas de DRC DSP).
 * ARCHI_V13_BANDMIX.md. */
struct cmp_ch {
	int   on;
	int   releasing;                   /* extinction douce : ramp gain→1 */
	float thr_db, ratio, makeup_db;   /* config user */
	float atk_ms, rel_ms;
	float thr_lin, ka, kr, makeup_lin; /* précalc (control thread) */
	float env, gain;                   /* état audio */
	_Atomic uint32_t gr_mdb;           /* réduction courante milli-dB */
};
static struct cmp_ch g_cmp[N_EXP_CH];

static void cmp_configure(int src, int on, float thr_db, float ratio,
			  float atk_ms, float rel_ms, float makeup_db)
{
	if (src < 0 || src >= N_EXP_CH)
		return;
	struct cmp_ch *c = &g_cmp[src];
	if (thr_db < -60.0f) thr_db = -60.0f;
	if (thr_db > 0.0f)   thr_db = 0.0f;
	if (ratio < 1.0f)    ratio = 1.0f;
	if (ratio > 20.0f)   ratio = 20.0f;
	if (atk_ms < 0.5f)   atk_ms = 0.5f;
	if (atk_ms > 250.0f) atk_ms = 250.0f;
	if (rel_ms < 5.0f)   rel_ms = 5.0f;
	if (rel_ms > 2000.0f) rel_ms = 2000.0f;
	if (makeup_db < 0.0f)  makeup_db = 0.0f;
	if (makeup_db > 24.0f) makeup_db = 24.0f;
	c->thr_db = thr_db;  c->ratio = ratio;  c->makeup_db = makeup_db;
	c->atk_ms = atk_ms;  c->rel_ms = rel_ms;
	c->thr_lin = powf(10.0f, thr_db / 20.0f);
	c->ka = 1.0f - expf(-2.0f / atk_ms);
	c->kr = 1.0f - expf(-2.0f / rel_ms);
	c->makeup_lin = powf(10.0f, makeup_db / 20.0f);
	c->on = on ? 1 : 0;
	if (!c->on) {
		/* extinction : si le comp jouait (gain réduit), on RAMPE vers 1 via
		 * cmp_render (pas de saut = pas de clic). Si jamais lancé (gain≤0 au
		 * boot) ou déjà à l'unité, on fige direct. */
		if (c->gain <= 0.0f || c->gain == 1.0f) {
			c->env = 0.0f; c->gain = 1.0f; c->releasing = 0;
			atomic_store_explicit(&c->gr_mdb, 0, memory_order_relaxed);
		} else {
			c->releasing = 1;
		}
	} else {
		c->releasing = 0;
	}
}

/* Rendu (audio_thread, SOUS target_lock, juste après exp_render) */
/* NOTE (revue 2026-07-28, lot 5b) : ce compresseur de TRANCHE n'est PAS un
 * doublon de fx_init_compressor (effects.c) — algorithmes distincts à
 * dessein : ici crête PAR BLOC + loi de gain en dB + rampe de gain intra-
 * bloc anti-zipper + gr publié GUI + extinction douce (16 voies mono,
 * seuils adaptatifs bmx) ; effects.c = enveloppe PAR ÉCHANTILLON + loi
 * linéaire (insert stéréo master, params fixes). Les fusionner changerait
 * le son validé des deux. */
static void cmp_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	for (int i = 0; i < N_EXP_CH; i++) {
		struct cmp_ch *c = &g_cmp[i];
		if (!c->on && !c->releasing)
			continue;
		float gt;
		if (c->on) {
			float p = 0.0f;
			const float *x = in_block[i];
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float v = x[f] < 0 ? -x[f] : x[f];
				if (v > p) p = v;
			}
			c->env += (p > c->env ? c->ka : c->kr) * (p - c->env);

			float g_db = 0.0f;
			if (c->env > c->thr_lin) {
				float env_db = 20.0f * log10f(c->env + 1e-10f);
				g_db = (c->thr_db - env_db) * (1.0f - 1.0f / c->ratio);
			}
			gt = powf(10.0f, g_db / 20.0f) * c->makeup_lin;
			atomic_store_explicit(&c->gr_mdb,
					      (uint32_t)(-g_db * 1000.0f),
					      memory_order_relaxed);
		} else {
			gt = 1.0f;   /* releasing : cible unité, ramp doux vers 1 */
			atomic_store_explicit(&c->gr_mdb, 0, memory_order_relaxed);
		}

		float g0 = c->gain;
		float step = (gt - g0) / (float)PERIOD_FRAMES;
		float *y = in_block[i];
		float g = g0;
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			g += step;
			y[f] *= g;
		}
		c->gain = gt;
		if (c->releasing && fabsf(gt - 1.0f) < 1e-3f) {
			c->releasing = 0;   /* extinction terminée */
			c->env = 0.0f;
		}
	}
}

/* ========= V13-BANDMIX — assistant auto-mix musique =========
 * Soundcheck mesuré par tranche → calcul déterministe par RÔLE (gain
 * staging, gate, comp, fader de départ) → suivi live LENT (keeper ±3 dB,
 * parts de loudness vs référence verrouillée, priorité voix). Le cerveau
 * vit dans le plan de contrôle (persistence_thread 1 Hz) ; l'audio ne
 * fait que publier les mean-squares et appliquer keeper_gain (multiplié
 * dans la chaîne automix). ARCHI_V13_BANDMIX.md. */

/* mean square par bloc (brut) + EWMA τ≈3 s, publiés par l'audio
 * (bits float dans un u32). Le contrôle utilise l'EWMA (anti-aliasing —
 * lire 1 bloc de 2 ms par seconde échantillonne 0,2 % du signal). */
static _Atomic uint32_t g_ms_in[N_EXP_CH];
static _Atomic uint32_t g_ms_avg[N_EXP_CH];
static float            g_ms_sm[N_EXP_CH];   /* privé audio_thread */

static inline float bmx_ms(int i)
{
	union { float f; uint32_t u; } v =
		{ .u = atomic_load_explicit(&g_ms_in[i], memory_order_relaxed) };
	return v.f;
}
static inline float bmx_avg(int i)
{
	union { float f; uint32_t u; } v =
		{ .u = atomic_load_explicit(&g_ms_avg[i], memory_order_relaxed) };
	return v.f;
}

enum { BR_OFF, BR_LEAD, BR_CHOIR, BR_KICK, BR_SNARE, BR_DRUMS,
       BR_BASS, BR_GUITAR, BR_KEYS, BR_LINE, BR_NROLES };
static const char *const BR_NAMES[BR_NROLES] = {
	"off", "lead", "choir", "kick", "snare", "drums",
	"bass", "guitar", "keys", "line" };

/* presets par rôle : cible mix (dB rel. lead), gate, comp */
static const struct bmx_preset {
	float mix_db;
	int   gate_on;  float gate_ratio, gate_hold;
	int   comp_on;  float c_thr, c_ratio, c_atk, c_rel;
} BMX_P[BR_NROLES] = {
	[BR_OFF]    = { 0 },
	[BR_LEAD]   = {  0.0f, 1,  2.0f,  80, 1, -18, 3.0f,  15, 150 },
	[BR_CHOIR]  = { -4.0f, 1,  2.0f,  80, 1, -18, 3.0f,  15, 150 },
	[BR_KICK]   = { -2.0f, 1, 20.0f,  60, 1, -14, 4.0f,   5,  80 },
	[BR_SNARE]  = { -3.0f, 1, 20.0f,  60, 1, -15, 3.0f,   5, 100 },
	[BR_DRUMS]  = { -6.0f, 0,  0,      0, 1, -16, 2.0f,  10, 150 },
	[BR_BASS]   = { -3.0f, 0,  0,      0, 1, -18, 4.0f,  20, 250 },
	[BR_GUITAR] = { -5.0f, 0,  0,      0, 1, -17, 2.5f,  15, 180 },
	[BR_KEYS]   = { -5.0f, 0,  0,      0, 1, -17, 2.0f,  20, 200 },
	[BR_LINE]   = { -5.0f, 0,  0,      0, 0, 0, 0, 0, 0 },
};

/* V13.6 : décalage de seuil comp (dB au-dessus du loudness courant de la
 * source) pour l'AUTOMIX LIVE — seuil = al_ref[role] + offset. Négatif =
 * plus serré (tient la source), positif = ne prend que les crêtes. */
static const float COMP_OFF[BR_NROLES] = {
	[BR_OFF] = 0, [BR_LEAD] = -2, [BR_CHOIR] = -2, [BR_KICK] = +2,
	[BR_SNARE] = +2, [BR_DRUMS] = +2, [BR_BASS] = -3, [BR_GUITAR] = -1,
	[BR_KEYS] = -1, [BR_LINE] = 0,
};

/* ===== V13.6 : EQ logiciel de PLACEMENT par voix (autolive) =====
 * Cascade de 2 biquads RBJ par tranche (HPF + 1 cloche), coefs par RÔLE,
 * états par tranche. Entre gate et comp. Coefs recalculés au changement
 * de rôle seulement. Statique (place les instruments) ; complète le
 * vfocus dynamique. ARCHI_V13.6_AUTOMIX_COMP_EQ.md — g_eqx défini ici,
 * eqx_render (qui lit g_bmx.role) plus bas, APRÈS la struct g_bmx. */
/* 3 biquads par voix = parité avec les mics TAC (mode 3 Biquads/Ch) :
 * HPF + 2 cloches (présence/creusement + modelage). */
#define EQX_BQ 3
/* struct eqx_bq + designers RBJ (eqx_hpf, rbj_peak_core, eqx_peak,
 * meq_shelf) : déplacés dans dsp_bq.c/dsp_bq.h (V14.0 étape 0). */
static struct {
	struct eqx_bq bq[2][N_EXP_CH][EQX_BQ];  /* double-buffer : bascule sans clic */
	_Atomic int   bank[N_EXP_CH];           /* banque active par voie */
	float st[N_EXP_CH][EQX_BQ][2];          /* états (audio, JAMAIS vidés en live) */
	int   role_of[N_EXP_CH];      /* rôle pour lequel les coefs sont calculés */
	_Atomic int on;
} g_eqx;

/* presets : HPF Hz (0=off) ; 2 cloches freq/gain dB/Q (gain 0 = neutre) */
static const struct {
	float hpf;
	float f1, g1, q1;   /* cloche 1 : présence / creusement voix */
	float f2, g2, q2;   /* cloche 2 : modelage (boue/air/corps) */
} EQX_P[BR_NROLES] = {
	[BR_OFF]    = { 0 },
	/* V13.9 : HPF voix relevés — la voix ne bave plus dans le territoire
	 * basse/kick. Lead 250 Hz validé oreille (150 « mieux, pousse encore »). */
	[BR_LEAD]   = { 250, 3500, +3.0f, 0.9f,  500,  -2.0f, 1.0f },  /* présence + dé-boue */
	[BR_CHOIR]  = { 180, 4000, +1.5f, 0.9f,  400,  -1.5f, 1.0f },
	[BR_KICK]   = { 0,   70,   +2.5f, 0.9f,  400,  -3.0f, 1.2f },  /* poids + creux carton */
	[BR_SNARE]  = { 120, 4000, +2.0f, 0.9f,  250,  +1.5f, 1.0f },  /* claquant + corps */
	[BR_DRUMS]  = { 200, 6000, +1.5f, 0.9f,  500,  -1.5f, 1.0f },  /* air + dé-boue */
	[BR_BASS]   = { 30,  80,   +1.5f, 1.0f,  3500, -2.0f, 1.0f },  /* grave + dégage voix */
	[BR_GUITAR] = { 120, 3000, -3.0f, 1.0f,  300,  -2.0f, 1.0f },  /* creuse voix + dé-boue */
	[BR_KEYS]   = { 120, 3000, -3.0f, 1.0f,  300,  -2.0f, 1.0f },  /* creuse voix + dé-boue */
	[BR_LINE]   = { 80,  0, 0, 0,  0, 0, 0 },
};

static void eqx_config(int i, int role)   /* control thread (rare) */
{
	/* calcule dans la banque INACTIVE puis bascule atomiquement : l'audio ne
	 * lit jamais des coefs à moitié écrits. Les états NE sont PAS vidés → le
	 * filtre glisse vers les nouveaux coefs sans saut d'échantillon (crack). */
	int nb = !atomic_load_explicit(&g_eqx.bank[i], memory_order_relaxed);
	eqx_hpf(&g_eqx.bq[nb][i][0], EQX_P[role].hpf);
	eqx_peak(&g_eqx.bq[nb][i][1], EQX_P[role].f1, EQX_P[role].g1, EQX_P[role].q1);
	eqx_peak(&g_eqx.bq[nb][i][2], EQX_P[role].f2, EQX_P[role].g2, EQX_P[role].q2);
	atomic_store_explicit(&g_eqx.bank[i], nb, memory_order_release);
	g_eqx.role_of[i] = role;
}

/* ============ V13.7 — MASTER : EQ mastering + makeup LUFS (BS.1770) ========
 * Étage master dans l'audio_thread, AVANT l'insert (spectral_env/exciter/
 * limiter_native) pour que le limiteur EXISTANT tienne les crêtes :
 *   out 0/1 → EQ master (3 biquads) → makeup (piloté LUFS) → insert → out_gain
 * Mètre short-term LUFS K-pondéré (K-weighting ITU-R BS.1770) sur la sortie
 * réelle, lu par bmx_tick qui asservit le makeup vers MASTER_LUFS_TGT.
 * Lié à AUTOMIX LIVE (g_master_on). ARCHI_V13.7_MASTER_LUFS_EQ.md */
#define MASTER_LUFS_TGT   (-14.0f)
#define MASTER_MK_MAX_DB   (36.0f)   /* makeup = gain-staging global (stems faibles) */
#define MASTER_MK_MIN_DB   (-6.0f)
#define LUFS_ST_A          (1.0f / (3.0f * (float)SAMPLE_RATE))   /* short-term ~3 s */

static void save_master_eq(void);        /* déf. plus bas (persistance) */
static _Atomic int g_master_on;          /* étage master actif (autolive) */

/* --- EQ master : 3 biquads RBJ (low shelf / -500 bell / high shelf), double
 *     buffer pour bascule sans lock depuis le control thread --- */
static struct eqx_bq g_meq_bank[2][3];
static _Atomic int    g_meq_active;         /* banque active (steady) */
static _Atomic int    g_meq_pending = -1;   /* banque à fondre (-1 = aucune) */
static float          g_meq_st[2][2][3][2]; /* [banque][L/R][biquad][z] */
#define MEQ_XF_LEN 2400                     /* crossfade coefs ~50 ms (anti-clic) */
static int            g_meq_fading;         /* audio-owned : fondu en cours */
static int            g_meq_xf;             /* audio-owned : position du fondu */
static struct {                          /* params (control thread) */
	float low_hz, low_db;
	float mid_hz, mid_db, mid_q;
	float air_hz, air_db;
} g_meq_p = { 60.0f, +3.0f, 500.0f, -2.5f, 1.0f, 10000.0f, +3.0f };

/* --- makeup LUFS --- */
static struct {
	float k1[2][2], k2[2][2];   /* K-weighting : 2 biquads BS.1770 × L/R */
	float ms;                   /* EWMA puissance K-pondérée (short-term) */
	float mk_db;                /* makeup courant en dB (état bmx_tick) */
	_Atomic int makeup_mq;      /* cible makeup ×1000 linéaire (→ audio) */
	float makeup_cur;           /* gain lissé (audio_thread) */
	_Atomic int lufs_c;         /* LUFS short-term ×100 publié (→ bmx_tick) */
} g_mk = { .makeup_mq = 1000, .lufs_c = -12000 };

/* K-weighting ITU-R BS.1770 @ 48 kHz — coefficients canoniques (forme
 * transposée II, a0=1). Stage 1 = pré-filtre shelf tête ; stage 2 = RLB HP.
 * SOURCE (revue code 2026-07-28, F20) : Rec. UIT-R BS.1770-4 (10/2015),
 * §1 Annexe 1, Tableaux 1 et 2 — valeurs EXACTES de la norme pour fs=48 kHz
 * (reprises telles quelles par libebur128). Ne PAS les recalculer : toute
 * dérivation maison doit être validée contre ces valeurs de référence. */
#define K1_B0   1.53512485958697f
#define K1_B1  (-2.69169618940638f)
#define K1_B2   1.19839281085285f
#define K1_A1  (-1.69065929318241f)
#define K1_A2   0.73248077421585f
#define K2_B0   1.0f
#define K2_B1  (-2.0f)
#define K2_B2   1.0f
#define K2_A1  (-1.99004745483398f)
#define K2_A2   0.99007225036621f

static void meq_compute(int bank)
{
	meq_shelf(&g_meq_bank[bank][0], g_meq_p.low_hz, g_meq_p.low_db, 0);
	eqx_peak (&g_meq_bank[bank][1], g_meq_p.mid_hz, g_meq_p.mid_db, g_meq_p.mid_q);
	meq_shelf(&g_meq_bank[bank][2], g_meq_p.air_hz, g_meq_p.air_db, 1);
}

/* CHANGEMENT LIVE : calcule dans la banque inactive (états frais) et signale
 * un crossfade à l'audio → l'ancien et le nouveau filtre sont mélangés en
 * fondu sur ~50 ms. Zéro clic, même en passant par un gain 0 (passe-tout). */
static void meq_recalc(void)
{
	int nb = !atomic_load_explicit(&g_meq_active, memory_order_relaxed);
	meq_compute(nb);
	/* états : PAS de reset à zéro (sinon la cloche sonne un transitoire à sa
	 * fréquence = pop). L'audio copie l'état chaud de l'ancienne banque au
	 * démarrage du fondu → les deux filtres partent du même état. */
	atomic_store_explicit(&g_meq_pending, nb, memory_order_release);
}

/* BOOT / ENABLE : pose les coefs direct dans la banque active, pas de fondu
 * (pas d'audio en cours ou reset volontaire). */
static void meq_init(void)
{
	int a = atomic_load_explicit(&g_meq_active, memory_order_relaxed);
	meq_compute(a);
	atomic_store_explicit(&g_meq_pending, -1, memory_order_relaxed);
}

/* traite les 3 biquads d'une banque pour 1 échantillon (états mis à jour) */
static inline float meq_chain(int bank, int ch, float in)
{
	float x = in;
	for (int b = 0; b < 3; b++) {
		struct eqx_bq *q = &g_meq_bank[bank][b];
		float z1 = g_meq_st[bank][ch][b][0];
		float z2 = g_meq_st[bank][ch][b][1];
		float y = q->b0 * x + z1;
		g_meq_st[bank][ch][b][0] = q->b1 * x - q->a1 * y + z2;
		g_meq_st[bank][ch][b][1] = q->b2 * x - q->a2 * y;
		x = y;
	}
	return x;
}

struct bmx_meas {
	int    done;
	float  rms_avg_db, peak_db, floor_db;
};
static struct {
	int    role[N_EXP_CH];
	struct bmx_meas m[N_EXP_CH];
	/* soundcheck en cours (contrôle + audio) */
	_Atomic int meas_src;         /* -1 = aucune */
	struct timespec meas_t0;
	double acc_ms; uint32_t nblk_s;   /* échantillonné à 1 Hz (ticks) */
	float  peak_max, sm, minsm;
	int    warm;
	/* keeper live */
	int    live;
	int    ref_valid;
	int    autolive;                  /* V13.5 : automix continu */
	float  al_anchor;                 /* V13.5 : ancre loudness (voix), gelée */
	float  al_ref[N_EXP_CH];          /* V13.5 : peak-hold loudness pré-fader
					   * (détection silence sans soundcheck) */
	float  risk[N_EXP_CH];            /* V13.8 : mémoire du risque LENTE
					   * (peak-hold, décroît 0,05 dB/s) — plafonne
					   * le keeper AU RETOUR DE SILENCE (anti-
					   * blast de reprise, V13.9) */
	int    act_ticks[N_EXP_CH];       /* V13.9 : ticks consécutifs d'activité
					   * (remis à 0 au gel) — le plafond risque
					   * ne s'applique que ≤ 5 (reprise) */
	float  ref_share[N_EXP_CH];       /* parts de puissance verrouillées */
	float  lt_ms[N_EXP_CH];           /* loudness long terme POST-fader (τ 10 s) */
	float  lt_pre[N_EXP_CH];          /* idem PRÉ-fader (détection silence,
					   * même référentiel que floor_db) */
	float  kdb[N_EXP_CH];             /* correction courante dB (±3 keeper / ±24 auto) */
	int    locking;                    /* capture de référence en cours */
	int    lock_ticks;
	double lock_acc[N_EXP_CH];
	/* V13.9 — tunables automix réglables en live (R&D, jamais en dur) */
	float  freeze_db;                 /* gel si pre < al_ref − freeze_db
					   * (1/4 du max reçu = 12 dB) */
	float  risk_decay;                /* oubli mémoire du risque (dB/s) */
	float  risk_margin;              /* marge du plafond risque (dB) */
	float  gate_db;                   /* V13.9 GATE AUTO : seuil adaptatif
					   * = al_ref − gate_db (rôles gate_on,
					   * ferme le micro sur la repisse) */
	/* V13.9 — BALANCE AUTO musique/voix/chœurs : tient la voix lead et les
	 * chœurs à un écart cible au-dessus du lit musique (somme des loudness
	 * lt_ms par groupe). Agit sur presence_gain, le makeup LUFS tient −14. */
	int    balance_on;                /* balance auto active */
	float  bal_lufs_tgt;              /* cible LUFS master (−14) */
	/* bal_e_tgt / bal_c_tgt = CIBLES d'écart de loudness « groupe − musique »
	 * en dB, PAS des erreurs : valeur POSITIVE = le groupe est voulu AU-DESSUS
	 * du lit musique (design validé oreille : voix +3, chœurs +1,5). Plage
	 * [−6..+12] : négatif autorisé pour reculer un groupe si besoin. Op
	 * set_balance {lufs_tgt, e_tgt, c_tgt} — cf. MIXER_PRO_REFERENCE.md. */
	float  bal_e_tgt;                 /* cible écart voix lead−musique (+3 dB) */
	float  bal_c_tgt;                 /* cible écart chœurs−musique (+1,5 dB) */
	float  g_voice_db;                /* gain groupe VOIX LEAD courant (dB) */
	float  g_choir_db;                /* gain groupe CHŒURS courant (dB) */
	float  g_music_db;                /* gain groupe MUSIQUE courant (dB) */
	float  prog_peak;                 /* peak-hold loudness programme (gel) */
	int    bal_staged;                /* 0 = staging initial (8 dB/s jusqu'au
					   * 1er lock ±2 dB) — volume utilisable
					   * en ~4 s dès que les musiciens jouent */
	/* V13.9 — SOLO : la voie soloée monte à l'ancre + solo_db (sans plafond
	 * risque, slew rapide 3 dB/tick). Manuel (bouton GUI) ou détection auto
	 * (voix muette + une source musique domine nettement les autres). */
	int    solo_src;                  /* voie en solo (−1 = aucune) */
	int    solo_auto;                 /* détection automatique active */
	int    solo_is_auto;              /* le solo courant vient de l'auto */
	int    solo_on_cnt, solo_off_cnt; /* hystérésis engage/release (ticks) */
	float  solo_db;                   /* place du solo vs ancre (−1 dB) */
	float  solo_base[N_EXP_CH];       /* V2 : ligne de base par voie (EWMA
					   * lente du loudness pré-fader) — un
					   * solo = la voie monte +6 dB AU-DESSUS
					   * DE SA PROPRE base (pas de la
					   * batterie). −999 = non initialisée */
} g_bmx = { .meas_src = -1, .freeze_db = 12.0f, .risk_decay = 0.05f,
	    .risk_margin = 3.0f, .gate_db = 15.0f, .balance_on = 1,
	    .bal_lufs_tgt = -14.0f, .bal_e_tgt = 3.0f, .bal_c_tgt = 1.5f,
	    .prog_peak = -120.0f,
	    /* solo_auto OFF par défaut : une automation non validée à l'oreille
	     * ne s'active pas toute seule (suspect pompage) — opt-in au bouton */
	    .solo_src = -1, .solo_auto = 0, .solo_db = -1.0f };

/* V13.6 : EQ de placement (audio_thread, entre gate et comp) — lit
 * g_bmx.role donc défini APRÈS g_bmx. Biquads forme II transposée. */
static void eqx_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	if (!atomic_load_explicit(&g_eqx.on, memory_order_relaxed))
		return;
	for (int i = 0; i < N_EXP_CH; i++) {
		int role = g_bmx.role[i];
		if (role == BR_OFF)
			continue;
		if (g_eqx.role_of[i] != role)   /* rôle changé : recalcule */
			eqx_config(i, role);
		int bk = atomic_load_explicit(&g_eqx.bank[i], memory_order_acquire);
		for (int b = 0; b < EQX_BQ; b++) {
			struct eqx_bq *q = &g_eqx.bq[bk][i][b];
			float z1 = g_eqx.st[i][b][0], z2 = g_eqx.st[i][b][1];
			float *x = in_block[i];
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float in = x[f];
				float y = q->b0 * in + z1;
				z1 = q->b1 * in - q->a1 * y + z2;
				z2 = q->b2 * in - q->a2 * y;
				x[f] = y;
			}
			g_eqx.st[i][b][0] = z1;
			g_eqx.st[i][b][1] = z2;
		}
	}
}

/* --- tick 1 Hz (persistence_thread) : soundcheck + lock + keeper --- */
static void bmx_tick(void)
{
	/* 1. soundcheck : échantillonne la tranche mesurée */
	int src = atomic_load(&g_bmx.meas_src);
	if (src >= 0) {
		float ms  = bmx_ms(src);    /* bloc brut : crête */
		float avg = bmx_avg(src);   /* EWMA 3 s : moyenne/floor */
		g_bmx.acc_ms += avg;
		g_bmx.nblk_s++;
		if (ms > g_bmx.peak_max) g_bmx.peak_max = ms;
		if (g_bmx.warm++ >= 3 && avg < g_bmx.minsm)
			g_bmx.minsm = avg;
		(void)g_bmx.sm;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - g_bmx.meas_t0.tv_sec >= 12) {
			struct bmx_meas *m = &g_bmx.m[src];
			float avg = g_bmx.nblk_s ?
				(float)(g_bmx.acc_ms / g_bmx.nblk_s) : 0.0f;
			m->rms_avg_db = 10.0f * log10f(avg + 1e-12f);
			m->peak_db    = 10.0f * log10f(g_bmx.peak_max + 1e-12f);
			m->floor_db   = 10.0f * log10f(g_bmx.minsm + 1e-12f);
			m->done = 1;
			atomic_store(&g_bmx.meas_src, -1);
			mlog("bandmix: mesure src %d — rms %.1f dB, floor %.1f dB",
			     src, m->rms_avg_db, m->floor_db);
		}
	}

	/* 2. loudness long terme (τ ≈ 10 s) pour lock + keeper.
	 * lt_ms = POST-fader (parts du mix) ; lt_pre = PRÉ-fader (même
	 * référentiel que floor_db mesuré → détection silence cohérente). */
	const float kl = 0.095f;   /* 1 - exp(-1/10) */
	for (int i = 0; i < N_EXP_CH; i++) {
		float ms = bmx_avg(i);   /* EWMA 3 s — anti-aliasing */
		/* BOUCLE FERMÉE : la mesure inclut keeper_gain, sinon le
		 * correcteur ne voit jamais l'effet de ses corrections et
		 * l'intégrateur file aux butées (observé au 1er test). */
		float g = g_st.input_gain[i] * g_st.keeper_gain[i];
		g_bmx.lt_ms[i] += kl * (ms * g * g - g_bmx.lt_ms[i]);
		g_bmx.lt_pre[i] += kl * (ms - g_bmx.lt_pre[i]);
	}

	/* 3. capture de référence (VERROUILLER, 30 ticks) */
	if (g_bmx.locking) {
		for (int i = 0; i < N_EXP_CH; i++)
			if (g_bmx.role[i] != BR_OFF)
				g_bmx.lock_acc[i] += g_bmx.lt_ms[i];
		if (++g_bmx.lock_ticks >= 30) {
			double tot = 1e-12;
			for (int i = 0; i < N_EXP_CH; i++)
				tot += g_bmx.lock_acc[i];
			for (int i = 0; i < N_EXP_CH; i++)
				g_bmx.ref_share[i] =
					(float)(g_bmx.lock_acc[i] / tot);
			g_bmx.ref_valid = 1;
			g_bmx.locking = 0;
			atomic_store(&g_presets_dirty, 1);
			mlog("bandmix: équilibre verrouillé");
		}
	}

	/* 4a. AUTOMIX LIVE (V13.5) : nivellement ANCRÉ SUR LA VOIX, sans saut.
	 * Une ancre suit le loudness de la voix lead quand elle chante et se
	 * GÈLE pendant les breaks (pas de saut de balance). Chaque source
	 * active est amenée à (ancre + offset de rôle) d'après son loudness
	 * intrinsèque pré-fader → balance exacte relative à la voix, gains
	 * modérés (la voix reste ≈ à l'unité, elle EST la référence). Le fader
	 * reste un biais utilisateur. Silence auto par peak-hold. */
	if (g_bmx.autolive) {
		/* ancre = loudness voix lissé ; sinon max des sources actives */
		float Llead = -120.0f, Lmax = -120.0f;
		int   have_lead = 0, any = 0;
		float pre_db[N_EXP_CH];
		int   act[N_EXP_CH];
		for (int i = 0; i < N_EXP_CH; i++) {
			act[i] = 0;
			if (g_bmx.role[i] == BR_OFF) continue;
			float pre = 10.0f * log10f(g_bmx.lt_pre[i] + 1e-12f);
			pre_db[i] = pre;
			if (pre > g_bmx.al_ref[i]) g_bmx.al_ref[i] = pre;
			else g_bmx.al_ref[i] -= 0.5f;
			/* V13.8 : mémoire du risque LENTE (monte instantané, oublie
			 * 0,05 dB/s ≈ 3 dB/min) → tient une source longtemps */
			if (pre > g_bmx.risk[i]) g_bmx.risk[i] = pre;
			else g_bmx.risk[i] -= g_bmx.risk_decay;
			if (pre < -60.0f || pre < g_bmx.al_ref[i] - g_bmx.freeze_db) {
				g_bmx.act_ticks[i] = 0;   /* gel → prochaine
							   * activité = reprise */
				continue;   /* < 1/4 du max reçu : gelée (V13.9) */
			}
			act[i] = 1; any = 1;
			if (g_bmx.act_ticks[i] < 1000) g_bmx.act_ticks[i]++;
			/* V13.9 — solo v2 : ligne de base (monte τ 60 s, descend
			 * τ 20 s ; jamais entretenue par la voie en solo, sinon
			 * le solo remonterait sa propre base). */
			if (g_bmx.solo_base[i] < -500.0f)
				g_bmx.solo_base[i] = pre;
			else if (i != g_bmx.solo_src)
				g_bmx.solo_base[i] +=
					((pre > g_bmx.solo_base[i])
					 ? (1.0f / 60.0f) : (1.0f / 20.0f))
					* (pre - g_bmx.solo_base[i]);
			if (pre > Lmax) Lmax = pre;
			if (g_bmx.role[i] == BR_LEAD && pre > Llead) {
				Llead = pre; have_lead = 1;
			}
		}
		if (!any) return;
		float anchor_now = have_lead ? Llead : Lmax;
		/* ancre lissée ; gelée si pas de voix (have_lead=0 → on garde) */
		if (g_bmx.al_anchor < -110.0f) g_bmx.al_anchor = anchor_now;
		else if (have_lead)
			g_bmx.al_anchor += 0.30f * (anchor_now - g_bmx.al_anchor);
		else if (g_bmx.al_anchor < -110.0f)   /* jamais de voix : suit max */
			g_bmx.al_anchor += 0.10f * (anchor_now - g_bmx.al_anchor);

		/* V13.9 — DÉTECTION AUTO DE SOLO (v2) : voix muette ET une voie
		 * musique monte ≥ +6 dB AU-DESSUS DE SA PROPRE LIGNE DE BASE
		 * (médiane lente) — le vrai signal d'un soliste : il joue plus
		 * fort que lui-même, pas plus fort que la batterie (v1 ne se
		 * déclenchait jamais en solo accompagné). Engage 2 ticks ;
		 * release : voix de retour ou élévation < +3 dB (2 ticks). Ne
		 * touche jamais un solo posé MANUELLEMENT (bouton GUI). */
		if (g_bmx.solo_auto) {
			if (g_bmx.solo_src < 0) {
				int best = -1; float bex = 6.0f;
				for (int i = 0; i < N_EXP_CH; i++) {
					int r = g_bmx.role[i];
					if (!act[i] || r == BR_OFF ||
					    r == BR_LEAD || r == BR_CHOIR)
						continue;
					float ex = pre_db[i] - g_bmx.solo_base[i];
					if (ex > bex) { bex = ex; best = i; }
				}
				if (!have_lead && best >= 0) {
					g_bmx.solo_off_cnt = 0;
					if (++g_bmx.solo_on_cnt >= 2) {
						g_bmx.solo_src = best;
						g_bmx.solo_is_auto = 1;
						g_bmx.solo_on_cnt = 0;
					}
				} else
					g_bmx.solo_on_cnt = 0;
			} else if (g_bmx.solo_is_auto) {
				int s = g_bmx.solo_src;
				int keep = !have_lead && act[s] &&
					   (pre_db[s] - g_bmx.solo_base[s] > 3.0f);
				if (keep)
					g_bmx.solo_off_cnt = 0;
				else if (++g_bmx.solo_off_cnt >= 2) {
					g_bmx.solo_src = -1;
					g_bmx.solo_is_auto = 0;
					g_bmx.solo_off_cnt = 0;
				}
			}
		}

		for (int i = 0; i < N_EXP_CH; i++) {
			if (!act[i]) continue;
			/* V13.9 — SOLO : la voie monte à l'ancre + solo_db, sans
			 * plafond risque, slew rapide 3 dB/tick (montée ~2 s). */
			if (i == g_bmx.solo_src) {
				float tgt = g_bmx.al_anchor + g_bmx.solo_db
					  - pre_db[i];
				if (tgt >  18.0f) tgt =  18.0f;
				if (tgt < -24.0f) tgt = -24.0f;
				float d = tgt - g_bmx.kdb[i];
				if (d >  3.0f) d =  3.0f;
				if (d < -3.0f) d = -3.0f;
				g_bmx.kdb[i] += d;
				continue;
			}
			float place = g_bmx.al_anchor + BMX_P[g_bmx.role[i]].mix_db;
			float tgt = place - pre_db[i];
			/* V13.8/V13.9 — MÉMOIRE DU RISQUE, recentrée sur son but :
			 * l'ANTI-BLAST DE REPRISE. Le plafond (à son niveau fort
			 * mémorisé la source ne dépasse pas place+3) ne s'applique
			 * QUE dans les 5 premières secondes après un retour de
			 * silence. Une source qui JOUE en continu est équilibrée
			 * à sa place sans frein — sinon les instruments dynamiques
			 * (cuivres) restaient 8-10 dB sous leur place en permanence. */
			if (g_bmx.act_ticks[i] <= 5) {
				float risk_cap = place + g_bmx.risk_margin
					       - g_bmx.risk[i];
				if (tgt > risk_cap) tgt = risk_cap;
			}
			if (tgt >  18.0f) tgt =  18.0f;
			if (tgt < -24.0f) tgt = -24.0f;
			float d = tgt - g_bmx.kdb[i];   /* slew ≤1 dB/tick, zm 0,5 */
			if (d > 0.5f)  g_bmx.kdb[i] += (d > 1.0f ? 1.0f : d);
			if (d < -0.5f) g_bmx.kdb[i] += (d < -1.0f ? -1.0f : d);
		}

		/* V13.9 — BALANCE AUTO (table utilisateur). Ramène EN MÊME TEMPS le
		 * master à −14 LUFS ET l'écart voix−musique à +3 dB, en bougeant UN
		 * SEUL gain de groupe par tick selon le quadrant :
		 *   LUFS<−14 & E<3 → monter VOIX    | LUFS<−14 & E>3 → monter MUSIQUE
		 *   LUFS>−14 & E<3 → baisser MUSIQUE | LUFS>−14 & E>3 → baisser VOIX
		 * GEL : dans un creux (prog < crête récente −3 dB) ou groupe muet, on
		 * ne monte JAMAIS → fin de morceau / passage calme restent calmes.
		 * VOIX = LEAD · MUSIQUE = instruments. Les CHŒURS ont leur propre
		 * asservissement d'écart (bal_c_tgt, +1,5 dB) subordonné : leur gain
		 * suit musique+cible, même gel — ils ne pilotent pas le LUFS.
		 * Remplace le chase makeup. */
		if (g_bmx.balance_on) {
			/* RÈGLE UNIVERSELLE « pas de signal → on ne bouge rien » :
			 * chaque groupe n'est sommé que sur ses voies ACTIVES au sens
			 * du gel keeper (act[] : pre ≥ al_ref − freeze_db). Un groupe
			 * sans voie active = h?=0 → ses gains sont GELÉS (ni montée
			 * ni descente) — on n'asservit JAMAIS du bruit de fond. */
			float Pv = 0.0f, Pc = 0.0f, Pm = 0.0f;
			for (int i = 0; i < N_EXP_CH; i++) {
				int r = g_bmx.role[i];
				if (r == BR_OFF || !act[i]) continue;
				if      (r == BR_LEAD)  Pv += g_bmx.lt_ms[i];
				else if (r == BR_CHOIR) Pc += g_bmx.lt_ms[i];
				else                    Pm += g_bmx.lt_ms[i];
			}
			int hv = (Pv > 1e-6f), hm = (Pm > 1e-6f), hc = (Pc > 1e-6f);
			/* peak-hold du programme pour le gel (décroît 0,5 dB/s) */
			float prog = 10.0f * log10f(Pv + Pc + Pm + 1e-12f);
			if (prog > g_bmx.prog_peak) g_bmx.prog_peak = prog;
			else                        g_bmx.prog_peak -= 0.5f;
			/* « au niveau fort » = programme à moins de 3 dB sous sa crête
			 * récente. Sous ce seuil = creux (pause, fin, passage calme) :
			 * on n'autorise PLUS aucune MONTÉE (la descente reste permise). */
			int loud = (prog >= g_bmx.prog_peak - 3.0f);
			float lufs = atomic_load_explicit(&g_mk.lufs_c,
					memory_order_relaxed) * 0.01f;
			if (hv && hm && lufs > -50.0f) {
				/* E = écart RÉEL en sortie : loudness pré-présence (lt_ms)
				 * + les gains de groupe déjà appliqués → boucle fermée
				 * (sinon l'axe écart file aux butées). LUFS l'est déjà. */
				float E = (10.0f * log10f(Pv) - 10.0f * log10f(Pm))
					+ (g_bmx.g_voice_db - g_bmx.g_music_db);
				float lerr = lufs - g_bmx.bal_lufs_tgt;   /* >0 trop fort */
				float eerr = E - g_bmx.bal_e_tgt;          /* >0 voix haute */
				const float DB = 1.0f;                     /* deadband */
				/* STAGING INITIAL (exigence scène : volume utilisable
				 * tout de suite) : 8 dB/s jusqu'au 1er lock ±2 dB,
				 * puis vitesses douces 3/1 dB/s (anti-pompage). */
				if (!g_bmx.bal_staged && fabsf(lerr) <= 2.0f)
					g_bmx.bal_staged = 1;
				float st = !g_bmx.bal_staged ? 8.0f
					 : (fabsf(lerr) > 6.0f) ? 3.0f : 1.0f;
				float dv = 0.0f, dm = 0.0f;
				if (lerr < -DB) {              /* trop faible → MONTER */
					if (eerr > DB) dm = +st;      /* voix trop haute → musique */
					else           dv = +st;      /* sinon → voix */
				} else if (lerr > DB) {       /* trop fort → BAISSER */
					if (eerr > DB) dv = -st;      /* voix trop haute → voix */
					else           dm = -st;      /* sinon → musique */
				} else {                      /* LUFS ok → écart seul */
					if      (eerr >  DB) dv = -1.0f;
					else if (eerr < -DB) dv = +1.0f;
				}
				/* GEL DES MONTÉES dans un creux : ne JAMAIS monter quand
				 * le programme baisse (fin de morceau / passage calme). */
				if (!loud) { if (dv > 0.0f) dv = 0.0f;
					     if (dm > 0.0f) dm = 0.0f; }
				/* garde silence : ne JAMAIS monter un groupe muet */
				if (dv > 0.0f && !hv) dv = 0.0f;
				if (dm > 0.0f && !hm) dm = 0.0f;
				g_bmx.g_voice_db += dv;
				g_bmx.g_music_db += dm;
				if (g_bmx.g_voice_db >  36.0f) g_bmx.g_voice_db =  36.0f;
				if (g_bmx.g_voice_db < -24.0f) g_bmx.g_voice_db = -24.0f;
				if (g_bmx.g_music_db >  36.0f) g_bmx.g_music_db =  36.0f;
				if (g_bmx.g_music_db < -24.0f) g_bmx.g_music_db = -24.0f;
			}
			/* CHŒURS : asservissement d'écart subordonné — tient les
			 * chœurs à musique + bal_c_tgt (boucle fermée, slew ≤1 dB/
			 * tick), même gel : jamais de montée dans un creux. */
			if (hc && hm && lufs > -50.0f) {
				float Ec = (10.0f * log10f(Pc) - 10.0f * log10f(Pm))
					 + (g_bmx.g_choir_db - g_bmx.g_music_db);
				float d = g_bmx.bal_c_tgt - Ec;   /* >0 → monter */
				float cs = g_bmx.bal_staged ? 1.0f : 8.0f;
				if (d >  cs) d =  cs;
				if (d < -cs) d = -cs;
				if (d > 0.0f && !loud) d = 0.0f;  /* gel des montées */
				g_bmx.g_choir_db += d;
				if (g_bmx.g_choir_db >  36.0f) g_bmx.g_choir_db =  36.0f;
				if (g_bmx.g_choir_db < -24.0f) g_bmx.g_choir_db = -24.0f;
			}
		}

		/* V13.6 : compresseur auto par rôle, seuil ADAPTATIF (al_ref =
		 * mémoire de crête → tient le trop-fort, agit au sample). */
		pthread_mutex_lock(&g_st.target_lock);
		for (int i = 0; i < N_EXP_CH; i++) {
			g_st.keeper_target[i] = powf(10.0f, g_bmx.kdb[i] / 20.0f);
			int r = g_bmx.role[i];
			/* V13.9 — gain de groupe (balance auto) : lead/chœurs/musique */
			g_st.presence_target[i] =
				(!g_bmx.balance_on || r == BR_OFF) ? 1.0f
				: (r == BR_LEAD)
					? powf(10.0f, g_bmx.g_voice_db / 20.0f)
				: (r == BR_CHOIR)
					? powf(10.0f, g_bmx.g_choir_db / 20.0f)
					: powf(10.0f, g_bmx.g_music_db / 20.0f);
			/* V13.9 — GATE AUTO : seuil adaptatif = al_ref − gate_db
			 * (al_ref = crête mémorisée de LA voie → le seuil suit la
			 * source ; la repisse, bien plus basse, n'ouvre pas).
			 * Rôles gate_on seulement (LEAD/CHŒURS/KICK/SNARE) ;
			 * ré-armé chaque tick (état préservé si déjà on). */
			if (r != BR_OFF && BMX_P[r].gate_on) {
				float gthr = g_bmx.al_ref[i] - g_bmx.gate_db;
				if (gthr < -80.0f) gthr = -80.0f;
				if (gthr > -20.0f) gthr = -20.0f;
				exp_configure(i, 1, gthr, BMX_P[r].gate_ratio,
					      2.0f, 150.0f, 40.0f,
					      BMX_P[r].gate_hold);
			} else if (g_exp[i].on) {
				/* rôle sans gate (ou off) : DÉSARME la gate auto
				 * héritée d'un rôle précédent (symétrique au comp
				 * — sinon gate fantôme après changement de rôle) */
				exp_configure(i, 0, g_exp[i].thr_db,
					      g_exp[i].ratio, g_exp[i].atk_ms,
					      g_exp[i].rel_ms, g_exp[i].range_db,
					      g_exp[i].hold_ms);
			}
			if (r == BR_OFF || !BMX_P[r].comp_on) {
				if (g_cmp[i].on)
					cmp_configure(i, 0, g_cmp[i].thr_db,
						g_cmp[i].ratio, g_cmp[i].atk_ms,
						g_cmp[i].rel_ms, g_cmp[i].makeup_db);
				continue;
			}
			float thr = g_bmx.al_ref[i] + COMP_OFF[r];
			if (thr < -50.0f) thr = -50.0f;
			if (thr >  -3.0f) thr =  -3.0f;
			cmp_configure(i, 1, thr, BMX_P[r].c_ratio,
				      BMX_P[r].c_atk, BMX_P[r].c_rel, 0.0f);
		}
		pthread_mutex_unlock(&g_st.target_lock);
		/* V13.7 — makeup LUFS. V13.9 : si la BALANCE AUTO est active, c'est
		 * ELLE qui tient −14 LUFS (via les gains de groupe) → le makeup reste
		 * NEUTRE. Deux correcteurs sur le même LUFS = pompage : on n'en garde
		 * qu'un. Le chase makeup ne sert que si la balance est coupée. */
		if (g_bmx.balance_on) {
			g_mk.mk_db = 0.0f;
			atomic_store_explicit(&g_mk.makeup_mq, 1000,
					      memory_order_relaxed);
		} else {
			float lufs = atomic_load_explicit(&g_mk.lufs_c,
					memory_order_relaxed) * 0.01f;
			if (lufs > -50.0f) {
				float want = MASTER_LUFS_TGT - lufs + g_mk.mk_db;
				if (want > MASTER_MK_MAX_DB) want = MASTER_MK_MAX_DB;
				if (want < MASTER_MK_MIN_DB) want = MASTER_MK_MIN_DB;
				float d = want - g_mk.mk_db;
				/* slew adaptatif : 4 dB/s si loin, 1 dB/s près (anti-pompage) */
				float lim = (fabsf(d) > 4.0f) ? 4.0f : 1.0f;
				if (d >  lim) d =  lim;
				if (d < -lim) d = -lim;
				g_mk.mk_db += d;
				atomic_store_explicit(&g_mk.makeup_mq,
					(int)(powf(10.0f, g_mk.mk_db / 20.0f) * 1000.0f),
					memory_order_relaxed);
			}
		}
		return;
	}

	/* 4b. keeper verrouillé : parts courantes vs référence lockée, ±3 dB,
	 * zone morte 1 dB, tranches silencieuses ignorées, priorité voix */
	if (!g_bmx.live || !g_bmx.ref_valid)
		return;
	double tot = 1e-12;
	for (int i = 0; i < N_EXP_CH; i++)
		if (g_bmx.role[i] != BR_OFF)
			tot += g_bmx.lt_ms[i];
	float lead_err = 0.0f;
	for (int i = 0; i < N_EXP_CH; i++)
		if (g_bmx.role[i] == BR_LEAD && g_bmx.ref_share[i] > 1e-9f)
			lead_err = 10.0f * log10f(
				(float)(g_bmx.lt_ms[i] / tot) /
				g_bmx.ref_share[i] + 1e-12f);
	for (int i = 0; i < N_EXP_CH; i++) {
		if (g_bmx.role[i] == BR_OFF || g_bmx.ref_share[i] < 1e-9f)
			continue;
		float pre_db = 10.0f * log10f(g_bmx.lt_pre[i] + 1e-12f);
		float thr_sil = g_bmx.m[i].done
			? fminf(g_bmx.m[i].floor_db + 6.0f,
				g_bmx.m[i].rms_avg_db - 15.0f)
			: -70.0f;
		if (pre_db < thr_sil)
			continue;
		float err = 10.0f * log10f(
			(float)(g_bmx.lt_ms[i] / tot) / g_bmx.ref_share[i]
			+ 1e-12f);
		if (err > -1.0f && err < 1.0f)
			continue;
		float step = 0.5f;
		if (g_bmx.role[i] == BR_LEAD && lead_err < -2.0f)
			step = 1.0f;
		else if (lead_err < -2.0f && err > 0.0f)
			step = 0.25f;
		g_bmx.kdb[i] += (err > 0 ? -step : step);
		if (g_bmx.kdb[i] > 3.0f)  g_bmx.kdb[i] = 3.0f;
		if (g_bmx.kdb[i] < -3.0f) g_bmx.kdb[i] = -3.0f;
	}
	pthread_mutex_lock(&g_st.target_lock);
	for (int i = 0; i < N_EXP_CH; i++)
		g_st.keeper_target[i] = powf(10.0f, g_bmx.kdb[i] / 20.0f);
	pthread_mutex_unlock(&g_st.target_lock);
}

/* --- CALCULER LE MIX : applique staging + gate + comp + faders --- */
static void bmx_calc(void)
{
	for (int i = 0; i < N_EXP_CH; i++) {
		int r = g_bmx.role[i];
		if (r == BR_OFF || !g_bmx.m[i].done)
			continue;
		const struct bmx_preset *P = &BMX_P[r];
		const struct bmx_meas *m = &g_bmx.m[i];
		/* gain staging + cible de mix : rms → −20 dBFS puis offset
		 * de rôle, le tout dans le fader (sémantique console E7.2),
		 * garde-crête −6 dBFS (crête ≈ rms_max, marge 3 dB) */
		float g_db = (-20.0f - m->rms_avg_db) + P->mix_db - 6.0f;
		if (m->peak_db + g_db > -6.0f)
			g_db = -6.0f - m->peak_db;
		float g = powf(10.0f, g_db / 20.0f);
		if (g < 0.02f) g = 0.02f;
		if (g > 4.0f)  g = 4.0f;
		pthread_mutex_lock(&g_st.target_lock);
		g_st.input_target[i] = g;
		/* gate : seuil = floor mesuré + 8 dB, plafonné rms − 10 */
		if (P->gate_on) {
			float thr = m->floor_db + 8.0f;
			if (thr > m->rms_avg_db - 10.0f)
				thr = m->rms_avg_db - 10.0f;
			if (thr < -80.0f) thr = -80.0f;
			exp_configure(i, 1, thr, P->gate_ratio, 2.0f,
				      150.0f, 40.0f, P->gate_hold);
		} else {
			exp_configure(i, 0, g_exp[i].thr_db, g_exp[i].ratio,
				      g_exp[i].atk_ms, g_exp[i].rel_ms,
				      g_exp[i].range_db, g_exp[i].hold_ms);
		}
		/* comp : preset de rôle (seuil relatif au niveau stagé) */
		if (P->comp_on)
			cmp_configure(i, 1, P->c_thr, P->c_ratio,
				      P->c_atk, P->c_rel, 0.0f);
		else
			cmp_configure(i, 0, g_cmp[i].thr_db, g_cmp[i].ratio,
				      g_cmp[i].atk_ms, g_cmp[i].rel_ms,
				      g_cmp[i].makeup_db);
		pthread_mutex_unlock(&g_st.target_lock);
	}
	atomic_store(&g_presets_dirty, 1);
	mlog("bandmix: mix calculé");
}

/* ========= V13-VFOCUS — « place à la voix » (unmasking spectral) =========
 * Dynamic EQ sidechainé : la musique (tranches rôle instrument du
 * bandmix) est creusée UNIQUEMENT dans les bandes où la voix (tranches
 * rôle lead/choir) a de l'énergie, UNIQUEMENT quand elle chante.
 * 5 bandes peaking RBJ fixes (250/500/1k/2k/4k, Q 1,4) : analyse =
 * passe-bande fixes sur le sidechain voix (post-fader) ; application =
 * MÊMES 5 gains pour toutes les tranches musique → coefs recalculés UNE
 * fois par bloc, 5 biquads cascade par tranche (états par tranche×bande).
 * Zéro alloc, zéro transcendante par sample. ARCHI_V13_VOICEFOCUS.md. */
#define VF_BANDS 5

/* L'ancienne « struct vf_bq » (champs identiques) a été fusionnée dans
 * struct eqx_bq (revue 2026-07-28, lot 5b) : UN seul type de biquad RBJ
 * dans tout le moteur. */

static struct {
	int   on;
	float amount;                    /* 0..1 */
	float max_cut_db;                /* profondeur max (défaut 4,5) */
	/* précalculs par bande (fréquences fixes) */
	float cw[VF_BANDS], alpha[VF_BANDS];   /* cos(w0), alpha(Q=1,4) */
	struct eqx_bq ana[VF_BANDS];      /* passe-bande analyse (fixes) */
	struct eqx_bq cut[VF_BANDS];      /* peaking application (par bloc) */
	/* états */
	float az[VF_BANDS][2];           /* biquads analyse */
	float env[VF_BANDS];             /* enveloppes bande (crête lissée) */
	float env_wb;                    /* large bande (activité voix) */
	float cut_db[VF_BANDS];          /* cuts lissés (≥ 0 = creuse) */
	float st[N_EXP_CH][VF_BANDS][2]; /* biquads application */
	_Atomic uint32_t pub_cut[VF_BANDS];  /* milli-dB (GUI) */
	_Atomic int active;
} g_vf = { .amount = 0.5f, .max_cut_db = 4.5f };

static void vf_init(void)
{
	static const float FR[VF_BANDS] = { 250, 500, 1000, 2000, 4000 };
	for (int b = 0; b < VF_BANDS; b++) {
		float w = 2.0f * (float)M_PI * FR[b] / (float)SAMPLE_RATE;
		float sw = sinf(w);
		g_vf.cw[b] = cosf(w);
		g_vf.alpha[b] = sw / (2.0f * 1.4f);   /* Q = 1,4 */
		/* passe-bande RBJ (pic 0 dB) */
		float a0 = 1.0f + g_vf.alpha[b];
		g_vf.ana[b].b0 = g_vf.alpha[b] / a0;
		g_vf.ana[b].b1 = 0.0f;
		g_vf.ana[b].b2 = -g_vf.alpha[b] / a0;
		g_vf.ana[b].a1 = -2.0f * g_vf.cw[b] / a0;
		g_vf.ana[b].a2 = (1.0f - g_vf.alpha[b]) / a0;
		g_vf.cut[b] = (struct eqx_bq){ 1, 0, 0, 0, 0 };   /* neutre */
	}
}

/* peaking RBJ, gain −cut_db — même NOYAU rbj_peak_core que l'eqx (lot 5b),
 * avec cos/alpha précalculés à l'init → qq mults par bloc (chemin RT) */
static inline void vf_peak_coefs(int b, float cut_db)
{
	rbj_peak_core(&g_vf.cut[b], g_vf.cw[b], g_vf.alpha[b],
		      powf(10.0f, -cut_db / 40.0f));
}

static inline int vf_is_voice(int i)
{
	return g_bmx.role[i] == BR_LEAD || g_bmx.role[i] == BR_CHOIR;
}
static inline int vf_is_music(int i)
{
	int r = g_bmx.role[i];
	return r >= BR_KICK && r <= BR_LINE;
}

/* Rendu (audio_thread, SOUS target_lock, après cmp_render) */
static void duck_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	if (!g_vf.on) {
		if (atomic_load_explicit(&g_vf.active, memory_order_relaxed))
			atomic_store(&g_vf.active, 0);
		return;
	}

	/* 1. sidechain voix = somme post-fader des tranches lead/choir */
	static float sc[PERIOD_FRAMES];
	memset(sc, 0, sizeof(sc));
	int nvoice = 0;
	for (int i = 0; i < N_EXP_CH; i++) {
		if (!vf_is_voice(i))
			continue;
		nvoice++;
		const float g = g_st.input_gain[i];
		for (int f = 0; f < PERIOD_FRAMES; f++)
			sc[f] += in_block[i][f] * g;
	}

	/* 2. enveloppes : large bande + par bande (crête, att 5 ms/rel 180) */
	float pk_wb = 0.0f, pk_b[VF_BANDS] = { 0 };
	if (nvoice) {
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			float v = sc[f] < 0 ? -sc[f] : sc[f];
			if (v > pk_wb) pk_wb = v;
		}
		for (int b = 0; b < VF_BANDS; b++) {
			const struct eqx_bq *q = &g_vf.ana[b];
			float z1 = g_vf.az[b][0], z2 = g_vf.az[b][1];
			float m = 0.0f;
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float x = sc[f];
				float y = q->b0 * x + z1;
				z1 = q->b1 * x - q->a1 * y + z2;
				z2 = q->b2 * x - q->a2 * y;
				float v = y < 0 ? -y : y;
				if (v > m) m = v;
			}
			g_vf.az[b][0] = z1; g_vf.az[b][1] = z2;
			pk_b[b] = m;
		}
	}
	const float ka = 0.33f, kr = 0.011f;   /* 5 ms / 180 ms (blocs 2 ms) */
	g_vf.env_wb += (pk_wb > g_vf.env_wb ? ka : kr) * (pk_wb - g_vf.env_wb);
	float emax = 1e-12f;
	for (int b = 0; b < VF_BANDS; b++) {
		g_vf.env[b] += (pk_b[b] > g_vf.env[b] ? ka : kr)
			       * (pk_b[b] - g_vf.env[b]);
		if (g_vf.env[b] > emax) emax = g_vf.env[b];
	}

	/* 3. cuts cibles : voix active → proportionnel à la bande dominante */
	int act = g_vf.env_wb > 0.005623f;   /* −45 dBFS */
	atomic_store_explicit(&g_vf.active, act, memory_order_relaxed);
	const float kca = 0.18f, kcr = 0.01f;   /* 10 ms / 200 ms */
	int any = 0;
	for (int b = 0; b < VF_BANDS; b++) {
		float tgt = act ? g_vf.max_cut_db * g_vf.amount
				  * (g_vf.env[b] / emax) : 0.0f;
		g_vf.cut_db[b] += (tgt > g_vf.cut_db[b] ? kca : kcr)
				  * (tgt - g_vf.cut_db[b]);
		if (g_vf.cut_db[b] > 0.05f) {
			vf_peak_coefs(b, g_vf.cut_db[b]);
			any = 1;
		}
		atomic_store_explicit(&g_vf.pub_cut[b],
				      (uint32_t)(g_vf.cut_db[b] * 1000.0f),
				      memory_order_relaxed);
	}
	if (!any || !nvoice)
		return;

	/* 4. application : 5 peaking cascade sur les tranches musique */
	for (int i = 0; i < N_EXP_CH; i++) {
		if (!vf_is_music(i))
			continue;
		float *x = in_block[i];
		for (int b = 0; b < VF_BANDS; b++) {
			if (g_vf.cut_db[b] <= 0.05f)
				continue;
			const struct eqx_bq *q = &g_vf.cut[b];
			float z1 = g_vf.st[i][b][0], z2 = g_vf.st[i][b][1];
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float xi = x[f];
				float y = q->b0 * xi + z1;
				z1 = q->b1 * xi - q->a1 * y + z2;
				z2 = q->b2 * xi - q->a2 * y;
				x[f] = y;
			}
			g_vf.st[i][b][0] = z1; g_vf.st[i][b][1] = z2;
		}
	}
}

/* ========= V12-MIDIX — expandeur MIDI (consumer du ring SHM) =========
 * Le daemon midi-expander (fluidsynth, cores 0-1) rend le son du module
 * MIDI dans /dev/shm/ala-midix ; l'audio_thread le pop (non-bloquant,
 * zéros si retard/absent) et l'ADDITIONNE dans P1/P2 comme le sampleur
 * et le looper. mmap fait par persistence_thread (1 Hz, jamais en RT).
 * ARCHI_V12_MIDI_EXPANDER.md. */
#define MIDIX_SHM   "/ala-midix"
#define MIDIX_MAGIC 0x4D494458u

struct midix_hdr {
	uint32_t magic;
	uint32_t ring_frames;
	_Atomic uint32_t widx;
	uint32_t _pad;
};
static struct {
	struct midix_hdr *_Atomic hdr;   /* NULL tant que non mappé */
	float   *data;
	size_t   map_sz;
	uint32_t ridx;                    /* cursor consumer privé */
	float    gain;
	_Atomic uint32_t underruns;
	_Atomic uint32_t peak;
} g_midix = { .gain = 1.0f };

/* persistence_thread (1 Hz) — tente le mmap tant que le daemon n'est pas
 * là ; invalide si le magic disparaît (arrêt propre du daemon). */
static void midix_try_map(void)
{
	struct midix_hdr *h = atomic_load(&g_midix.hdr);
	if (h) {
		if (h->magic != MIDIX_MAGIC) {   /* daemon parti */
			atomic_store(&g_midix.hdr, NULL);
			munmap(h, g_midix.map_sz);
			g_midix.data = NULL;
			mlog("midix: ring invalidé (daemon arrêté)");
		}
		return;
	}
	int fd = shm_open(MIDIX_SHM, O_RDONLY, 0);
	if (fd < 0)
		return;
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(*h)) {
		close(fd);
		return;
	}
	void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		return;
	h = m;
	if (h->magic != MIDIX_MAGIC || !h->ring_frames ||
	    (off_t)(sizeof(*h) + (size_t)h->ring_frames * 2 * sizeof(float))
	    > st.st_size) {
		munmap(m, (size_t)st.st_size);
		return;
	}
	g_midix.map_sz = (size_t)st.st_size;
	g_midix.data = (float *)((char *)m + sizeof(*h));
	g_midix.ridx = atomic_load(&h->widx);   /* démarre au présent */
	atomic_store_explicit(&g_midix.hdr, h, memory_order_release);
	mlog("midix: ring mappé (%u frames)", h->ring_frames);
}

/* Rendu (audio_thread, SOUS target_lock, après loop_render) */
static void midix_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	struct midix_hdr *h = atomic_load_explicit(&g_midix.hdr,
						   memory_order_acquire);
	if (!h)
		return;
	uint32_t w = atomic_load_explicit(&h->widx, memory_order_acquire);
	int32_t avail = (int32_t)(w - g_midix.ridx);
	if (avail < PERIOD_FRAMES) {   /* producer en retard → silence */
		atomic_fetch_add_explicit(&g_midix.underruns, 1,
					  memory_order_relaxed);
		return;
	}
	/* dérive/burst : si on traîne trop, on saute au présent */
	if (avail > (int32_t)(h->ring_frames / 2))
		g_midix.ridx = w - PERIOD_FRAMES;

	const int P = N_INPUT_MICS + N_INPUT_STEMS;   /* P1 = 16 */
	const uint32_t ring = h->ring_frames;
	const float g = g_midix.gain;
	float pk = 0.0f;
	for (int f = 0; f < PERIOD_FRAMES; f++) {
		uint32_t idx = (g_midix.ridx + f) % ring;
		float l = g_midix.data[(size_t)idx * 2];
		float r = g_midix.data[(size_t)idx * 2 + 1];
		in_block[P][f]     += l * g;
		in_block[P + 1][f] += r * g;
		float a = l < 0 ? -l : l, b = r < 0 ? -r : r;
		if (a > b) b = a;
		if (b > pk) pk = b;
	}
	g_midix.ridx += PERIOD_FRAMES;
	atomic_store_explicit(&g_midix.peak,
			      (uint32_t)(pk * g * 2147483647.0f),
			      memory_order_relaxed);
}

/* V12-AMX — calcul Dugan par bloc (appelé par audio_thread AVANT mix_block).
 * Énergie post-fader : e_i = mean(x²) × ig². Enveloppe asymétrique
 * (attack 10 ms, release 200 ms — parole). Cible : part d'énergie
 * pondérée, plancher automix_floor, non-membres ≡ 1.0. */
static void automix_update(const float in_block[N_INPUT_REAL][PERIOD_FRAMES],
			   uint32_t N)
{
	if (!g_st.automix_on)
		return;
	const float ka = 1.0f - expf(-2.0f / 10.0f);    /* attack 10 ms/2 ms */
	const float kr = 1.0f - expf(-2.0f / 200.0f);   /* release 200 ms */
	float wsum = 0.0f;

	for (int i = 0; i < N_INPUT_REAL; i++) {
		if (!g_st.automix_member[i])
			continue;
		float acc = 0.0f;
		const float *x = in_block[i];
		for (uint32_t f = 0; f < N; f++)
			acc += x[f] * x[f];
		const float ig = g_st.input_gain[i];
		float e = (acc / (float)N) * ig * ig;
		if (g_st.mute_mask & (1u << i))
			e = 0.0f;
		float *env = &g_st.automix_env[i];
		*env += (e > *env ? ka : kr) * (e - *env);
		wsum += *env * g_st.automix_weight[i];
	}

	const float eps = 1e-12f;
	for (int i = 0; i < N_INPUT_REAL; i++) {
		if (!g_st.automix_member[i]) {
			g_st.automix_gtarget[i] = 1.0f;
			continue;
		}
		float share = (g_st.automix_env[i] * g_st.automix_weight[i] + eps)
			      / (wsum + eps * 8.0f);
		/* Dugan : atténuation en dB = 10·log10(part d'énergie) →
		 * multiplicateur d'AMPLITUDE = sqrt(part). 2 micros égaux =
		 * −3 dB chacun (NOM constant), conforme au standard. */
		float g = sqrtf(share);
		if (g < g_st.automix_floor)
			g = g_st.automix_floor;
		if (g > 1.0f)
			g = 1.0f;
		g_st.automix_gtarget[i] = g;
	}
}

/* V9.3 : mix_block — process N samples en 1 passe (vs mix_frame × N).
 *
 * Buffers in/out organisés par channel-major (in[ch][frame]) pour permettre
 * au compilo d'auto-vectoriser les boucles inner `for (f=0..N-1)` en NEON.
 *
 * Phases :
 *   A. Sends : in_block[26][N] × send_gain[26][8] → bus_in[8][N]
 *   B. FX    : fx_engines[b].process_block(bus_in, bus_out, N) × 4 bus
 *      → bus_out[8][N] (post-FX), puis × fx_bus_gain → ret_block[8][N]
 *   C. Master: (in_block + ret_block) × master_gain[34][18] → out_block[18][N]
 *
 * Tous les paramètres (input_gain, send_gain, master_gain, fx_bus_gain) sont
 * lus une fois en début de block (snapshot post-smooth_gains). Pour smooth
 * intra-block sur des changements rapides, voir TODO V9.4.
 */
/* V9.3.1 : buffers internes mix_block en static BSS (pas stack).
 * Appelée uniquement depuis audio_thread (1 thread), donc thread-safe sans lock.
 * Taille : 3 × N_BUS_FX_CH × PERIOD_FRAMES × 4 + N_RETURN_CH × PERIOD_FRAMES × 4
 *       = 3 × 8 × 96 × 4 + 8 × 96 × 4 = 12288 octets = 12 KB en BSS. */
static float g_mix_bus_in[N_BUS_FX_CH][PERIOD_FRAMES];
static float g_mix_bus_out[N_BUS_FX_CH][PERIOD_FRAMES];
static float g_mix_ret[N_RETURN_CH][PERIOD_FRAMES];

/* V9.3.2 : NEON intrinsics pour les boucles inner du mix.
 * aarch64 a NEON nativement (toujours dispo). PERIOD_FRAMES=96 = multiple de 4
 * → pas de tail handling. Gain attendu × 3-4 sur les matrices send + master.
 *
 * Helper inline : dst[f] += src[f] * g pour f=0..N-1, N multiple de 4.
 * vmlaq_f32(a, b, c) = a + b * c (multiply-accumulate sur 4 floats). */
#include <arm_neon.h>

static inline void mac_block_n4(float *dst, const float *src, float g, uint32_t N)
{
	float32x4_t vg = vdupq_n_f32(g);
	for (uint32_t f = 0; f < N; f += 4) {
		float32x4_t vs = vld1q_f32(src + f);
		float32x4_t vd = vld1q_f32(dst + f);
		vd = vmlaq_f32(vd, vs, vg);
		vst1q_f32(dst + f, vd);
	}
}

/* dst[f] = src[f] * g pour f=0..N-1 (multiply, pas accumulate). */
static inline void mul_block_n4(float *dst, const float *src, float g, uint32_t N)
{
	float32x4_t vg = vdupq_n_f32(g);
	for (uint32_t f = 0; f < N; f += 4) {
		float32x4_t vs = vld1q_f32(src + f);
		vst1q_f32(dst + f, vmulq_f32(vs, vg));
	}
}

static void mix_block(const float in_block[N_INPUT_REAL][PERIOD_FRAMES],
		      float out_block[N_OUTPUT_TOTAL][PERIOD_FRAMES],
		      float bus_pre_out[N_BUS_FX_CH][PERIOD_FRAMES],
		      float ret_post_out[N_RETURN_CH][PERIOD_FRAMES],
		      uint32_t N)
{
	/* Phase A : Sends 26→8 (block). */
	for (int b = 0; b < N_BUS_FX_CH; b++)
		memset(g_mix_bus_in[b], 0, sizeof(float) * N);

	for (int i = 0; i < N_INPUT_REAL; i++) {
		if (g_st.mute_mask & (1u << i))
			continue;
		/* V12-AMX/V13 : auto-gain + keeper composent avec le fader
		 * (≡1 hors automix / hors live) */
		const float ig = g_st.input_gain[i] * g_st.automix_gain[i]
				 * g_st.keeper_gain[i];
		for (int b = 0; b < N_BUS_FX_CH; b++) {
			const float g = ig * g_st.send_gain[i][b];
			if (g == 0.0f) continue;   /* sparse skip */
			/* V9.3.2 : NEON mac_block. dst += src * g sur N samples. */
			mac_block_n4(g_mix_bus_in[b], in_block[i], g, N);
		}
	}

	/* Snapshot pour peak meters bus pre-FX */
	if (bus_pre_out) {
		for (int b = 0; b < N_BUS_FX_CH; b++)
			memcpy(bus_pre_out[b], g_mix_bus_in[b], sizeof(float) * N);
	}

	/* Phase B : FX process_block × 4 bus stéréo */
	for (int b = 0; b < N_BUS_FX; b++) {
		g_st.fx_engines[b].process_block(&g_st.fx_engines[b],
			g_mix_bus_in[b * 2], g_mix_bus_in[b * 2 + 1],
			g_mix_bus_out[b * 2], g_mix_bus_out[b * 2 + 1],
			N);
	}

	/* Phase B.5 : fx_bus_gain post-effet → ret_block (NEON mul) */
	for (int b = 0; b < N_BUS_FX_CH; b++) {
		mul_block_n4(g_mix_ret[b], g_mix_bus_out[b], g_st.fx_bus_gain[b], N);
	}
	if (ret_post_out) {
		for (int s = 0; s < N_RETURN_CH; s++)
			memcpy(ret_post_out[s], g_mix_ret[s], sizeof(float) * N);
	}

	/* Phase C : Master 26 sources → 18 outputs */
	for (int o = 0; o < N_OUTPUT_TOTAL; o++)
		memset(out_block[o], 0, sizeof(float) * N);

	/* V9.3.2 : NEON mac sur tout master matrix.
	 * Inputs réels 0..17 */
	for (int s = 0; s < N_INPUT_REAL; s++) {
		if (g_st.mute_mask & (1u << s))
			continue;
		/* V12-AMX/V13 : idem phase A — cohérence sends/master.
		 * V13.9 : × presence_gain (balance auto voix/musique). */
		const float ig = g_st.input_gain[s] * g_st.automix_gain[s]
				 * g_st.keeper_gain[s] * g_st.presence_gain[s];
		const float *src = in_block[s];
		for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
			const float g = ig * g_st.master_gain[s][o];
			if (g == 0.0f) continue;
			mac_block_n4(out_block[o], src, g, N);
		}
	}
	/* Returns 18..25 */
	for (int s = 0; s < N_RETURN_CH; s++) {
		int src_idx = N_INPUT_REAL + s;
		if (g_st.mute_mask & (1u << src_idx))
			continue;
		const float *src = g_mix_ret[s];
		for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
			const float g = g_st.master_gain[src_idx][o];
			if (g == 0.0f) continue;
			mac_block_n4(out_block[o], src, g, N);
		}
	}
}

/* ===== V13.9 — VOICE SPATIALIZER : widener décorrélé (Lauridsen) =====
 * Élargit la voix (rôles LEAD + CHŒURS) sans la décentrer : on somme la voix
 * telle qu'elle apparaît au master (mono, centre), on en dérive un « side »
 * décorrélé = copie retardée (~18 ms), et on l'injecte ±dans out0/out1 APRÈS
 * mix_block et AVANT l'EQ/limiter master. Mono-compatible (out0+out1 annule le
 * side → repli mono = mix d'origine). amount=0 → strictement transparent.
 * RT : somme voix en NEON (mac_block_n4), boucle retard/inject légère (96 it). */
#define VSPAT_RING     4096          /* ≥ délai max (85 ms @48k = 4080) */
#define VSPAT_DLY_DEF  18            /* ms par défaut */
static struct {
	_Atomic int   on;
	_Atomic int   amount_mq;     /* cible ×1000 (0..1000 → gain side 0..1) */
	_Atomic int   delay_smp;     /* retard en samples */
	float         amount_cur;    /* gain lissé (audio_thread) */
	int           wpos;
	float         ring[VSPAT_RING];
} g_vspat = { .amount_mq = 500,
	      .delay_smp = (VSPAT_DLY_DEF * SAMPLE_RATE) / 1000 };

static void vspat_render(const float in_block[N_INPUT_REAL][PERIOD_FRAMES],
			 float out_block[N_OUTPUT_TOTAL][PERIOD_FRAMES], int N)
{
	int on = atomic_load_explicit(&g_vspat.on, memory_order_relaxed);
	float tgt = on ? atomic_load_explicit(&g_vspat.amount_mq,
					      memory_order_relaxed) / 1000.0f : 0.0f;
	/* court-circuit total quand inactif ET déjà éteint (transparent) */
	if (tgt == 0.0f && g_vspat.amount_cur < 1e-4f) {
		g_vspat.amount_cur = 0.0f;
		return;
	}
	int dly = atomic_load_explicit(&g_vspat.delay_smp, memory_order_relaxed);
	if (dly < 1) dly = 1;
	if (dly > VSPAT_RING - PERIOD_FRAMES) dly = VSPAT_RING - PERIOD_FRAMES;

	/* voix telle qu'elle sort au master (mono, centre) : somme des voies
	 * LEAD/CHŒURS avec leur gain effectif vers out0 — accumulation NEON. */
	static float vbus[PERIOD_FRAMES];
	memset(vbus, 0, sizeof(float) * N);
	for (int s = 0; s < N_EXP_CH; s++) {
		int r = g_bmx.role[s];
		if (r != BR_LEAD && r != BR_CHOIR) continue;
		if (g_st.mute_mask & (1u << s)) continue;
		float ig = g_st.input_gain[s] * g_st.automix_gain[s]
			 * g_st.keeper_gain[s] * g_st.master_gain[s][0];
		if (ig == 0.0f) continue;
		mac_block_n4(vbus, in_block[s], ig, N);
	}

	/* retard + injection ±side (séquentiel à cause du ring, mais 96 it).
	 * NEUTRE EN LOUDNESS : on réduit la voix sèche de (1−c) tout en ajoutant
	 * le side ±a·vd, avec c=√(1−a²) → puissance de la voix PAR CANAL
	 * constante (c²+a²=1). On gagne la largeur sans monter le niveau voix.
	 * (a=0 → c=1 : strictement transparent). */
	const float ka = 1.0f / (0.02f * SAMPLE_RATE);   /* slew ~20 ms */
	float *o0 = out_block[0], *o1 = out_block[1];
	for (int f = 0; f < N; f++) {
		g_vspat.ring[g_vspat.wpos] = vbus[f];
		int rp = g_vspat.wpos - dly;
		if (rp < 0) rp += VSPAT_RING;
		float vd = g_vspat.ring[rp];
		if (++g_vspat.wpos >= VSPAT_RING) g_vspat.wpos = 0;

		g_vspat.amount_cur += (tgt - g_vspat.amount_cur) * ka;
		float a = g_vspat.amount_cur;
		float c = sqrtf(1.0f - a * a);       /* a∈[0,1] → c∈[1,0] */
		float dry  = (c - 1.0f) * vbus[f];   /* retire (1−c) de la voix sèche */
		float side = a * vd;
		o0[f] += dry + side;
		o1[f] += dry - side;
	}
}

/* ============================== Audio loop ========================= */

static void *audio_thread(void *arg)
{
	(void)arg;

	struct sched_param sp = { .sched_priority = RT_PRIO_AUDIO };
	int rt_ok = (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0);
	/* V9.0 — pin sur core 2 (DSP cap readi + mix + ring push, le thread le plus critique) */
	{
		cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(CPU_AUDIO, &cs);
		pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
	}
	mlog("audio thread : SCHED_FIFO prio %d core %d %s", RT_PRIO_AUDIO, CPU_AUDIO,
	     rt_ok ? "OK" : "(failed, fallback SCHED_OTHER)");

	/* Pré-allocation des buffers ALSA */
	int32_t cap_dsp_buf  [PERIOD_FRAMES * N_INPUT_MICS];
	int32_t cap_uac2_buf [PERIOD_FRAMES * N_INPUT_STEMS];
	int32_t cap_phone_buf[PERIOD_FRAMES * N_INPUT_PHONE];
	int32_t play_dsp_buf  [PERIOD_FRAMES * N_OUTPUT_DSP];
	int32_t play_uac2_buf [PERIOD_FRAMES * N_OUTPUT_UAC2];
	int32_t play_phone_buf[PERIOD_FRAMES * N_OUTPUT_PHONE];

	/* V8.1 : UAC2 cap/play sont owned par cap_uac2_thread / play_uac2_thread
	 * (BLOCKING dans ces threads, lus/écrits via rings SPSC). On NE touche
	 * plus aux UAC2 PCMs ici. Phone reste NONBLOCK dans ce thread.
	 */
	if (!g_skip_phone) {
		snd_pcm_nonblock(g_st.cap_phone.pcm,  1);
		snd_pcm_nonblock(g_st.play_phone.pcm, 1);
	}

	/* E6.g Phase 1 : RETRAIT snd_pcm_link.
	 * Le link forçait un snd_pcm_recover simultané sur cap+play à chaque
	 * underrun de l'un, multipliant les blocages 500 ms observés en E6.f.
	 * Sans link, cap et play sont gérés indépendamment côté ALSA — le
	 * recover d'un PCM ne bloque pas l'autre.
	 *
	 * Tradeoff : on n'a plus la garantie sample-précis sur le start. Mais
	 * les 2 PCMs partagent la même horloge hardware SAI7 (i.MX8MP), donc
	 * la sync de phase est garantie par le hardware. Le start non-link
	 * peut décaler l'origine de quelques ms, ce qui est dans le buffer.
	 */

	/* E6.g Phase 2 : DSP play prefill géré par le play_thread. Ici on
	 * prefill juste le ring avec quelques périodes de silence pour que
	 * play_thread démarre immédiatement.
	 * UAC2/Phone restent prefillés ici (NONBLOCK directs).
	 */
	memset(play_dsp_buf, 0, sizeof(play_dsp_buf));
	memset(play_uac2_buf, 0, sizeof(play_uac2_buf));
	memset(play_phone_buf, 0, sizeof(play_phone_buf));

	/* E6.h : Ring prefill = 1 période (2 ms) seulement. Le play_thread
	 * démarre dès la 1ère push depuis l'audio_thread, latence ring minimale.
	 */
	for (int prime = 0; prime < 1; prime++) {
		unsigned wi = atomic_load_explicit(&g_st.ring_write_idx, memory_order_relaxed);
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			unsigned slot = (wi + f) % RING_FRAMES;
			memset(&g_st.ring_buf[slot * N_OUTPUT_DSP], 0,
			       N_OUTPUT_DSP * sizeof(int32_t));
		}
		atomic_store_explicit(&g_st.ring_write_idx, wi + PERIOD_FRAMES,
				      memory_order_release);
	}

	/* V8.1 : UAC2 prefill + start sont faits par leurs threads dédiés
	 * (cap_uac2_thread + play_uac2_thread). On ne start ici que le DSP
	 * (horloge maître) + Phone (still NONBLOCK in this thread). */
	if (!g_skip_phone)
		for (int prime = 0; prime < N_PERIODS - 1; prime++)
			snd_pcm_writei(g_st.play_phone.pcm, play_phone_buf, PERIOD_FRAMES);

	snd_pcm_start(g_st.cap_dsp.pcm);
	if (!g_skip_phone) snd_pcm_start(g_st.cap_phone.pcm);

	struct timespec t_iter_start, t_cap_done, t_mix_done, t_play_done;
	/* V8.33 — Anti-burst : self-paced à 500 Hz via clock_nanosleep absolu.
	 * Si le DSP cap a un backlog (preempt momentané), on ne le rattrape pas
	 * en burst → pas de cap_empty massif. snd_pcm_readi reste blocking : si
	 * DSP en retard, il bloquera ; si DSP en avance, le nanosleep cap. */
	struct timespec t_next;
	clock_gettime(CLOCK_MONOTONIC, &t_next);
	/* PERIOD_FRAMES = 96 @ 48 kHz = 2 ms = 2_000_000 ns */
	const long PERIOD_NS = 2000000L;

	while (atomic_load(&g_st.running)) {
		snd_pcm_sframes_t r;

		/* V9.1 — capture target wake-up BEFORE clock_nanosleep + advance */
		struct timespec t_wakeup_target = t_next;
		/* Wait jusqu'à l'heure cible (= précédent iter + 2 ms) */
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t_next, NULL);
		t_next.tv_nsec += PERIOD_NS;
		while (t_next.tv_nsec >= 1000000000L) {
			t_next.tv_nsec -= 1000000000L;
			t_next.tv_sec  += 1;
		}

		clock_gettime(CLOCK_MONOTONIC, &t_iter_start);

		/* V9.1 — wake-up jitter : combien µs après t_wakeup_target on a repris la main */
		long wakeup_jitter_us =
		    (t_iter_start.tv_sec  - t_wakeup_target.tv_sec)  * 1000000L +
		    (t_iter_start.tv_nsec - t_wakeup_target.tv_nsec) / 1000L;
		if (wakeup_jitter_us > 0) {
			atomic_fetch_add(&g_wake_jitter_sum_us, wakeup_jitter_us);
			atomic_fetch_add(&g_wake_jitter_count, 1);
			long cur_max = atomic_load_explicit(&g_wake_jitter_max_us, memory_order_relaxed);
			if (wakeup_jitter_us > cur_max)
				atomic_store_explicit(&g_wake_jitter_max_us, wakeup_jitter_us, memory_order_relaxed);
		}

		/* 1. DSP cap = horloge maître (blocking read) */
		r = snd_pcm_readi(g_st.cap_dsp.pcm, cap_dsp_buf, PERIOD_FRAMES);
		if (r < 0) { pcm_recover(g_st.cap_dsp.pcm, r); memset(cap_dsp_buf, 0, sizeof(cap_dsp_buf)); }

		/* V8.1 : UAC2 cap = pop du ring SPSC alimenté par cap_uac2_thread.
		 * Si ring vide (thread pas encore prêt, ou USB bloqué), silence
		 * automatique. Pas de risque de propagation USB → DSP. */
		if (g_skip_uac2) {
			memset(cap_uac2_buf, 0, sizeof(cap_uac2_buf));
		} else {
			uac2_ring_pop_period(&g_ring_uac2_cap, cap_uac2_buf);
			/* V8.15 — dump raw pop, ce que le matrix mix verra */
			if (g_usb_cap_dump)
				fwrite(cap_uac2_buf, sizeof(int32_t),
				       PERIOD_FRAMES * UAC2_CH, g_usb_cap_dump);
		}
		if (g_skip_phone) {
			memset(cap_phone_buf, 0, sizeof(cap_phone_buf));
		} else {
			r = snd_pcm_readi(g_st.cap_phone.pcm, cap_phone_buf, PERIOD_FRAMES);
			if (r != PERIOD_FRAMES) {
				memset(cap_phone_buf, 0, sizeof(cap_phone_buf));
				if (r < 0 && r != -EAGAIN) snd_pcm_recover(g_st.cap_phone.pcm, r, 1);
			}
		}

		clock_gettime(CLOCK_MONOTONIC, &t_cap_done);

		/* 2. Mixer loop frame-par-frame */
		/* V9.3.1 : tenir target_lock pendant tout le mix_block + analyzer
		 * + peaks + convert. Fix race use-after-free entre set_fx_engine
		 * (fx_free du state worker LV2) et audio_thread (process_block sur
		 * le même state). audio_thread RT prio 99 préempte control_thread
		 * → blocage de set_fx_engine de quelques µs au pire pendant 1 cycle. */
		pthread_mutex_lock(&g_st.target_lock);
		smooth_gains();

		/* V9.3 : block-based processing.
		 * V9.3.1 : buffers float static (BSS, pas stack) — RT-safe, pas
		 * de risque overflow stack. Audio_thread = thread unique → safe.
		 * Taille totale BSS : (N_INPUT_REAL + N_OUTPUT_TOTAL + N_BUS_FX_CH
		 * + N_RETURN_CH) × PERIOD_FRAMES × 4 = (18+18+8+8) × 96 × 4 = 20 KB. */
		static float in_block[N_INPUT_REAL][PERIOD_FRAMES];
		static float out_block[N_OUTPUT_TOTAL][PERIOD_FRAMES];
		static float bus_pre_block[N_BUS_FX_CH][PERIOD_FRAMES];
		static float ret_post_block[N_RETURN_CH][PERIOD_FRAMES];

		uint32_t pk_in[N_INPUT_TOTAL] = {0};
		uint32_t pk_out[N_OUTPUT_TOTAL] = {0};
		uint32_t pk_fx[N_BUS_FX_CH] = {0};

		/* Convert S32 → float, déinterleave par channel.
		 * V9.5.21 : remap des 8 mics DSP (g_mic_map) — corrige un câblage/
		 * ordre de slots TDM ≠ M1..M8 attendu. in_block[i] = slot g_mic_map[i].
		 * Affecte métre ET audio (cohérent). Défaut identité = sans effet. */
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			for (int i = 0; i < N_INPUT_MICS; i++)
				in_block[i][f] = s32_to_f(
					cap_dsp_buf[f * N_INPUT_MICS
					            + atomic_load_explicit(&g_mic_map[i],
					                                   memory_order_relaxed)]);
			for (int i = 0; i < N_INPUT_STEMS; i++)
				in_block[N_INPUT_MICS + i][f] = s32_to_f(cap_uac2_buf[f * N_INPUT_STEMS + i]);
			for (int i = 0; i < N_INPUT_PHONE; i++)
				in_block[N_INPUT_MICS + N_INPUT_STEMS + i][f] =
					s32_to_f(cap_phone_buf[f * N_INPUT_PHONE + i]);
		}

		/* V12-EXP : gate/expandeur par tranche, in-place AVANT tout
		 * consommateur (sends/master/looper/automix/tap) */
		exp_render(in_block);
		/* V13.6 : EQ de placement par rôle (autolive), entre gate et comp */
		eqx_render(in_block);
		/* V13-COMP : compresseur par tranche, APRÈS le gate */
		cmp_render(in_block);
		/* V13-VFOCUS : la musique s'écarte des bandes de la voix */
		duck_render(in_block);

		/* V12-SMP/LOOP/MIDIX : sources internes → P1/P2 (addition) */
		smp_render(in_block);
		loop_render(in_block);
		midix_render(in_block);

		/* MIX BLOCK — 1 appel pour 96 frames (vs 96 calls × 1 frame) */
		automix_update(in_block, PERIOD_FRAMES);   /* V12-AMX */
		mix_block(in_block, out_block, bus_pre_block, ret_post_block, PERIOD_FRAMES);

		/* V9.5.12 — Export SHM tap USB IN [8,9] pour daemon mixer-ml-inference
		 * (process séparé). Le daemon lit ce tap pour faire l'inférence NPU
		 * sans toucher au process audio RT. Toujours actif (overhead ~768 B
		 * memcpy par période = trivial). */
		extern void mixer_pro_shm_tap_write(const float *L, const float *R, int n);
		mixer_pro_shm_tap_write(in_block[N_INPUT_MICS],
		                        in_block[N_INPUT_MICS + 1],
		                        PERIOD_FRAMES);

		/* V13.9 — spatializer voix : widener décorrélé LEAD+CHŒURS,
		 * injecté dans out0/out1 avant l'EQ/limiter master (tap NPU
		 * ci-dessus non affecté, il lit in_block mic). */
		vspat_render(in_block, out_block, PERIOD_FRAMES);

		/* V13.7 — EQ master + makeup LUFS AVANT l'insert : le boost passe
		 * par le limiter_native (slot 2) → crêtes tenues, pas d'écrêtage. */
		if (atomic_load_explicit(&g_master_on, memory_order_relaxed)) {
			int act = atomic_load_explicit(&g_meq_active,
			                               memory_order_acquire);
			int pend = atomic_load_explicit(&g_meq_pending,
			                                memory_order_acquire);
			if (!g_meq_fading && pend >= 0 && pend != act) {
				g_meq_fading = 1; g_meq_xf = 0;  /* démarre le fondu */
				/* démarrage à chaud : le nouveau filtre part de l'état
				 * courant de l'ancien → pas de ring (pop) à la Fc */
				memcpy(g_meq_st[pend], g_meq_st[act],
				       sizeof(g_meq_st[pend]));
			}
			if (g_meq_fading) {
				/* fondu ancien(act) → nouveau(pend) sur MEQ_XF_LEN */
				for (int ch = 0; ch < 2; ch++) {
					float *x = out_block[ch];
					int xf = g_meq_xf;
					for (int f = 0; f < PERIOD_FRAMES; f++) {
						float yo = meq_chain(act,  ch, x[f]);
						float yn = meq_chain(pend, ch, x[f]);
						float w = (float)(xf + f) / (float)MEQ_XF_LEN;
						if (w > 1.0f) w = 1.0f;
						x[f] = yo * (1.0f - w) + yn * w;
					}
				}
				g_meq_xf += PERIOD_FRAMES;
				if (g_meq_xf >= MEQ_XF_LEN) {   /* fondu terminé */
					atomic_store_explicit(&g_meq_active, pend,
					                      memory_order_release);
					atomic_store_explicit(&g_meq_pending, -1,
					                      memory_order_relaxed);
					g_meq_fading = 0;
				}
			} else {
				for (int ch = 0; ch < 2; ch++) {
					float *x = out_block[ch];
					for (int f = 0; f < PERIOD_FRAMES; f++)
						x[f] = meq_chain(act, ch, x[f]);
				}
			}
			float mtgt = atomic_load_explicit(&g_mk.makeup_mq,
			                memory_order_relaxed) * 0.001f;
			float mc = g_mk.makeup_cur;
			mc += (mtgt - mc) * 0.0625f;   /* converge ~32 ms (anti-zipper) */
			if (fabsf(mc - mtgt) < 1e-4f) mc = mtgt;
			g_mk.makeup_cur = mc;
			if (mc != 1.0f)
				for (int ch = 0; ch < 2; ch++)
					for (int f = 0; f < PERIOD_FRAMES; f++)
						out_block[ch][f] *= mc;
		}

		/* V9.4 — Insert mastering post-master sur out_0+out_1 DSP.
		 * In-place : out_block[0/1] modifié si insert actif. Autres out
		 * (UAC2 stems, phone) restent dry.
		 * V13-SCENES : bypass runtime (bouton MASTERING) — la chaîne
		 * reste chaude, bascule = 1 load atomique. */
		if (atomic_load_explicit(&g_insert_active, memory_order_acquire) &&
		    !atomic_load_explicit(&g_insert_bypass, memory_order_relaxed)) {
			g_insert_chain.process_block(&g_insert_chain,
				out_block[0], out_block[1],
				out_block[0], out_block[1],
				PERIOD_FRAMES);
		}

		/* V9.5.21 — gain de sortie par strip OUT (trim final, lissé anti-
		 * zipper : converge vers la cible en ~32 ms au lieu de sauter) */
		for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
			float tgt = atomic_load_explicit(&g_out_gain_m[o],
			                                 memory_order_relaxed) * 0.001f;
			float cur = g_out_gain_cur[o];
			cur += (tgt - cur) * 0.0625f;
			if (fabsf(cur - tgt) < 1e-4f) cur = tgt;
			g_out_gain_cur[o] = cur;
			if (cur != 1.0f)
				for (int f = 0; f < PERIOD_FRAMES; f++)
					out_block[o][f] *= cur;
		}

		/* V13.7 — mètre short-term LUFS K-pondéré (BS.1770) sur la sortie
		 * réelle out 0/1, publié pour l'asservissement makeup (bmx_tick). */
		if (atomic_load_explicit(&g_master_on, memory_order_relaxed)) {
			float ms = g_mk.ms;
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float acc = 0.0f;
				for (int ch = 0; ch < 2; ch++) {
					float in = out_block[ch][f];
					float y1 = K1_B0 * in + g_mk.k1[ch][0];
					g_mk.k1[ch][0] = K1_B1 * in - K1_A1 * y1 + g_mk.k1[ch][1];
					g_mk.k1[ch][1] = K1_B2 * in - K1_A2 * y1;
					float y2 = K2_B0 * y1 + g_mk.k2[ch][0];
					g_mk.k2[ch][0] = K2_B1 * y1 - K2_A1 * y2 + g_mk.k2[ch][1];
					g_mk.k2[ch][1] = K2_B2 * y1 - K2_A2 * y2;
					acc += y2 * y2;
				}
				ms += LUFS_ST_A * (acc - ms);
			}
			g_mk.ms = ms;
			float lufs = -0.691f + 10.0f * log10f(ms + 1e-12f);
			atomic_store_explicit(&g_mk.lufs_c, (int)(lufs * 100.0f),
			                      memory_order_relaxed);
		}

		/* Analyzer taps : push N samples par tap (lecture buffers block) */
		for (int t = 0; t < N_TAPS; t++) {
			int kind = atomic_load_explicit(
				&g_taps[t].kind, memory_order_relaxed);
			if (kind == TAP_KIND_NONE)
				continue;
			int a = atomic_load_explicit(
				&g_taps[t].a, memory_order_relaxed);
			int b = atomic_load_explicit(
				&g_taps[t].b, memory_order_relaxed);
			const float *bufL = NULL, *bufR = NULL;
			switch (kind) {
			case TAP_KIND_INPUT:
				if (a >= 0 && a < N_INPUT_REAL)        bufL = in_block[a];
				else if (a >= N_INPUT_REAL && a < N_INPUT_TOTAL)
					bufL = ret_post_block[a - N_INPUT_REAL];
				if (b >= 0) {
					if (b < N_INPUT_REAL)               bufR = in_block[b];
					else if (b < N_INPUT_TOTAL)
						bufR = ret_post_block[b - N_INPUT_REAL];
				} else bufR = bufL;
				break;
			case TAP_KIND_BUS_PRE:
				if (a >= 0 && a < N_BUS_FX_CH)         bufL = bus_pre_block[a];
				if (b >= 0 && b < N_BUS_FX_CH)         bufR = bus_pre_block[b];
				else                                    bufR = bufL;
				break;
			case TAP_KIND_OUTPUT:
				if (a >= 0 && a < N_OUTPUT_TOTAL)      bufL = out_block[a];
				if (b >= 0 && b < N_OUTPUT_TOTAL)      bufR = out_block[b];
				else                                    bufR = bufL;
				break;
			}
			if (bufL && bufR) {
				for (int f = 0; f < PERIOD_FRAMES; f++)
					analyzer_tap_write(&g_taps[t], bufL[f], bufR[f]);
			}
		}

		/* Convert float → S32 vers play buffers */
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			for (int o = 0; o < N_OUTPUT_DSP; o++)
				play_dsp_buf[f * N_OUTPUT_DSP + o] = f_to_s32(out_block[o][f]);
			for (int o = 0; o < N_OUTPUT_UAC2; o++)
				play_uac2_buf[f * N_OUTPUT_UAC2 + o] =
					f_to_s32(out_block[N_OUTPUT_DSP + o][f]);
			for (int o = 0; o < N_OUTPUT_PHONE; o++)
				play_phone_buf[f * N_OUTPUT_PHONE + o] =
					f_to_s32(out_block[N_OUTPUT_DSP + N_OUTPUT_UAC2 + o][f]);
		}

		/* Peaks : max(abs) sur N samples par channel.
		 * V13-BANDMIX : accumulation x² au passage → puissance moyenne
		 * (mean square) publiée en atomic float par tranche 0..15,
		 * lue par le plan de contrôle (soundcheck + keeper live). */
		for (int i = 0; i < N_INPUT_REAL; i++) {
			float m = 0.0f, acc = 0.0f;
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float s = in_block[i][f];
				float v = s < 0 ? -s : s;
				if (v > m) m = v;
				acc += s * s;
			}
			pk_in[i] = (uint32_t)(m * 2147483647.0f);
			if (i < N_EXP_CH) {
				/* V13 : puissance brute (crêtes soundcheck) +
				 * EWMA τ≈3 s calculée À CHAQUE BLOC (500 Hz) —
				 * le contrôle qui lisait 1 bloc/s aliasait les
				 * sources modulées (faux keeper). */
				float msv = acc / (float)PERIOD_FRAMES;
				union { float f; uint32_t u; } ms = { .f = msv };
				atomic_store_explicit(&g_ms_in[i], ms.u,
						      memory_order_relaxed);
				g_ms_sm[i] += 0.000666f * (msv - g_ms_sm[i]);
				union { float f; uint32_t u; } sa =
					{ .f = g_ms_sm[i] };
				atomic_store_explicit(&g_ms_avg[i], sa.u,
						      memory_order_relaxed);
			}
		}
		for (int i = 0; i < N_RETURN_CH; i++) {
			float m = 0.0f;
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float v = ret_post_block[i][f] < 0 ? -ret_post_block[i][f] : ret_post_block[i][f];
				if (v > m) m = v;
			}
			pk_in[N_INPUT_REAL + i] = (uint32_t)(m * 2147483647.0f);
		}
		for (int b = 0; b < N_BUS_FX_CH; b++) {
			float m = 0.0f;
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float v = bus_pre_block[b][f] < 0 ? -bus_pre_block[b][f] : bus_pre_block[b][f];
				if (v > m) m = v;
			}
			pk_fx[b] = (uint32_t)(m * 2147483647.0f);
		}
		for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
			float m = 0.0f;
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float v = out_block[o][f] < 0 ? -out_block[o][f] : out_block[o][f];
				if (v > m) m = v;
			}
			pk_out[o] = (uint32_t)(m * 2147483647.0f);
		}

		/* V9.3.1 : unlock fin section critique fx_engines */
		pthread_mutex_unlock(&g_st.target_lock);

		/* E7.1 decay backend × 240/256 (≈ 0.9375) appliqué par bloc 2 ms.
		 * Fall ≈ 12 dB/s, suffisant pour un VU visuel à 30 Hz refresh. */
		for (int i = 0; i < N_INPUT_TOTAL; i++) {
			uint32_t prev = atomic_load_explicit(&g_st.peak_in[i], memory_order_relaxed);
			uint32_t decay = (uint32_t)((uint64_t)prev * 240u / 256u);
			uint32_t v = (pk_in[i] > decay) ? pk_in[i] : decay;
			atomic_store_explicit(&g_st.peak_in[i], v, memory_order_relaxed);
		}
		for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
			uint32_t prev = atomic_load_explicit(&g_st.peak_out[o], memory_order_relaxed);
			uint32_t decay = (uint32_t)((uint64_t)prev * 240u / 256u);
			uint32_t v = (pk_out[o] > decay) ? pk_out[o] : decay;
			atomic_store_explicit(&g_st.peak_out[o], v, memory_order_relaxed);
		}
		for (int b = 0; b < N_BUS_FX_CH; b++) {
			uint32_t prev = atomic_load_explicit(&g_st.peak_fx[b], memory_order_relaxed);
			uint32_t decay = (uint32_t)((uint64_t)prev * 240u / 256u);
			uint32_t v = (pk_fx[b] > decay) ? pk_fx[b] : decay;
			atomic_store_explicit(&g_st.peak_fx[b], v, memory_order_relaxed);
		}

		clock_gettime(CLOCK_MONOTONIC, &t_mix_done);

		atomic_fetch_add(&g_st.frames_processed, PERIOD_FRAMES);

		/* V8.17 — dump play_dsp_buf après matrix mix, avant push au ring play */
		if (g_dsp_play_dump)
			fwrite(play_dsp_buf, sizeof(int32_t),
			       PERIOD_FRAMES * N_OUTPUT_DSP, g_dsp_play_dump);

		/* 3. E6.g Phase 2 : DSP play traité par thread séparé via ring SPSC.
		 *    Le thread audio ne fait QUE push dans le ring (rapide, atomic).
		 *    Si ring full → on écrase le plus vieux (drop policy) pour ne
		 *    jamais bloquer la cap.
		 */
		unsigned wi = atomic_load_explicit(&g_st.ring_write_idx, memory_order_relaxed);
		unsigned ri = atomic_load_explicit(&g_st.ring_read_idx,  memory_order_acquire);
		unsigned avail = wi - ri;   /* unsigned arithmetic wraps OK */
		/* V8.16 — fix race SPSC : si ring full, on ne touche PAS read_idx
		 * (ce qui causait data corruption avec le play_thread consumer en
		 * cours de lecture). On drop simplement cette période entière. */
		if (avail + PERIOD_FRAMES > RING_FRAMES) {
			atomic_fetch_add(&g_st.ring_drops, PERIOD_FRAMES);
			/* skip ce push : data perdue, mais consumer pas corrompu */
		} else {
			/* Copy 96 frames × 8 ch dans le ring (avec wrap modulo RING_FRAMES) */
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				unsigned slot = (wi + f) % RING_FRAMES;
				memcpy(&g_st.ring_buf[slot * N_OUTPUT_DSP],
				       &play_dsp_buf[f * N_OUTPUT_DSP],
				       N_OUTPUT_DSP * sizeof(int32_t));
			}
			atomic_store_explicit(&g_st.ring_write_idx, wi + PERIOD_FRAMES,
					      memory_order_release);

			/* E6.h : signal play_thread (eventfd compteur). On NE signale
			 * QUE quand on a effectivement publié une nouvelle période,
			 * sinon play_thread se déclenche pour rien et lit le slot
			 * actuel à nouveau. */
			uint64_t one = 1;
			(void)write(g_st.ring_event_fd, &one, sizeof(one));
		}

		/* V8.1 : UAC2 play = push dans le ring SPSC consommé par
		 * play_uac2_thread. Si ring full (thread USB trop lent / suspended),
		 * drop oldest sample, pas de blocage du thread audio.
		 * V8.3d : drop event tracké par compteur atomique du ring, le
		 * shift_controller_thread le lit périodiquement et ajuste shift_ppm. */
		if (!g_skip_uac2) {
			/* V8.29 — Push atomique 96 frames. Si ring play plein,
			 * la frame est DROP entière (drops_evt++) plutôt que tronquée. */
			(void)uac2_ring_try_push_period(&g_ring_uac2_play, play_uac2_buf);
		}
		if (!g_skip_phone) {
			r = snd_pcm_writei(g_st.play_phone.pcm, play_phone_buf, PERIOD_FRAMES);
			if (r < 0 && r != -EAGAIN) snd_pcm_recover(g_st.play_phone.pcm, r, 1);
		}

		clock_gettime(CLOCK_MONOTONIC, &t_play_done);

		/* E6.f profiling : update atomic stats. Faible overhead (~50 ns × 3). */
		long us_cap  = (t_cap_done.tv_sec  - t_iter_start.tv_sec)  * 1000000L
		             + (t_cap_done.tv_nsec - t_iter_start.tv_nsec) / 1000L;
		long us_mix  = (t_mix_done.tv_sec  - t_cap_done.tv_sec)    * 1000000L
		             + (t_mix_done.tv_nsec - t_cap_done.tv_nsec)   / 1000L;
		long us_play = (t_play_done.tv_sec - t_mix_done.tv_sec)    * 1000000L
		             + (t_play_done.tv_nsec - t_mix_done.tv_nsec)  / 1000L;
		long us_iter = (t_play_done.tv_sec - t_iter_start.tv_sec)  * 1000000L
		             + (t_play_done.tv_nsec - t_iter_start.tv_nsec)/ 1000L;
		atomic_store(&g_st.last_cap_read_us,   us_cap);
		atomic_store(&g_st.last_mix_us,        us_mix);
		atomic_store(&g_st.last_play_write_us, us_play);
		atomic_store(&g_st.last_iter_us,       us_iter);

		/* V9.1 — Histogram prof_iter_us + outlier log */
		if      (us_iter < 1800)  atomic_fetch_add(&g_iter_lt18,  1);
		else if (us_iter < 2200)  atomic_fetch_add(&g_iter_18_22, 1);
		else if (us_iter < 3000)  atomic_fetch_add(&g_iter_22_30, 1);
		else if (us_iter < 5000)  atomic_fetch_add(&g_iter_30_50, 1);
		else                       atomic_fetch_add(&g_iter_ge50,  1);

		if (us_iter > 3000) {
			mlog("ITER PIC %ldus wake=%ldus cap=%ldus mix=%ldus push=%ldus",
			     us_iter, wakeup_jitter_us, us_cap, us_mix, us_play);
		}
	}

	mlog("audio thread exiting");
	return NULL;
}

/* ============================== Play thread DSP ==================== */

/* Thread dédié au write DSP play. Découple le recover SOF (~60 ms) du flux
 * cap+mix. Lit le ring SPSC alimenté par le thread audio.
 * E6.g Phase 2.
 */
static void *play_thread(void *arg)
{
	(void)arg;
	struct sched_param sp = { .sched_priority = RT_PRIO_PLAY };
	if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
		mlog("WARN: play_thread SCHED_FIFO failed: %s", strerror(errno));
	/* V9.0 — pin sur core 2 (même core que audio_thread, partage L2 cache + ring SPSC) */
	{
		cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(CPU_PLAY, &cs);
		pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
	}
	mlog("play_thread : SCHED_FIFO prio %d core %d", RT_PRIO_PLAY, CPU_PLAY);

	int32_t period_buf[PERIOD_FRAMES * N_OUTPUT_DSP];

	while (atomic_load(&g_st.running)) {
		unsigned wi = atomic_load_explicit(&g_st.ring_write_idx, memory_order_acquire);
		unsigned ri = atomic_load_explicit(&g_st.ring_read_idx,  memory_order_relaxed);
		unsigned avail = wi - ri;

		if (avail < PERIOD_FRAMES) {
			/* E6.h : bloque sur eventfd jusqu'à signal du push.
			 * eventfd_t = uint64, semaphore-style accumule les signaux.
			 * On consomme tout d'un coup, peu importe la valeur.
			 */
			uint64_t consumed;
			(void)read(g_st.ring_event_fd, &consumed, sizeof(consumed));
			continue;
		}

		/* Pop 96 frames du ring */
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			unsigned slot = (ri + f) % RING_FRAMES;
			memcpy(&period_buf[f * N_OUTPUT_DSP],
			       &g_st.ring_buf[slot * N_OUTPUT_DSP],
			       N_OUTPUT_DSP * sizeof(int32_t));
		}
		atomic_store_explicit(&g_st.ring_read_idx, ri + PERIOD_FRAMES,
				      memory_order_release);

		/* Write DSP play (peut bloquer 60 ms sur recover, mais le thread
		 * audio continue de drain le cap en parallèle).
		 */
		snd_pcm_sframes_t r = snd_pcm_writei(g_st.play_dsp.pcm,
						     period_buf, PERIOD_FRAMES);
		if (r < 0) pcm_recover(g_st.play_dsp.pcm, r);
	}
	return NULL;
}

/* ============================== Control socket ===================== */

/* Cherche une clé numérique dans une string JSON simple. -1 si absent.
 * Très minimaliste — pas un parser JSON complet, juste `"key":<number>`.
 */
/* Extrait une string entre guillemets pour une clé "key":"..." */
static int json_get_str(const char *s, const char *key, char *out, int max)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(s, pattern);
	if (!p) return -1;
	p += strlen(pattern);
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	if (*p != '"') return -1;
	p++;
	int i = 0;
	while (*p && *p != '"' && i < max - 1) out[i++] = *p++;
	out[i] = 0;
	return (*p == '"') ? 0 : -1;
}

static int json_get_int(const char *s, const char *key, int *out)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(s, pattern);
	if (!p) return -1;
	p += strlen(pattern);
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	*out = (int)strtol(p, NULL, 10);
	return 0;
}

static int json_get_float(const char *s, const char *key, float *out)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(s, pattern);
	if (!p) return -1;
	p += strlen(pattern);
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	*out = strtof(p, NULL);
	return 0;
}

static int json_has_op(const char *s, const char *op)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"op\"");
	const char *p = strstr(s, pattern);
	if (!p) return 0;
	p += strlen(pattern);
	while (*p == ' ' || *p == ':' || *p == '\t' || *p == '"') p++;
	size_t n = strlen(op);
	/* V9.4.1 : match exact — sinon "set_insert" matche "set_insert_param".
	 * Le char après op doit terminer la string JSON ("). */
	return strncmp(p, op, n) == 0 && p[n] == '"';
}

static void handle_cmd(int fd, const char *line)
{
	/* V9.3.3 : 16 KB pour get_fx avec params + ranges (NPU). */
	static char reply[49152];

	if (json_has_op(line, "set_send")) {
		int in, bus;
		float gain = 0;
		if (json_get_int(line, "in", &in) < 0 ||
		    json_get_int(line, "bus", &bus) < 0 ||
		    json_get_float(line, "gain", &gain) < 0 ||
		    in < 0 || in >= N_INPUT_TOTAL ||
		    bus < 0 || bus >= N_BUS_FX_CH) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_send args\"}\n");
			return;
		}
		pthread_mutex_lock(&g_st.target_lock);
		g_st.send_target[in][bus] = gain;
		pthread_mutex_unlock(&g_st.target_lock);
		atomic_store(&g_presets_dirty, 1);   /* V13.1 : persistance sends */
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"op\":\"set_send\",\"in\":%d,\"bus\":%d,\"gain\":%.4f}\n",
			 in, bus, gain);
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "set_master")) {
		int src, out;
		float gain = 0;
		if (json_get_int(line, "src", &src) < 0 ||
		    json_get_int(line, "out", &out) < 0 ||
		    json_get_float(line, "gain", &gain) < 0 ||
		    src < 0 || src >= N_INPUT_TOTAL ||
		    out < 0 || out >= N_OUTPUT_TOTAL) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_master args\"}\n");
			return;
		}
		pthread_mutex_lock(&g_st.target_lock);
		g_st.master_target[src][out] = gain;
		pthread_mutex_unlock(&g_st.target_lock);
		atomic_store(&g_presets_dirty, 1);   /* V9.5.21b : persistance */
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"op\":\"set_master\",\"src\":%d,\"out\":%d,\"gain\":%.4f}\n",
			 src, out, gain);
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "set_fx_bus")) {
		int bus;
		float gain = 0;
		if (json_get_int(line, "bus", &bus) < 0 ||
		    json_get_float(line, "gain", &gain) < 0 ||
		    bus < 0 || bus >= N_BUS_FX_CH) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_fx_bus args\"}\n");
			return;
		}
		pthread_mutex_lock(&g_st.target_lock);
		g_st.fx_bus_target[bus] = gain;
		pthread_mutex_unlock(&g_st.target_lock);
		atomic_store(&g_presets_dirty, 1);   /* V9.5.21b : persistance */
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"op\":\"set_fx_bus\",\"bus\":%d,\"gain\":%.4f}\n",
			 bus, gain);
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "set_input_gain")) {
		int src;
		float gain = 1.0f;
		if (json_get_int(line, "src", &src) < 0 ||
		    json_get_float(line, "gain", &gain) < 0 ||
		    src < 0 || src >= N_INPUT_TOTAL) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_input_gain args\"}\n");
			return;
		}
		{
			const int lp = link_partner(src);   /* V13.3 */
			pthread_mutex_lock(&g_st.target_lock);
			g_st.input_target[src] = gain;
			if (lp >= 0)
				g_st.input_target[lp] = gain;
			pthread_mutex_unlock(&g_st.target_lock);
		}
		atomic_store(&g_presets_dirty, 1);   /* V9.5.21b : persistance */
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"op\":\"set_input_gain\",\"src\":%d,\"gain\":%.4f}\n",
			 src, gain);
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "set_mute")) {
		int src, mute;
		if (json_get_int(line, "src", &src) < 0 ||
		    json_get_int(line, "mute", &mute) < 0 ||
		    src < 0 || src >= N_INPUT_TOTAL) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_mute args\"}\n");
			return;
		}
		{
			const int lp = link_partner(src);   /* V13.3 */
			pthread_mutex_lock(&g_st.target_lock);
			if (mute) {
				g_st.mute_mask |= (1u << src);
				if (lp >= 0) g_st.mute_mask |= (1u << lp);
			} else {
				g_st.mute_mask &= ~(1u << src);
				if (lp >= 0) g_st.mute_mask &= ~(1u << lp);
			}
			pthread_mutex_unlock(&g_st.target_lock);
		}
		atomic_store(&g_presets_dirty, 1);   /* V9.5.21b : persistance */
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"op\":\"set_mute\",\"src\":%d,\"mute\":%d}\n",
			 src, mute);
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "set_link")) {
		/* V13.3 : {"op":"set_link","pair":0-7,"on":0|1} — lie les
		 * tranches (2k,2k+1). Ne modifie rien d'autre : le premier
		 * geste (fader/mute/...) resynchronise la paire. */
		int pair = -1, on = 0;
		if (json_get_int(line, "pair", &pair) < 0 ||
		    json_get_int(line, "on", &on) < 0 ||
		    pair < 0 || pair >= N_LINK_PAIRS) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_link args\"}\n");
			return;
		}
		atomic_store_explicit(&g_link[pair], on ? 1 : 0,
				      memory_order_relaxed);
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_link\",\"pair\":%d,"
			    "\"on\":%d}\n", pair, on ? 1 : 0);

	} else if (json_has_op(line, "get_links")) {
		int n = snprintf(reply, sizeof(reply),
				 "{\"ok\":true,\"links\":[");
		for (int i = 0; i < N_LINK_PAIRS; i++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%d",
				      i ? "," : "",
				      atomic_load(&g_link[i]));
		n += snprintf(reply + n, sizeof(reply) - n, "]}\n");
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "get_strip_routing")) {
		/* E7.3a : retourne l'état routing complet pour 1 input strip :
		 *   - sends[8]    : send_target[src][bus] pour bus 0..7
		 *   - master[18]  : master_target[src][out] pour out 0..17
		 *   - gain        : input_target[src] (strip fader)
		 *   - mute        : (mute_mask >> src) & 1
		 */
		int src;
		if (json_get_int(line, "src", &src) < 0 ||
		    src < 0 || src >= N_INPUT_TOTAL) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad get_strip_routing src\"}\n");
			return;
		}
		int n = snprintf(reply, sizeof(reply),
				 "{\"ok\":true,\"src\":%d,\"sends\":[", src);
		pthread_mutex_lock(&g_st.target_lock);
		for (int b = 0; b < N_BUS_FX_CH && n < (int)sizeof(reply); b++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%.4f",
				      b ? "," : "", g_st.send_target[src][b]);
		n += snprintf(reply + n, sizeof(reply) - n, "],\"master\":[");
		for (int o = 0; o < N_OUTPUT_TOTAL && n < (int)sizeof(reply); o++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%.4f",
				      o ? "," : "", g_st.master_target[src][o]);
		n += snprintf(reply + n, sizeof(reply) - n,
			      "],\"gain\":%.4f,\"mute\":%d}\n",
			      g_st.input_target[src],
			      (g_st.mute_mask >> src) & 1);
		pthread_mutex_unlock(&g_st.target_lock);
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "get_state")) {
		/* snd_pcm_delay : nb de frames entre le pointeur applicatif et le hw.
		 * cap delay = samples accumulés non encore lus
		 * play delay = samples écrits non encore joués
		 * latence DSP one-way ≈ play_delay / 48 ms (à 48 kHz).
		 */
		snd_pcm_sframes_t cap_d = 0, play_d = 0;
		snd_pcm_delay(g_st.cap_dsp.pcm,  &cap_d);
		snd_pcm_delay(g_st.play_dsp.pcm, &play_d);
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"version\":\"%s\",\"frames\":%lu,\"xrun\":%lu,"
			 "\"mute_mask\":%u,\"cap_delay_frames\":%ld,\"play_delay_frames\":%ld,"
			 "\"latency_us_one_way\":%ld,"
			 "\"prof_cap_us\":%ld,\"prof_mix_us\":%ld,\"prof_play_us\":%ld,"
			 "\"prof_iter_us\":%ld,\"ring_drops\":%lu,"
			 "\"ring_fill_frames\":%u}\n",
			 MIXER_VERSION,
			 (unsigned long)atomic_load(&g_st.frames_processed),
			 (unsigned long)atomic_load(&g_st.xrun_count),
			 g_st.mute_mask,
			 (long)cap_d, (long)play_d,
			 (long)((cap_d + play_d) * 1000000L / SAMPLE_RATE),
			 (long)atomic_load(&g_st.last_cap_read_us),
			 (long)atomic_load(&g_st.last_mix_us),
			 (long)atomic_load(&g_st.last_play_write_us),
			 (long)atomic_load(&g_st.last_iter_us),
			 (unsigned long)atomic_load(&g_st.ring_drops),
			 (unsigned)(atomic_load(&g_st.ring_write_idx) -
				    atomic_load(&g_st.ring_read_idx)));
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "set_fx_param")) {
		int bus;
		char param[32];
		float value = 0;
		if (json_get_int(line, "bus", &bus) < 0 ||
		    json_get_str(line, "param", param, sizeof(param)) < 0 ||
		    json_get_float(line, "value", &value) < 0 ||
		    bus < 0 || bus >= N_BUS_FX) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_fx_param args\"}\n");
			return;
		}
		pthread_mutex_lock(&g_st.target_lock);
		int rc = g_st.fx_engines[bus].set_param(&g_st.fx_engines[bus], param, value);
		pthread_mutex_unlock(&g_st.target_lock);
		if (rc < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"unknown fx param\"}\n");
		} else {
			atomic_store(&g_presets_dirty, 1);  /* V9.3.5 */
			snprintf(reply, sizeof(reply),
				 "{\"ok\":true,\"op\":\"set_fx_param\",\"bus\":%d,"
				 "\"param\":\"%s\",\"value\":%.4f}\n",
				 bus, param, value);
			write(fd, reply, strlen(reply));
		}

	} else if (json_has_op(line, "get_fx")) {
		int bus;
		if (json_get_int(line, "bus", &bus) < 0 ||
		    bus < 0 || bus >= N_BUS_FX) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad get_fx args\"}\n");
			return;
		}
		/* V9.3.3 : 8 KB pour tenir params + ranges (NPU). LSP MB Comp x8
		 * a ~200 params × ~30 chars = 6 KB + ranges 6 KB → 12 KB sécurité. */
		static char body[49152];
		g_st.fx_engines[bus].get_state(&g_st.fx_engines[bus], body, sizeof(body));
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"bus\":%d,%s}\n", bus, body);
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "set_fx_engine")) {
		/* V9.2 — Change l'engine d'un bus FX. Engines builtin (compressor,
		 * reverb, delay, eq) OU LV2 plugin par URI.
		 * Format : {"op":"set_fx_engine","bus":N,"engine":"lv2","uri":"..."}
		 * Pour engines builtin : "engine":"compressor"|"reverb"|"delay"|"eq"
		 */
		int bus;
		char engine[32];
		char uri[256] = "";
		if (json_get_int(line, "bus", &bus) < 0 ||
		    json_get_str(line, "engine", engine, sizeof(engine)) < 0 ||
		    bus < 0 || bus >= N_BUS_FX) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_fx_engine args\"}\n");
			return;
		}
		int uri_set = (json_get_str(line, "uri", uri, sizeof(uri)) >= 0);

		fx_engine_t new_eng = {0};
		int ok = 0;
		if (!strcmp(engine, "passthrough")) ok = fx_init_passthrough(&new_eng, (float)SAMPLE_RATE);
		else if (!strcmp(engine, "compressor")) ok = fx_init_compressor(&new_eng, (float)SAMPLE_RATE);
		else if (!strcmp(engine, "reverb"))     ok = fx_init_reverb    (&new_eng, (float)SAMPLE_RATE);
		else if (!strcmp(engine, "delay"))      ok = fx_init_delay     (&new_eng, (float)SAMPLE_RATE);
		else if (!strcmp(engine, "eq"))         ok = fx_init_eq        (&new_eng, (float)SAMPLE_RATE);
		else if (!strcmp(engine, "lv2") && uri_set)
			ok = fx_init_lv2(&new_eng, (float)SAMPLE_RATE, uri);

		if (!ok) {
			dprintf(fd, "{\"ok\":false,\"err\":\"engine init failed\","
			        "\"engine\":\"%s\",\"uri\":\"%s\"}\n", engine, uri);
			return;
		}

		/* Swap atomic sous mutex. fx_free de l'ancien APRÈS swap pour que
		 * audio_thread voie toujours un engine valide. */
		pthread_mutex_lock(&g_st.target_lock);
		fx_engine_t old_eng = g_st.fx_engines[bus];
		g_st.fx_engines[bus] = new_eng;
		pthread_mutex_unlock(&g_st.target_lock);
		fx_free(&old_eng);
		atomic_store(&g_presets_dirty, 1);  /* V9.3.5 */

		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"op\":\"set_fx_engine\",\"bus\":%d,"
			 "\"engine\":\"%s\",\"uri\":\"%s\"}\n",
			 bus, engine, uri);
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "set_insert")) {
		/* V9.4 — Configure la chaîne insert post-master.
		 * Format : {"op":"set_insert","plugins":[
		 *   {"engine":"lv2","uri":"http://..."},
		 *   {"engine":"compressor"},
		 *   ...
		 * ]}
		 * plugins:[] = bypass (insert désactivé).
		 *
		 * Parser ad-hoc : itère sur les `{...}` contenus entre `"plugins":[`
		 * et le matching `]`. Pour chaque, extrait engine + uri. Limite
		 * FX_CHAIN_MAX (8) plugins. */
		const char *p = strstr(line, "\"plugins\"");
		if (!p) { dprintf(fd, "{\"ok\":false,\"err\":\"missing plugins\"}\n"); return; }
		p = strchr(p, '['); if (!p) { dprintf(fd, "{\"ok\":false,\"err\":\"bad plugins array\"}\n"); return; }
		p++;
		struct fx_chain_spec specs[FX_CHAIN_MAX];
		char engines[FX_CHAIN_MAX][32], uris[FX_CHAIN_MAX][256];
		int n_specs = 0;
		while (*p && *p != ']' && n_specs < FX_CHAIN_MAX) {
			const char *brace = strchr(p, '{');
			if (!brace) break;
			const char *end = strchr(brace, '}');
			if (!end) break;
			char obj[512];
			size_t len_obj = (size_t)(end - brace + 1);
			if (len_obj >= sizeof(obj)) len_obj = sizeof(obj) - 1;
			memcpy(obj, brace, len_obj); obj[len_obj] = '\0';
			engines[n_specs][0] = '\0';
			uris[n_specs][0] = '\0';
			(void)json_get_str(obj, "engine", engines[n_specs], sizeof(engines[0]));
			(void)json_get_str(obj, "uri",     uris[n_specs],    sizeof(uris[0]));
			specs[n_specs].engine = engines[n_specs];
			specs[n_specs].uri    = uris[n_specs];
			n_specs++;
			p = end + 1;
		}

		if (n_specs == 0) {
			/* Bypass : désactive l'insert + free chain existante */
			pthread_mutex_lock(&g_st.target_lock);
			int was_active = atomic_exchange(&g_insert_active, 0);
			g_insert_spec_n = 0;   /* V9.5.21b : persiste le bypass */
			pthread_mutex_unlock(&g_st.target_lock);
			if (was_active) fx_free(&g_insert_chain);
			dprintf(fd, "{\"ok\":true,\"op\":\"set_insert\",\"n\":0}\n");
			atomic_store(&g_presets_dirty, 1);
			return;
		}

		fx_engine_t new_chain = {0};
		if (!fx_init_chain(&new_chain, (float)SAMPLE_RATE, specs, n_specs)) {
			dprintf(fd, "{\"ok\":false,\"err\":\"chain init failed\"}\n");
			return;
		}

		pthread_mutex_lock(&g_st.target_lock);
		fx_engine_t old_chain = g_insert_chain;
		int was_active = atomic_load(&g_insert_active);
		g_insert_chain = new_chain;
		atomic_store(&g_insert_active, 1);
		/* V9.5.21b : copie de la spec pour persistance */
		g_insert_spec_n = n_specs;
		for (int i = 0; i < n_specs; i++) {
			strncpy(g_insert_spec_engine[i], engines[i], sizeof(g_insert_spec_engine[0]) - 1);
			g_insert_spec_engine[i][sizeof(g_insert_spec_engine[0]) - 1] = '\0';
			strncpy(g_insert_spec_uri[i], uris[i], sizeof(g_insert_spec_uri[0]) - 1);
			g_insert_spec_uri[i][sizeof(g_insert_spec_uri[0]) - 1] = '\0';
		}
		pthread_mutex_unlock(&g_st.target_lock);
		if (was_active) fx_free(&old_chain);
		atomic_store(&g_presets_dirty, 1);

		dprintf(fd, "{\"ok\":true,\"op\":\"set_insert\",\"n\":%d}\n", n_specs);

	} else if (json_has_op(line, "set_insert_param")) {
		/* Format : {"op":"set_insert_param","slot":N,"param":"name","value":X} */
		int slot;
		char param[32]; float value = 0;
		if (json_get_int(line, "slot", &slot) < 0 ||
		    json_get_str(line, "param", param, sizeof(param)) < 0 ||
		    json_get_float(line, "value", &value) < 0 ||
		    slot < 0 || slot >= FX_CHAIN_MAX) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad args\"}\n"); return;
		}
		if (!atomic_load(&g_insert_active)) {
			dprintf(fd, "{\"ok\":false,\"err\":\"insert not active\"}\n"); return;
		}
		/* Construit "<slot>/<param>" pour chain_set_param.
		 * V9.5.12 — PAS de target_lock : chain.set_param est interne
		 * lock-free (LV2 = atomic ctrl_target write, para_eq_x16 = direct
		 * struct write + biquad recalc). Lock contention avec audio_thread
		 * RT99 causait xrun + kernel freeze sous flux dense (50+ Hz). */
		char composite[64];
		snprintf(composite, sizeof(composite), "%d/%s", slot, param);
		int rc = g_insert_chain.set_param(&g_insert_chain, composite, value);
		if (rc < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"unknown param or slot\"}\n");
		} else {
			atomic_store(&g_presets_dirty, 1);
			dprintf(fd, "{\"ok\":true,\"op\":\"set_insert_param\",\"slot\":%d,"
			            "\"param\":\"%s\",\"value\":%.4f}\n",
			        slot, param, value);
		}

	} else if (json_has_op(line, "set_insert_params_bulk")) {
		/* V9.5.5 : set N params en 1 seule call HTTP pour 50 Hz update NPU.
		 *
		 * Format : {"op":"set_insert_params_bulk","params":[
		 *   [slot, "name", value],
		 *   [slot, "name", value],
		 *   ...
		 * ]}
		 *
		 * Parser ad-hoc : itère sur les `[slot,"name",value]` entre `"params":[`
		 * et le matching `]` final. Pour chaque triple, set le param.
		 * Tous les sets sont effectués sous un seul lock pour cohérence atomic. */
		if (!atomic_load(&g_insert_active)) {
			dprintf(fd, "{\"ok\":false,\"err\":\"insert not active\"}\n"); return;
		}
		const char *p = strstr(line, "\"params\"");
		if (!p) { dprintf(fd, "{\"ok\":false,\"err\":\"missing params\"}\n"); return; }
		p = strchr(p, '[');
		if (!p) { dprintf(fd, "{\"ok\":false,\"err\":\"bad params array\"}\n"); return; }
		p++;
		int n_set = 0, n_fail = 0;
		/* V9.5.12 — PAS de target_lock : chain.set_param est lock-free
		 * en interne (cf set_insert_param ci-dessus). Évite contention
		 * avec audio_thread RT99 sous flux dense (10+ Hz × 76 params). */
		while (*p && *p != ']') {
			/* Find next `[slot,"name",value]` */
			while (*p == ' ' || *p == ',') p++;
			if (*p != '[') break;
			p++;   /* skip '[' */
			while (*p == ' ') p++;
			int slot = atoi(p);
			while (*p && *p != ',') p++;
			if (*p == ',') p++;
			while (*p == ' ') p++;
			if (*p != '"') break;
			p++;
			char pname[32];
			int i_name = 0;
			while (*p && *p != '"' && i_name < (int)sizeof(pname) - 1)
				pname[i_name++] = *p++;
			pname[i_name] = 0;
			if (*p == '"') p++;
			while (*p == ' ' || *p == ',') p++;
			float value = (float)atof(p);
			/* Skip value digits */
			while (*p && *p != ']' && *p != ',') p++;
			while (*p && *p != ']') p++;
			if (*p == ']') p++;
			/* Apply */
			char composite[64];
			snprintf(composite, sizeof(composite), "%d/%s", slot, pname);
			int rc = g_insert_chain.set_param(&g_insert_chain, composite, value);
			if (rc < 0) n_fail++;
			else n_set++;
		}
		/* (target_lock retiré V9.5.12 — voir commentaire avant la boucle) */
		if (n_set > 0) atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_insert_params_bulk\","
		            "\"set\":%d,\"fail\":%d}\n", n_set, n_fail);

	} else if (json_has_op(line, "get_insert")) {
		/* Dump JSON full : type + n + chain[] avec slot/state/ranges */
		if (!atomic_load(&g_insert_active)) {
			dprintf(fd, "{\"ok\":true,\"active\":false}\n"); return;
		}
		static char insert_buf[32768];
		pthread_mutex_lock(&g_st.target_lock);
		int n = g_insert_chain.get_state(&g_insert_chain, insert_buf, sizeof(insert_buf));
		pthread_mutex_unlock(&g_st.target_lock);
		dprintf(fd, "{\"ok\":true,\"active\":true,%s}\n", n > 0 ? insert_buf : "");

	} else if (json_has_op(line, "set_assistant_mode")) {
		/* V9.5.12 — Stocke l'état Mixer Assistant. mixer-pro ne fait PAS
		 * d'inférence (process séparé mixer-ml-inference s'en charge,
		 * pour éviter freeze kernel TFLite+galcore+RT99). Le daemon poll
		 * get_assistant pour savoir quoi faire.
		 *
		 * Format : {"op":"set_assistant_mode","mode":"mastering"|"passthrough",
		 *          "source":"hw"|"usb"}    (source optionnel, défaut hw)
		 */
		char mode_str[32] = "", src_str[8] = "";
		(void)json_get_str(line, "mode",   mode_str, sizeof(mode_str));
		(void)json_get_str(line, "source", src_str,  sizeof(src_str));
		int mode = (strcmp(mode_str, "mastering") == 0) ? 1 : 0;
		int src  = (strcmp(src_str,  "usb")       == 0) ? 1 : 0;
		atomic_store_explicit(&g_assistant_mode,   mode, memory_order_release);
		atomic_store_explicit(&g_assistant_source, src,  memory_order_release);
		atomic_store(&g_presets_dirty, 1);   /* V9.5.21b : persistance */
		dprintf(fd, "{\"ok\":true,\"op\":\"set_assistant_mode\","
		            "\"mode\":\"%s\",\"source\":\"%s\"}\n",
		        mode ? "mastering" : "passthrough",
		        src  ? "usb"       : "hw");

	} else if (json_has_op(line, "looper_track_ctl")) {
		/* V12-LOOP-PRO : {"op":"looper_track_ctl","track":N,
		 * "action":"rec|play|mute|unmute|clear"} */
		int t = -1; char act[16] = "";
		(void)json_get_int(line, "track", &t);
		(void)json_get_str(line, "action", act, sizeof(act));
		if (t < 0 || t >= LOOP_TRACKS) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad track\"}\n");
			return;
		}
		struct loop_track *tr = &g_tr[t];
		int st = atomic_load(&tr->state);
		uint32_t mlen = atomic_load(&g_master_len);

		if (!strcmp(act, "rec")) {
			/* V13.2 : re-tap REC sur une piste ARMÉE = désarme */
			if (st == TR_ARMED) {
				atomic_store_explicit(&tr->state, TR_EMPTY,
						      memory_order_release);
				dprintf(fd, "{\"ok\":true,\"track\":%d,"
					    "\"armed\":0}\n", t);
				return;
			}
			/* un seul REC ACTIF simultané (l'armement est libre) */
			int busy = 0;
			for (int i = 0; i < LOOP_TRACKS; i++)
				if (atomic_load(&g_tr[i].state) == TR_REC) busy = 1;
			if (st != TR_EMPTY) {
				dprintf(fd, "{\"ok\":false,\"err\":\"not empty\"}\n");
				return;
			}
			/* memset de la piste VIDE (non lue par l'audio) → silence
			 * des zones non ré-enregistrées, aucun glitch. */
			memset(tr->buf, 0, (size_t)LOOP_MAX_FRAMES * 2 * sizeof(float));
			tr->rec_head = 0;
			tr->rec_done = 0;
			atomic_store(&tr->rec_start, REC_START_NONE);
			atomic_store(&tr->len, 0);
			atomic_store(&tr->muted, 0);
			if (mlen == 0 && !busy) {
				/* pas encore de boucle maître : REC libre
				 * immédiat (définit la longueur au PLAY) */
				atomic_store_explicit(&tr->state, TR_REC,
						      memory_order_release);
			} else {
				/* V13.2 : boucle maître présente (ou en cours
				 * d'enregistrement) → ARMÉ, départ quantifié
				 * au prochain début de boucle, un tour exact
				 * puis PLAY (loop_render). */
				atomic_store(&g_loop_run, 1);
				atomic_store_explicit(&tr->state, TR_ARMED,
						      memory_order_release);
			}
		} else if (!strcmp(act, "play")) {
			if (st == TR_REC) {
				if (mlen == 0) {
					/* piste MAÎTRE : fige master_len = rec_head */
					uint32_t h = tr->rec_head;
					if (h == 0) {
						dprintf(fd, "{\"ok\":false,\"err\":\"empty rec\"}\n");
						return;
					}
					atomic_store_explicit(&tr->len, h, memory_order_release);
					atomic_store(&tr->state, TR_PLAY);
					atomic_store(&g_master_len, h);
					atomic_store(&g_lpos, 0);
					atomic_store(&g_loop_run, 1);
				} else {
					/* piste alignée : fige à mlen (zones non
					 * enregistrées = silence memsetté) */
					atomic_store_explicit(&tr->len, mlen, memory_order_release);
					atomic_store(&tr->state, TR_PLAY);
				}
			}
			/* si déjà PLAY : no-op (transport global via looper_ctl) */
		} else if (!strcmp(act, "mute")) {
			atomic_store(&tr->muted, 1);
		} else if (!strcmp(act, "unmute")) {
			atomic_store(&tr->muted, 0);
		} else if (!strcmp(act, "clear")) {
			atomic_store_explicit(&tr->state, TR_EMPTY, memory_order_release);
			atomic_store(&tr->len, 0);
			atomic_store(&tr->muted, 0);
			atomic_store(&tr->peak, 0);
			tr->rec_head = 0;
			tr->rec_done = 0;
			/* si plus aucune piste n'a de contenu ni n'enregistre →
			 * réinitialise l'horloge maître (nouveau départ). */
			int alive = 0;
			for (int i = 0; i < LOOP_TRACKS; i++) {
				int s = atomic_load(&g_tr[i].state);
				if (s == TR_REC || (s == TR_PLAY && atomic_load(&g_tr[i].len)))
					alive = 1;
			}
			if (!alive) {
				atomic_store(&g_master_len, 0);
				atomic_store(&g_lpos, 0);
				atomic_store(&g_loop_run, 0);
				/* V13.2 : plus de boucle maître → les pistes
				 * ARMÉES n'ont plus de départ possible */
				for (int i = 0; i < LOOP_TRACKS; i++)
					if (atomic_load(&g_tr[i].state) == TR_ARMED)
						atomic_store(&g_tr[i].state, TR_EMPTY);
			}
		} else {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad action\"}\n");
			return;
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"looper_track_ctl\",\"track\":%d,"
			"\"state\":\"%s\"}\n", t, TR_NAMES[atomic_load(&tr->state)]);

	} else if (json_has_op(line, "looper_track_cfg")) {
		/* {"op":"looper_track_cfg","track":N,"src_a":N,"src_b":N|-1,
		 * "gain_db":F} — refusé pendant REC de cette piste */
		int t = -1;
		(void)json_get_int(line, "track", &t);
		if (t < 0 || t >= LOOP_TRACKS) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad track\"}\n");
			return;
		}
		struct loop_track *tr = &g_tr[t];
		if (atomic_load(&tr->state) == TR_REC) {
			dprintf(fd, "{\"ok\":false,\"err\":\"busy rec\"}\n");
			return;
		}
		int a = -2, b = -2;
		float gdb = 1000.0f;
		(void)json_get_int(line, "src_a", &a);
		(void)json_get_int(line, "src_b", &b);
		(void)json_get_float(line, "gain_db", &gdb);
		if (a >= 0 && a < N_INPUT_REAL) tr->src_a = a;
		if (b >= -1 && b < N_INPUT_REAL) tr->src_b = b;
		if (gdb > -60.0f && gdb <= 12.0f) tr->gain = powf(10.0f, gdb / 20.0f);
		dprintf(fd, "{\"ok\":true,\"op\":\"looper_track_cfg\",\"track\":%d}\n", t);

	} else if (json_has_op(line, "looper_ctl")) {
		/* transport global : {"op":"looper_ctl","action":"play_all|stop_all|clear_all"} */
		char act[16] = "";
		(void)json_get_str(line, "action", act, sizeof(act));
		if (!strcmp(act, "play_all")) {
			if (atomic_load(&g_master_len)) atomic_store(&g_loop_run, 1);
		} else if (!strcmp(act, "stop_all")) {
			atomic_store(&g_loop_run, 0);
		} else if (!strcmp(act, "clear_all")) {
			for (int i = 0; i < LOOP_TRACKS; i++) {
				atomic_store_explicit(&g_tr[i].state, TR_EMPTY,
						      memory_order_release);
				atomic_store(&g_tr[i].len, 0);
				atomic_store(&g_tr[i].muted, 0);
				atomic_store(&g_tr[i].peak, 0);
				g_tr[i].rec_head = 0;
				g_tr[i].rec_done = 0;
			}
			atomic_store(&g_master_len, 0);
			atomic_store(&g_lpos, 0);
			atomic_store(&g_loop_run, 0);
		} else {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad action\"}\n");
			return;
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"looper_ctl\",\"action\":\"%s\"}\n", act);

	} else if (json_has_op(line, "looper_status")) {
		uint32_t mlen = atomic_load(&g_master_len);
		int n = snprintf(reply, sizeof(reply),
			"{\"ok\":true,\"master_len_s\":%.2f,\"pos_s\":%.2f,"
			"\"run\":%d,\"max_s\":%u,\"master_peak\":%u,\"tracks\":[",
			mlen / 48000.0f, atomic_load(&g_lpos) / 48000.0f,
			atomic_load(&g_loop_run), LOOP_MAX_FRAMES / 48000u,
			atomic_load(&g_loop_mpeak));
		for (int t = 0; t < LOOP_TRACKS; t++) {
			struct loop_track *tr = &g_tr[t];
			n += snprintf(reply + n, sizeof(reply) - n,
				"%s{\"track\":%d,\"state\":\"%s\",\"len_s\":%.2f,"
				"\"muted\":%d,\"src_a\":%d,\"src_b\":%d,"
				"\"gain_db\":%.1f,\"peak\":%u}",
				t ? "," : "", t, TR_NAMES[atomic_load(&tr->state)],
				atomic_load(&tr->len) / 48000.0f,
				atomic_load(&tr->muted), tr->src_a, tr->src_b,
				20.0f * log10f(tr->gain > 1e-6f ? tr->gain : 1e-6f),
				atomic_load(&tr->peak));
		}
		n += snprintf(reply + n, sizeof(reply) - n, "]}\n");
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "sampler_list")) {
		/* V12-SMP : slots (nom, durée s, playing, position s) */
		int n = snprintf(reply, sizeof(reply), "{\"ok\":true,\"slots\":[");
		for (int i = 0; i < SMP_SLOTS; i++) {
			struct smp_slot *s = &g_smp[i];
			n += snprintf(reply + n, sizeof(reply) - n,
				"%s{\"slot\":%d,\"name\":\"%s\",\"len_s\":%.1f,"
				"\"playing\":%d,\"pos_s\":%.1f}",
				i ? "," : "", i, s->buf ? s->name : "",
				s->frames / 48000.0f,
				atomic_load(&s->playing),
				atomic_load(&s->pos) / 48000.0f);
		}
		n += snprintf(reply + n, sizeof(reply) - n, "]}\n");
		write(fd, reply, n);

	} else if (json_has_op(line, "sampler_trigger")) {
		/* {"op":"sampler_trigger","slot":N,"gain_db":F} — retrigger OK */
		int slot;
		float gdb = 0.0f;
		if (json_get_int(line, "slot", &slot) < 0 ||
		    slot < 0 || slot >= SMP_SLOTS || !g_smp[slot].buf) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad slot\"}\n");
			return;
		}
		(void)json_get_float(line, "gain_db", &gdb);
		g_smp[slot].gain = powf(10.0f, gdb / 20.0f);
		atomic_store(&g_smp[slot].pos, 0);
		atomic_store_explicit(&g_smp[slot].playing, 1,
				      memory_order_release);
		dprintf(fd, "{\"ok\":true,\"op\":\"sampler_trigger\",\"slot\":%d}\n",
			slot);

	} else if (json_has_op(line, "sampler_stop")) {
		/* {"op":"sampler_stop","slot":N|-1} — -1 = tous */
		int slot = -1;
		(void)json_get_int(line, "slot", &slot);
		for (int i = 0; i < SMP_SLOTS; i++)
			if (slot < 0 || slot == i)
				atomic_store(&g_smp[i].playing, 0);
		dprintf(fd, "{\"ok\":true,\"op\":\"sampler_stop\"}\n");

	} else if (json_has_op(line, "sampler_reload")) {
		smp_scan(1);
		int loaded = 0;
		for (int i = 0; i < SMP_SLOTS; i++)
			if (g_smp[i].buf)
				loaded++;
		dprintf(fd, "{\"ok\":true,\"op\":\"sampler_reload\",\"loaded\":%d}\n",
			loaded);

	} else if (json_has_op(line, "set_automix")) {
		/* V12-AMX : adhésion + poids par tranche.
		 * {"op":"set_automix","src":N,"on":0|1,"weight_db":F} */
		int src, on = 0;
		float wdb = 0.0f;
		if (json_get_int(line, "src", &src) < 0 ||
		    src < 0 || src >= N_INPUT_REAL) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_automix src\"}\n");
			return;
		}
		/* updates PARTIELS : toggler « A » sans weight_db ne doit pas
		 * écraser le poids, et régler le poids ne touche pas l'adhésion */
		int has_on = json_get_int(line, "on", &on) == 0;
		int has_w  = json_get_float(line, "weight_db", &wdb) == 0;
		{
			const int lp = link_partner(src);   /* V13.3 */
			pthread_mutex_lock(&g_st.target_lock);
			if (has_on) {
				g_st.automix_member[src] = on ? 1 : 0;
				if (!on)
					g_st.automix_gtarget[src] = 1.0f;
				if (lp >= 0) {
					g_st.automix_member[lp] = on ? 1 : 0;
					if (!on)
						g_st.automix_gtarget[lp] = 1.0f;
				}
			}
			if (has_w && wdb >= -20.0f && wdb <= 20.0f) {
				g_st.automix_weight[src] = powf(10.0f, wdb / 20.0f);
				if (lp >= 0)
					g_st.automix_weight[lp] =
						g_st.automix_weight[src];
			}
			pthread_mutex_unlock(&g_st.target_lock);
		}
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_automix\",\"src\":%d,"
			    "\"on\":%d}\n", src, g_st.automix_member[src]);

	} else if (json_has_op(line, "set_automix_cfg")) {
		/* {"op":"set_automix_cfg","on":0|1,"resp_ms":F,"floor_db":F} */
		int on = -1;
		float resp = -1.0f, floordb = 1.0f;
		(void)json_get_int(line, "on", &on);
		(void)json_get_float(line, "resp_ms", &resp);
		(void)json_get_float(line, "floor_db", &floordb);
		pthread_mutex_lock(&g_st.target_lock);
		if (on >= 0)
			g_st.automix_on = on ? 1 : 0;
		if (resp >= 10.0f && resp <= 2000.0f)
			g_st.automix_resp_ms = resp;
		if (floordb <= 0.0f && floordb >= -40.0f)
			g_st.automix_floor = powf(10.0f, floordb / 20.0f);
		pthread_mutex_unlock(&g_st.target_lock);
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_automix_cfg\",\"on\":%d}\n",
			g_st.automix_on);

	} else if (json_has_op(line, "get_automix")) {
		/* état + gains courants (dB) pour la GUI */
		int n = snprintf(reply, sizeof(reply),
			"{\"ok\":true,\"on\":%d,\"resp_ms\":%.0f,"
			"\"floor_db\":%.1f,\"members\":[",
			g_st.automix_on, g_st.automix_resp_ms,
			20.0f * log10f(g_st.automix_floor + 1e-9f));
		for (int i = 0; i < N_INPUT_REAL; i++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%d",
				      i ? "," : "", g_st.automix_member[i]);
		n += snprintf(reply + n, sizeof(reply) - n, "],\"gains_db\":[");
		for (int i = 0; i < N_INPUT_REAL; i++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%.1f",
				      i ? "," : "",
				      20.0f * log10f(g_st.automix_gain[i] + 1e-9f));
		/* V12-AMX-UI : poids par tranche (dB) pour le panneau réglages */
		n += snprintf(reply + n, sizeof(reply) - n, "],\"weights_db\":[");
		for (int i = 0; i < N_INPUT_REAL; i++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%.1f",
				      i ? "," : "",
				      20.0f * log10f(g_st.automix_weight[i] + 1e-9f));
		n += snprintf(reply + n, sizeof(reply) - n, "]}\n");
		write(fd, reply, n);

	} else if (json_has_op(line, "set_expander")) {
		/* V12-EXP : {"op":"set_expander","src":N, on?, threshold_db?,
		 * ratio?, attack_ms?, release_ms?, range_db?, hold_ms?} —
		 * updates partiels : les champs absents gardent leur valeur. */
		int src = -1;
		if (json_get_int(line, "src", &src) < 0 ||
		    src < 0 || src >= N_EXP_CH) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad src\"}\n");
			return;
		}
		struct exp_ch *e = &g_exp[src];
		int on = e->on;
		float thr = e->thr_db, ratio = e->ratio, atk = e->atk_ms,
		      rel = e->rel_ms, rng = e->range_db, hold = e->hold_ms;
		(void)json_get_int(line, "on", &on);
		(void)json_get_float(line, "threshold_db", &thr);
		(void)json_get_float(line, "ratio", &ratio);
		(void)json_get_float(line, "attack_ms", &atk);
		(void)json_get_float(line, "release_ms", &rel);
		(void)json_get_float(line, "range_db", &rng);
		(void)json_get_float(line, "hold_ms", &hold);
		{
			const int lp = link_partner(src);   /* V13.3 */
			pthread_mutex_lock(&g_st.target_lock);
			exp_configure(src, on, thr, ratio, atk, rel, rng, hold);
			if (lp >= 0 && lp < N_EXP_CH)
				exp_configure(lp, on, thr, ratio, atk, rel,
					      rng, hold);
			pthread_mutex_unlock(&g_st.target_lock);
		}
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_expander\",\"src\":%d,"
			"\"on\":%d}\n", src, g_exp[src].on);

	} else if (json_has_op(line, "bandmix_role")) {
		/* V13 : {"op":"bandmix_role","src":N,"role":"lead|choir|..."} */
		int src = -1;
		char rn[16] = "";
		(void)json_get_int(line, "src", &src);
		(void)json_get_str(line, "role", rn, sizeof(rn));
		int role = -1;
		for (int r = 0; r < BR_NROLES; r++)
			if (!strcmp(rn, BR_NAMES[r])) role = r;
		if (src < 0 || src >= N_EXP_CH || role < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad role\"}\n");
			return;
		}
		g_bmx.role[src] = role;
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"src\":%d,\"role\":\"%s\"}\n",
			src, BR_NAMES[role]);

	} else if (json_has_op(line, "bandmix_measure")) {
		/* {"op":"bandmix_measure","src":N} — 12 s, auto-stop.
		 * src:-1 = annuler. */
		int src = -2;
		(void)json_get_int(line, "src", &src);
		if (src == -1) {
			atomic_store(&g_bmx.meas_src, -1);
			dprintf(fd, "{\"ok\":true,\"measuring\":-1}\n");
			return;
		}
		if (src < 0 || src >= N_EXP_CH ||
		    atomic_load(&g_bmx.meas_src) >= 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"busy or bad src\"}\n");
			return;
		}
		g_bmx.acc_ms = 0; g_bmx.nblk_s = 0;
		g_bmx.peak_max = 0; g_bmx.sm = 0;
		g_bmx.minsm = 1e9f; g_bmx.warm = 0;
		clock_gettime(CLOCK_MONOTONIC, &g_bmx.meas_t0);
		atomic_store(&g_bmx.meas_src, src);
		dprintf(fd, "{\"ok\":true,\"measuring\":%d,\"secs\":12}\n", src);

	} else if (json_has_op(line, "bandmix_calc")) {
		bmx_calc();
		dprintf(fd, "{\"ok\":true,\"op\":\"bandmix_calc\"}\n");

	} else if (json_has_op(line, "bandmix_lock")) {
		memset(g_bmx.lock_acc, 0, sizeof(g_bmx.lock_acc));
		g_bmx.lock_ticks = 0;
		g_bmx.locking = 1;
		dprintf(fd, "{\"ok\":true,\"op\":\"bandmix_lock\",\"secs\":30}\n");

	} else if (json_has_op(line, "bandmix_live")) {
		int on = 0;
		(void)json_get_int(line, "on", &on);
		g_bmx.live = on ? 1 : 0;
		if (!g_bmx.live) {
			/* retour doux à 0 dB */
			memset(g_bmx.kdb, 0, sizeof(g_bmx.kdb));
			pthread_mutex_lock(&g_st.target_lock);
			for (int i = 0; i < N_INPUT_TOTAL; i++)
				g_st.keeper_target[i] = 1.0f;
			pthread_mutex_unlock(&g_st.target_lock);
		}
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"live\":%d}\n", g_bmx.live);

	} else if (json_has_op(line, "bandmix_autolive")) {
		/* V13.5 : automix continu — un seul interrupteur, aucun
		 * soundcheck/verrouillage. {"op":"bandmix_autolive","on":0|1} */
		int on = 0;
		(void)json_get_int(line, "on", &on);
		g_bmx.autolive = on ? 1 : 0;
		if (g_bmx.autolive) {
			for (int i = 0; i < N_EXP_CH; i++)
				g_bmx.al_ref[i] = g_bmx.risk[i] = -120.0f;   /* recale les peak-holds */
			g_bmx.al_anchor = -120.0f;           /* ré-init de l'ancre */
			/* V13.9 — reset balance auto : les GAINS DE GROUPE sont
			 * CONSERVÉS (même groupe, même salle → volume plein dès
			 * la 1re seconde, exigence scène) ; on ne recale que le
			 * peak-hold programme, le staging (petites corrections
			 * rapides) et les compteurs d'activité. */
			g_bmx.prog_peak = -120.0f;
			g_bmx.bal_staged = 0;
			memset(g_bmx.act_ticks, 0, sizeof(g_bmx.act_ticks));
			/* V13.9 — reset solo (l'auto se re-déclenchera si mérité) */
			g_bmx.solo_src = -1;
			g_bmx.solo_is_auto = 0;
			g_bmx.solo_on_cnt = g_bmx.solo_off_cnt = 0;
			for (int i = 0; i < N_EXP_CH; i++)
				g_bmx.solo_base[i] = -999.0f;   /* base v2 à réapprendre */
			/* V13.6 : EQ de placement.
			 * V13.9 : le vfocus n'est PLUS forcé ici — un reset ne doit
			 * JAMAIS écraser un réglage posé par l'opérateur (le bouton
			 * PLACE À LA VOIX semblait « cassé » : choix OFF silencieuse-
			 * ment ré-armé à chaque lancement de morceau). */
			for (int i = 0; i < N_EXP_CH; i++)
				g_eqx.role_of[i] = -1;       /* force le recalcul coefs */
			atomic_store(&g_eqx.on, 1);
			/* V13.7 — étage master : EQ mastering + makeup LUFS */
			memset(g_meq_st, 0, sizeof(g_meq_st));
			g_meq_fading = 0;
			meq_init();   /* pose l'EQ direct (pas de fondu à l'activation) */
			memset(g_mk.k1, 0, sizeof(g_mk.k1));
			memset(g_mk.k2, 0, sizeof(g_mk.k2));
			g_mk.ms = 0.0f;
			g_mk.mk_db = 0.0f;
			g_mk.makeup_cur = 1.0f;
			atomic_store(&g_mk.makeup_mq, 1000);
			atomic_store(&g_mk.lufs_c, -12000);   /* gelé au démarrage */
			atomic_store(&g_master_on, 1);
		} else {
			memset(g_bmx.kdb, 0, sizeof(g_bmx.kdb));
			atomic_store(&g_eqx.on, 0);
			atomic_store(&g_master_on, 0);
			atomic_store(&g_mk.makeup_mq, 1000);
			pthread_mutex_lock(&g_st.target_lock);
			for (int i = 0; i < N_INPUT_TOTAL; i++)
				g_st.keeper_target[i] = 1.0f;
			/* V13.6 : coupe les comps auto (rôle ≠ off) posés par l'automix */
			for (int i = 0; i < N_EXP_CH; i++)
				if (g_bmx.role[i] != BR_OFF && g_cmp[i].on &&
				    BMX_P[g_bmx.role[i]].comp_on)
					cmp_configure(i, 0, g_cmp[i].thr_db,
						g_cmp[i].ratio, g_cmp[i].atk_ms,
						g_cmp[i].rel_ms, g_cmp[i].makeup_db);
			/* V13.9 : coupe aussi les GATES AUTO posées par l'automix */
			for (int i = 0; i < N_EXP_CH; i++)
				if (g_bmx.role[i] != BR_OFF && g_exp[i].on &&
				    BMX_P[g_bmx.role[i]].gate_on)
					exp_configure(i, 0, g_exp[i].thr_db,
						g_exp[i].ratio, g_exp[i].atk_ms,
						g_exp[i].rel_ms, g_exp[i].range_db,
						g_exp[i].hold_ms);
			pthread_mutex_unlock(&g_st.target_lock);
		}
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"autolive\":%d}\n", g_bmx.autolive);

	} else if (json_has_op(line, "bandmix_solo")) {
		/* V13.9 — SOLO : {"op":"bandmix_solo","src":-1..15,"auto":0/1}
		 * src = voie à soloer (−1 = aucun), pose un solo MANUEL (que
		 * l'auto ne relâche pas). auto = détection automatique on/off. */
		int iv;
		if (json_get_int(line, "src", &iv) >= 0 && iv >= -1 && iv < N_EXP_CH) {
			g_bmx.solo_src = iv;
			g_bmx.solo_is_auto = 0;
			g_bmx.solo_on_cnt = g_bmx.solo_off_cnt = 0;
		}
		if (json_get_int(line, "auto", &iv) >= 0) {
			g_bmx.solo_auto = iv ? 1 : 0;
			/* seul le choix auto est persisté (pas le solo ponctuel) */
			atomic_store(&g_presets_dirty, 1);
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"bandmix_solo\",\"src\":%d,"
			"\"auto\":%d,\"is_auto\":%d}\n",
			g_bmx.solo_src, g_bmx.solo_auto, g_bmx.solo_is_auto);

	} else if (json_has_op(line, "bandmix_status")) {
		int ms = atomic_load(&g_bmx.meas_src);
		int elapsed = 0;
		if (ms >= 0) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			elapsed = (int)(now.tv_sec - g_bmx.meas_t0.tv_sec);
		}
		int n = snprintf(reply, sizeof(reply),
			"{\"ok\":true,\"live\":%d,\"ref_valid\":%d,"
			"\"autolive\":%d,"
			"\"locking\":%d,\"measuring\":%d,\"meas_elapsed\":%d,"
			"\"solo\":%d,\"solo_auto\":%d,\"solo_is_auto\":%d,"
			"\"chans\":[",
			g_bmx.live, g_bmx.ref_valid, g_bmx.autolive,
			g_bmx.locking, ms, elapsed,
			g_bmx.solo_src, g_bmx.solo_auto, g_bmx.solo_is_auto);
		for (int i = 0; i < N_EXP_CH; i++) {
			n += snprintf(reply + n, sizeof(reply) - n,
				"%s{\"src\":%d,\"role\":\"%s\",\"done\":%d,"
				"\"rms_db\":%.1f,\"floor_db\":%.1f,"
				"\"keeper_db\":%.2f}",
				i ? "," : "", i, BR_NAMES[g_bmx.role[i]],
				g_bmx.m[i].done,
				g_bmx.m[i].done ? g_bmx.m[i].rms_avg_db : -99.0f,
				g_bmx.m[i].done ? g_bmx.m[i].floor_db : -99.0f,
				g_bmx.kdb[i]);
		}
		n += snprintf(reply + n, sizeof(reply) - n, "]}\n");
		write(fd, reply, n);

	} else if (json_has_op(line, "scene_save")) {
		/* V13-SCENES : {"op":"scene_save","slot":0-5,"name":"..."} */
		int slot = -1;
		char nm[48] = "";
		(void)json_get_int(line, "slot", &slot);
		(void)json_get_str(line, "name", nm, sizeof(nm));
		if (slot < 0 || slot >= SCENE_SLOTS) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad slot\"}\n");
			return;
		}
		mkdir(SCENE_DIR, 0755);
		char p[128];
		snprintf(p, sizeof(p), SCENE_DIR "/scene%d", slot);
		save_state_to(p);
		if (nm[0]) {
			snprintf(p, sizeof(p), SCENE_DIR "/scene%d.name", slot);
			FILE *nf = fopen(p, "w");
			if (nf) { fprintf(nf, "%s\n", nm); fclose(nf); }
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"scene_save\",\"slot\":%d}\n",
			slot);

	} else if (json_has_op(line, "scene_recall")) {
		int slot = -1;
		(void)json_get_int(line, "slot", &slot);
		if (slot < 0 || slot >= SCENE_SLOTS) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad slot\"}\n");
			return;
		}
		char p[128];
		snprintf(p, sizeof(p), SCENE_DIR "/scene%d", slot);
		if (scene_apply(p) == 0)
			dprintf(fd, "{\"ok\":true,\"op\":\"scene_recall\","
				"\"slot\":%d}\n", slot);
		else
			dprintf(fd, "{\"ok\":false,\"err\":\"scene vide\"}\n");

	} else if (json_has_op(line, "scene_list")) {
		int n = snprintf(reply, sizeof(reply),
				 "{\"ok\":true,\"scenes\":[");
		for (int s = 0; s < SCENE_SLOTS; s++) {
			char p[128], nm[48] = "";
			snprintf(p, sizeof(p), SCENE_DIR "/scene%d", s);
			int used = access(p, R_OK) == 0;
			snprintf(p, sizeof(p), SCENE_DIR "/scene%d.name", s);
			FILE *nf = fopen(p, "r");
			if (nf) {
				if (fgets(nm, sizeof(nm), nf)) {
					char *e = strchr(nm, '\n');
					if (e) *e = '\0';
				}
				fclose(nf);
			}
			if (!nm[0])
				snprintf(nm, sizeof(nm), "Scène %d", s + 1);
			n += snprintf(reply + n, sizeof(reply) - n,
				"%s{\"slot\":%d,\"used\":%d,\"name\":\"%s\"}",
				s ? "," : "", s, used, nm);
		}
		n += snprintf(reply + n, sizeof(reply) - n, "]}\n");
		write(fd, reply, n);

	} else if (json_has_op(line, "set_insert_bypass")) {
		/* V13-SCENES : bouton MASTERING ON/OFF (chaîne gardée chaude) */
		int on = 0;
		(void)json_get_int(line, "on", &on);
		atomic_store(&g_insert_bypass, on ? 0 : 1);   /* on=1 → actif */
		dprintf(fd, "{\"ok\":true,\"mastering_on\":%d}\n", on ? 1 : 0);

	} else if (json_has_op(line, "get_insert_bypass")) {
		dprintf(fd, "{\"ok\":true,\"chain\":%d,\"bypass\":%d,"
			"\"mastering_on\":%d}\n",
			atomic_load(&g_insert_active),
			atomic_load(&g_insert_bypass),
			atomic_load(&g_insert_active) &&
			!atomic_load(&g_insert_bypass));

	} else if (json_has_op(line, "set_vfocus")) {
		/* V13-VFOCUS : {"op":"set_vfocus", on?, amount?(0-100),
		 * max_cut_db?} — updates partiels */
		int on = g_vf.on;
		float am = -1.0f, mc = -1.0f;
		(void)json_get_int(line, "on", &on);
		(void)json_get_float(line, "amount", &am);
		(void)json_get_float(line, "max_cut_db", &mc);
		pthread_mutex_lock(&g_st.target_lock);
		g_vf.on = on ? 1 : 0;
		if (am >= 0.0f && am <= 100.0f)
			g_vf.amount = am / 100.0f;
		if (mc >= 0.0f && mc <= 12.0f)
			g_vf.max_cut_db = mc;
		pthread_mutex_unlock(&g_st.target_lock);
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_vfocus\",\"on\":%d}\n",
			g_vf.on);

	} else if (json_has_op(line, "get_vfocus")) {
		int n = snprintf(reply, sizeof(reply),
			"{\"ok\":true,\"on\":%d,\"amount\":%.0f,"
			"\"max_cut_db\":%.1f,\"active\":%d,\"cuts_db\":[",
			g_vf.on, g_vf.amount * 100.0f, g_vf.max_cut_db,
			atomic_load_explicit(&g_vf.active,
					     memory_order_relaxed));
		for (int b = 0; b < VF_BANDS; b++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%.2f",
				      b ? "," : "",
				      atomic_load_explicit(&g_vf.pub_cut[b],
							   memory_order_relaxed)
					/ 1000.0f);
		n += snprintf(reply + n, sizeof(reply) - n, "]}\n");
		write(fd, reply, n);

	} else if (json_has_op(line, "set_comp")) {
		/* V13-COMP : updates partiels comme set_expander */
		int src = -1;
		if (json_get_int(line, "src", &src) < 0 ||
		    src < 0 || src >= N_EXP_CH) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad src\"}\n");
			return;
		}
		struct cmp_ch *c = &g_cmp[src];
		int on = c->on;
		float thr = c->thr_db, ratio = c->ratio, atk = c->atk_ms,
		      rel = c->rel_ms, mk = c->makeup_db;
		(void)json_get_int(line, "on", &on);
		(void)json_get_float(line, "threshold_db", &thr);
		(void)json_get_float(line, "ratio", &ratio);
		(void)json_get_float(line, "attack_ms", &atk);
		(void)json_get_float(line, "release_ms", &rel);
		(void)json_get_float(line, "makeup_db", &mk);
		{
			const int lp = link_partner(src);   /* V13.3 */
			pthread_mutex_lock(&g_st.target_lock);
			cmp_configure(src, on, thr, ratio, atk, rel, mk);
			if (lp >= 0 && lp < N_EXP_CH)
				cmp_configure(lp, on, thr, ratio, atk, rel, mk);
			pthread_mutex_unlock(&g_st.target_lock);
		}
		atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_comp\",\"src\":%d,"
			"\"on\":%d}\n", src, g_cmp[src].on);

	} else if (json_has_op(line, "get_comp")) {
		int n = snprintf(reply, sizeof(reply),
				 "{\"ok\":true,\"channels\":[");
		for (int i = 0; i < N_EXP_CH; i++) {
			struct cmp_ch *c = &g_cmp[i];
			n += snprintf(reply + n, sizeof(reply) - n,
				"%s{\"src\":%d,\"on\":%d,\"threshold_db\":%.1f,"
				"\"ratio\":%.1f,\"attack_ms\":%.1f,"
				"\"release_ms\":%.0f,\"makeup_db\":%.1f,"
				"\"gr_db\":%.1f}",
				i ? "," : "", i, c->on, c->thr_db, c->ratio,
				c->atk_ms, c->rel_ms, c->makeup_db,
				atomic_load_explicit(&c->gr_mdb,
						     memory_order_relaxed)
					/ -1000.0f);
		}
		n += snprintf(reply + n, sizeof(reply) - n, "]}\n");
		write(fd, reply, n);

	} else if (json_has_op(line, "get_expander")) {
		/* état complet + GR courant (milli-dB → dB) pour la GUI */
		int n = snprintf(reply, sizeof(reply),
				 "{\"ok\":true,\"channels\":[");
		for (int i = 0; i < N_EXP_CH; i++) {
			struct exp_ch *e = &g_exp[i];
			n += snprintf(reply + n, sizeof(reply) - n,
				"%s{\"src\":%d,\"on\":%d,\"threshold_db\":%.1f,"
				"\"ratio\":%.1f,\"attack_ms\":%.1f,"
				"\"release_ms\":%.0f,\"range_db\":%.0f,"
				"\"hold_ms\":%.0f,\"gr_db\":%.1f}",
				i ? "," : "", i, e->on, e->thr_db, e->ratio,
				e->atk_ms, e->rel_ms, e->range_db, e->hold_ms,
				atomic_load_explicit(&e->gr_mdb,
						     memory_order_relaxed)
					/ -1000.0f);
		}
		n += snprintf(reply + n, sizeof(reply) - n, "]}\n");
		write(fd, reply, n);

	} else if (json_has_op(line, "get_midix")) {
		/* V12-MIDIX : présence du module + santé pour la GUI */
		struct midix_hdr *h = atomic_load(&g_midix.hdr);
		dprintf(fd, "{\"ok\":true,\"present\":%d,\"underruns\":%u,"
			"\"peak\":%u,\"gain\":%.2f}\n",
			h ? 1 : 0,
			atomic_load(&g_midix.underruns),
			atomic_load(&g_midix.peak), g_midix.gain);

	} else if (json_has_op(line, "set_midix")) {
		/* {"op":"set_midix","gain":F} — trim du module dans P1/P2 */
		float g = -1.0f;
		(void)json_get_float(line, "gain", &g);
		if (g >= 0.0f && g <= 4.0f)
			g_midix.gain = g;
		dprintf(fd, "{\"ok\":true,\"op\":\"set_midix\",\"gain\":%.2f}\n",
			g_midix.gain);

	} else if (json_has_op(line, "midix_ctl")) {
		/* V12-MIDIX-GUI — proxy vers le daemon midi-expander (la GUI
		 * n'a qu'un canal : ce socket). {"op":"midix_ctl","cmd":
		 * "status"|"prog"|"gain"|"panic", chan?, num?, value?}.
		 * Control thread uniquement (jamais l'audio) ; si le daemon
		 * est absent, connect échoue immédiatement (pas de blocage). */
		char cmd[16] = "", raw[192] = "";
		int chan = -1, num = -1;
		float val = -1.0f;
		(void)json_get_str(line, "cmd", cmd, sizeof(cmd));
		(void)json_get_str(line, "line", raw, sizeof(raw));
		(void)json_get_int(line, "chan", &chan);
		(void)json_get_int(line, "num", &num);
		(void)json_get_float(line, "value", &val);
		char req[224];
		if (raw[0])   /* V12-SYNTH : passthrough générique (engine,
			       * inst_list, patch_get/set/save…) */
			snprintf(req, sizeof(req), "%s\n", raw);
		else if (!strcmp(cmd, "status"))
			snprintf(req, sizeof(req), "status\n");
		else if (!strcmp(cmd, "prog") && chan >= 0 && chan < 16 &&
			 num >= 0 && num < 128)
			snprintf(req, sizeof(req), "prog %d %d\n", chan, num);
		else if (!strcmp(cmd, "gain") && val >= 0.0f && val <= 10.0f)
			snprintf(req, sizeof(req), "gain %.3f\n", val);
		else if (!strcmp(cmd, "panic"))
			snprintf(req, sizeof(req), "panic\n");
		else {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad midix cmd\"}\n");
			return;
		}
		int s = socket(AF_UNIX, SOCK_STREAM, 0);
		struct sockaddr_un sa = { .sun_family = AF_UNIX };
		snprintf(sa.sun_path, sizeof(sa.sun_path),
			 "/run/midi-expander.sock");
		struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		/* réponses longues (inst_list ~8 Ko) : lecture en boucle
		 * jusqu'au '\n' final. Thread ctl unique → static ok. */
		static char resp[16384];
		ssize_t rn = 0;
		if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
		    write(s, req, strlen(req)) > 0) {
			while (rn < (ssize_t)sizeof(resp) - 1) {
				ssize_t k = read(s, resp + rn,
						 sizeof(resp) - 1 - (size_t)rn);
				if (k <= 0)
					break;
				rn += k;
				if (resp[rn - 1] == '\n')
					break;
			}
		}
		close(s);
		if (rn > 0)
			write(fd, resp, (size_t)rn);
		else
			dprintf(fd, "{\"ok\":false,\"err\":\"expander absent\"}\n");

	} else if (json_has_op(line, "get_assistant")) {
		/* Renvoie état Mixer Assistant. Le daemon mixer-ml-inference
		 * poll cet endpoint pour savoir source/mode actuels. */
		int mode = atomic_load_explicit(&g_assistant_mode,   memory_order_relaxed);
		int src  = atomic_load_explicit(&g_assistant_source, memory_order_relaxed);
		dprintf(fd, "{\"ok\":true,\"mode\":\"%s\",\"source\":\"%s\"}\n",
		        mode ? "mastering" : "passthrough",
		        src  ? "usb"       : "hw");

	} else if (json_has_op(line, "insert_bypass")) {
		/* Format : {"op":"insert_bypass","bypass":true|false}.
		 * Quand bypass=true : désactive l'insert sans free la chain
		 * (réactivable par bypass=false instantanément). */
		int bypass_flag = 1;   /* default true si pas spécifié */
		(void)json_get_int(line, "bypass", &bypass_flag);
		atomic_store(&g_insert_active, bypass_flag ? 0 : 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"insert_bypass\",\"active\":%s}\n",
		        bypass_flag ? "false" : "true");

	} else if (json_has_op(line, "set_alsa")) {
		/* V9.4.1 — Set ALSA control (INTEGER seul pour V9.4.1).
		 * Format :
		 *   {"op":"set_alsa","name":"PGA2.0 2 Out Strip1 Volume","value":50}
		 *   {"op":"set_alsa","numid":324,"value":50}
		 *
		 * Hardcode card "softac5212tdm" (le seul DSP HiFi4 SOF expose les
		 * kcontrols MULTIBAND_DRC + PGA + TAC BQ). Pour BYTES blob (DRC
		 * raw), implementation en V9.4.2 (besoin parser hex/base64). */
		char ctrl_name[128] = {0};
		int numid = 0;
		float value = 0;
		int by_numid = (json_get_int(line, "numid", &numid) == 0);
		int by_name  = (json_get_str(line, "name", ctrl_name, sizeof(ctrl_name)) == 0);
		/* V9.4.3 : "value" optionnel — pas requis pour BYTES (qui prend "bytes"). */
		int has_value = (json_get_float(line, "value", &value) == 0);
		if (!by_numid && !by_name) {
			dprintf(fd, "{\"ok\":false,\"err\":\"need numid or name\"}\n");
			return;
		}
		snd_ctl_t *h = NULL;
		if (snd_ctl_open(&h, "hw:CARD=softac5212tdm", 0) < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"snd_ctl_open failed\"}\n");
			return;
		}
		snd_ctl_elem_id_t *id;
		snd_ctl_elem_id_alloca(&id);
		if (by_numid) snd_ctl_elem_id_set_numid(id, numid);
		else {
			snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
			snd_ctl_elem_id_set_name(id, ctrl_name);
		}
		snd_ctl_elem_info_t *info;
		snd_ctl_elem_info_alloca(&info);
		snd_ctl_elem_info_set_id(info, id);
		int rc = snd_ctl_elem_info(h, info);
		if (rc < 0) {
			snd_ctl_close(h);
			dprintf(fd, "{\"ok\":false,\"err\":\"control not found\"}\n");
			return;
		}
		snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
		snd_ctl_elem_value_t *val;
		snd_ctl_elem_value_alloca(&val);
		snd_ctl_elem_value_set_id(val, id);
		if (type == SND_CTL_ELEM_TYPE_INTEGER) {
			if (!has_value) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"INTEGER needs value\"}\n");
				return;
			}
			unsigned int n_chan = snd_ctl_elem_info_get_count(info);
			for (unsigned int c = 0; c < n_chan; c++)
				snd_ctl_elem_value_set_integer(val, c, (long)value);
		} else if (type == SND_CTL_ELEM_TYPE_BOOLEAN) {
			if (!has_value) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"BOOLEAN needs value\"}\n");
				return;
			}
			snd_ctl_elem_value_set_boolean(val, 0, value != 0.0f);
		} else if (type == SND_CTL_ELEM_TYPE_BYTES) {
			/* V9.4.3 : parse "bytes":"<hex>" → raw bytes → snd_ctl set.
			 * Pour DRC blob 4096 octets = 8192 chars hex requis.
			 * Le control count = nb max d'octets attendu. */
			const char *hex_p = strstr(line, "\"bytes\"");
			if (!hex_p) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"BYTES needs hex field\"}\n");
				return;
			}
			hex_p = strchr(hex_p, '"');     /* skip "bytes" */
			if (hex_p) hex_p = strchr(hex_p + 1, '"');   /* skip : */
			if (hex_p) hex_p = strchr(hex_p + 1, '"');   /* opening " of value */
			if (!hex_p) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"bad bytes format\"}\n");
				return;
			}
			hex_p++;
			const char *hex_end = strchr(hex_p, '"');
			if (!hex_end) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"unclosed bytes\"}\n");
				return;
			}
			size_t hex_len = (size_t)(hex_end - hex_p);
			if (hex_len % 2 != 0) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"odd hex length\"}\n");
				return;
			}
			size_t n_bytes = hex_len / 2;
			unsigned int max_bytes = snd_ctl_elem_info_get_count(info);
			if (n_bytes > max_bytes) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"too many bytes (%zu > %u)\"}\n",
				        n_bytes, max_bytes);
				return;
			}
			/* Parse hex into raw bytes — local stack buf 4 KB suffit pour DRC */
			static unsigned char raw[4096];
			for (size_t i = 0; i < n_bytes && i < sizeof(raw); i++) {
				char c1 = hex_p[i*2], c2 = hex_p[i*2 + 1];
				int hi = (c1 <= '9') ? c1 - '0' : ((c1 | 0x20) - 'a' + 10);
				int lo = (c2 <= '9') ? c2 - '0' : ((c2 | 0x20) - 'a' + 10);
				if (hi < 0 || hi > 15 || lo < 0 || lo > 15) {
					snd_ctl_close(h);
					dprintf(fd, "{\"ok\":false,\"err\":\"bad hex char\"}\n");
					return;
				}
				raw[i] = (unsigned char)((hi << 4) | lo);
			}
			for (size_t i = 0; i < n_bytes; i++)
				snd_ctl_elem_value_set_byte(val, (unsigned int)i, raw[i]);
		} else {
			snd_ctl_close(h);
			dprintf(fd, "{\"ok\":false,\"err\":\"unsupported control type\"}\n");
			return;
		}
		rc = snd_ctl_elem_write(h, val);
		snd_ctl_close(h);
		if (rc < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"snd_ctl_elem_write failed\"}\n");
		} else {
			dprintf(fd, "{\"ok\":true,\"op\":\"set_alsa\",\"value\":%.4f}\n", value);
		}

	} else if (json_has_op(line, "get_alsa")) {
		/* Format : {"op":"get_alsa","name":"..."} ou numid */
		char ctrl_name[128] = {0};
		int numid = 0;
		int by_numid = (json_get_int(line, "numid", &numid) == 0);
		int by_name  = (json_get_str(line, "name", ctrl_name, sizeof(ctrl_name)) == 0);
		if (!by_numid && !by_name) { dprintf(fd, "{\"ok\":false,\"err\":\"bad args\"}\n"); return; }
		snd_ctl_t *h = NULL;
		if (snd_ctl_open(&h, "hw:CARD=softac5212tdm", 0) < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"snd_ctl_open failed\"}\n"); return;
		}
		snd_ctl_elem_id_t *id;
		snd_ctl_elem_id_alloca(&id);
		if (by_numid) snd_ctl_elem_id_set_numid(id, numid);
		else { snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
		       snd_ctl_elem_id_set_name(id, ctrl_name); }
		/* V9.4.3 : type-aware read. Lookup type via info pour distinguer
		 * INTEGER (value:N) de BYTES (bytes:"hex"). */
		snd_ctl_elem_info_t *ginfo;
		snd_ctl_elem_info_alloca(&ginfo);
		snd_ctl_elem_info_set_id(ginfo, id);
		if (snd_ctl_elem_info(h, ginfo) < 0) {
			snd_ctl_close(h);
			dprintf(fd, "{\"ok\":false,\"err\":\"info failed\"}\n"); return;
		}
		snd_ctl_elem_type_t gtype = snd_ctl_elem_info_get_type(ginfo);
		snd_ctl_elem_value_t *val;
		snd_ctl_elem_value_alloca(&val);
		snd_ctl_elem_value_set_id(val, id);
		if (snd_ctl_elem_read(h, val) < 0) {
			snd_ctl_close(h);
			dprintf(fd, "{\"ok\":false,\"err\":\"read failed\"}\n"); return;
		}
		if (gtype == SND_CTL_ELEM_TYPE_BYTES) {
			unsigned int n_bytes = snd_ctl_elem_info_get_count(ginfo);
			if (n_bytes > 4096) n_bytes = 4096;   /* cap pour hex 8 KB output */
			static char hex_out[8200];
			for (unsigned int i = 0; i < n_bytes; i++) {
				unsigned char b = snd_ctl_elem_value_get_byte(val, i);
				static const char hex_chars[] = "0123456789abcdef";
				hex_out[i*2]     = hex_chars[(b >> 4) & 0xF];
				hex_out[i*2 + 1] = hex_chars[b & 0xF];
			}
			hex_out[n_bytes * 2] = '\0';
			snd_ctl_close(h);
			dprintf(fd, "{\"ok\":true,\"bytes\":\"%s\",\"len\":%u}\n", hex_out, n_bytes);
		} else {
			long v = snd_ctl_elem_value_get_integer(val, 0);
			snd_ctl_close(h);
			dprintf(fd, "{\"ok\":true,\"value\":%ld}\n", v);
		}

	} else if (json_has_op(line, "set_tac_reg")) {
		/* V9.4.2 — Set TAC5212 codec register via i2c-3.
		 * Format : {"op":"set_tac_reg","tac":0..3,"reg":0xRR,"value":0xVV}
		 *
		 * 4 codecs TAC5212 aux addresses 0x50, 0x51, 0x52, 0x53 sur /dev/i2c-3.
		 * Le codec utilise des pages registres (reg 0x00 = page select).
		 * NPU peut writer reg 0x00 séparément pour switcher page.
		 *
		 * I2C_SLAVE_FORCE car le driver tac5212 kernel tient le device.
		 * Risque : désync driver/hw si on touche les registres init driver.
		 * En pratique le NPU vise les registres DRC/limiter/BQ que le driver
		 * ne reset jamais après init. */
		int tac_idx = 0, reg = 0;
		float val_f = 0;
		if (json_get_int(line, "tac", &tac_idx) < 0 ||
		    json_get_int(line, "reg", &reg) < 0 ||
		    json_get_float(line, "value", &val_f) < 0 ||
		    tac_idx < 0 || tac_idx > 3 ||
		    reg < 0 || reg > 0xFF) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad args (tac 0..3, reg 0..255)\"}\n");
			return;
		}
		int fd_i2c = open("/dev/i2c-3", O_RDWR);
		if (fd_i2c < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"open i2c-3 failed: %s\"}\n", strerror(errno));
			return;
		}
		if (ioctl(fd_i2c, 0x0706 /*I2C_SLAVE_FORCE*/, 0x50 + tac_idx) < 0) {
			close(fd_i2c);
			dprintf(fd, "{\"ok\":false,\"err\":\"ioctl I2C_SLAVE_FORCE failed: %s\"}\n", strerror(errno));
			return;
		}
		uint8_t buf[2] = { (uint8_t)reg, (uint8_t)(int)val_f };
		ssize_t w = write(fd_i2c, buf, 2);
		close(fd_i2c);
		if (w != 2) {
			dprintf(fd, "{\"ok\":false,\"err\":\"i2c write failed\"}\n");
		} else {
			dprintf(fd, "{\"ok\":true,\"op\":\"set_tac_reg\",\"tac\":%d,"
			            "\"reg\":%d,\"value\":%d}\n",
			        tac_idx, reg, (int)val_f);
		}

	} else if (json_has_op(line, "get_tac_reg")) {
		/* Format : {"op":"get_tac_reg","tac":0..3,"reg":0xRR}
		 * → {"ok":true,"value":N} */
		int tac_idx = 0, reg = 0;
		if (json_get_int(line, "tac", &tac_idx) < 0 ||
		    json_get_int(line, "reg", &reg) < 0 ||
		    tac_idx < 0 || tac_idx > 3 || reg < 0 || reg > 0xFF) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad args\"}\n");
			return;
		}
		int fd_i2c = open("/dev/i2c-3", O_RDWR);
		if (fd_i2c < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"open failed\"}\n"); return;
		}
		if (ioctl(fd_i2c, 0x0706 /*I2C_SLAVE_FORCE*/, 0x50 + tac_idx) < 0) {
			close(fd_i2c);
			dprintf(fd, "{\"ok\":false,\"err\":\"ioctl failed\"}\n"); return;
		}
		uint8_t r = (uint8_t)reg, v = 0;
		ssize_t ww = write(fd_i2c, &r, 1);
		ssize_t rr = read(fd_i2c, &v, 1);
		close(fd_i2c);
		if (ww != 1 || rr != 1) {
			dprintf(fd, "{\"ok\":false,\"err\":\"i2c read failed\"}\n");
		} else {
			dprintf(fd, "{\"ok\":true,\"value\":%d}\n", v);
		}

	} else if (json_has_op(line, "list_lv2_plugins")) {
		/* V9.2 — Énumère les plugins LV2 RT-safe disponibles. */
		static char lv2_buf[65536];
		int n = fx_lv2_list_uris(lv2_buf, sizeof(lv2_buf));
		dprintf(fd, "{\"ok\":true,\"op\":\"list_lv2_plugins\",\"plugins\":%s}\n",
		        n > 0 ? lv2_buf : "[]");

	} else if (json_has_op(line, "set_input_map")) {
		/* V9.5.21 — remap mic DSP : {"op":"set_input_map","mic":I,"slot":S}
		 * (un mic) ou {"op":"set_input_map","map":[s0..s7]} (les 8). */
		int mic, slot;
		if (json_get_int(line, "mic", &mic) >= 0 &&
		    json_get_int(line, "slot", &slot) >= 0 &&
		    mic >= 0 && mic < 8 && slot >= 0 && slot < 8) {
			atomic_store_explicit(&g_mic_map[mic], slot, memory_order_relaxed);
			atomic_store(&g_presets_dirty, 1);
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"set_input_map\",\"map\":[");
		for (int i = 0; i < 8; i++)
			dprintf(fd, "%s%d", i ? "," : "",
			        atomic_load_explicit(&g_mic_map[i], memory_order_relaxed));
		dprintf(fd, "]}\n");

	} else if (json_has_op(line, "get_input_map")) {
		dprintf(fd, "{\"ok\":true,\"op\":\"get_input_map\",\"map\":[");
		for (int i = 0; i < 8; i++)
			dprintf(fd, "%s%d", i ? "," : "",
			        atomic_load_explicit(&g_mic_map[i], memory_order_relaxed));
		dprintf(fd, "]}\n");

	} else if (json_has_op(line, "set_output_gain")) {
		/* V9.5.21 — gain d'une sortie : {"op":"set_output_gain","out":O,"db":X}
		 * out : 0..N_OUTPUT_TOTAL-1 (0-7 DSP, 8-15 USB, 16-17 phone).
		 * db : -60..+12 dB (ou "gain" linéaire direct). */
		int out;
		float db, gain;
		if (json_get_int(line, "out", &out) >= 0 &&
		    out >= 0 && out < N_OUTPUT_TOTAL) {
			float g = 1.0f;
			if (json_get_float(line, "db", &db) >= 0)
				g = (db <= -60.0f) ? 0.0f : powf(10.0f, db / 20.0f);
			else if (json_get_float(line, "gain", &gain) >= 0)
				g = gain;
			int gm = (int)(g * 1000.0f + 0.5f);
			if (gm < 0) gm = 0;
			if (gm > 4000) gm = 4000;
			atomic_store_explicit(&g_out_gain_m[out], gm, memory_order_relaxed);
			atomic_store(&g_presets_dirty, 1);
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"set_output_gain\",\"gains\":[");
		for (int o = 0; o < N_OUTPUT_TOTAL; o++)
			dprintf(fd, "%s%d", o ? "," : "",
			        atomic_load_explicit(&g_out_gain_m[o], memory_order_relaxed));
		dprintf(fd, "]}\n");

	} else if (json_has_op(line, "get_output_gain")) {
		dprintf(fd, "{\"ok\":true,\"op\":\"get_output_gain\",\"gains\":[");
		for (int o = 0; o < N_OUTPUT_TOTAL; o++)
			dprintf(fd, "%s%d", o ? "," : "",
			        atomic_load_explicit(&g_out_gain_m[o], memory_order_relaxed));
		dprintf(fd, "]}\n");

	} else if (json_has_op(line, "master_eq")) {
		/* V13.7 — EQ de mastering master (3 bandes), réglable en direct :
		 * {"op":"master_eq","low_db":..,"low_hz":..,"mid_db":..,"mid_hz":..,
		 *  "mid_q":..,"air_db":..,"air_hz":..} — champs absents = inchangés.
		 * Sans champ = simple lecture (makeup_db/lufs courants inclus). */
		float v; int ch = 0;
		if (json_get_float(line, "low_hz", &v) >= 0) { g_meq_p.low_hz = v; ch = 1; }
		if (json_get_float(line, "low_db", &v) >= 0) { g_meq_p.low_db = v; ch = 1; }
		if (json_get_float(line, "mid_hz", &v) >= 0) { g_meq_p.mid_hz = v; ch = 1; }
		if (json_get_float(line, "mid_db", &v) >= 0) { g_meq_p.mid_db = v; ch = 1; }
		if (json_get_float(line, "mid_q",  &v) >= 0) { g_meq_p.mid_q  = v; ch = 1; }
		if (json_get_float(line, "air_hz", &v) >= 0) { g_meq_p.air_hz = v; ch = 1; }
		if (json_get_float(line, "air_db", &v) >= 0) { g_meq_p.air_db = v; ch = 1; }
		if (ch) {   /* seulement si un champ a changé (sinon = lecture pure,
		             * pas de crossfade ni d'écriture flash sur un poll GUI) */
			meq_recalc();
			save_master_eq();
			atomic_store(&g_presets_dirty, 1);   /* V13.9 : EQ aussi
			                                      * dans l'état/scènes */
		}
		dprintf(fd, "{\"ok\":true,\"op\":\"master_eq\","
			"\"low_db\":%.2f,\"low_hz\":%.1f,\"mid_db\":%.2f,"
			"\"mid_hz\":%.1f,\"mid_q\":%.2f,\"air_db\":%.2f,"
			"\"air_hz\":%.1f,\"makeup_db\":%.2f,\"lufs\":%.2f}\n",
			g_meq_p.low_db, g_meq_p.low_hz, g_meq_p.mid_db,
			g_meq_p.mid_hz, g_meq_p.mid_q, g_meq_p.air_db,
			g_meq_p.air_hz, g_mk.mk_db,
			atomic_load_explicit(&g_mk.lufs_c, memory_order_relaxed) * 0.01f);

	} else if (json_has_op(line, "automix_tune")) {
		/* V13.9 — tunables automix réglables en LIVE (R&D) :
		 * {"op":"automix_tune","freeze_db":..,"risk_decay":..,"risk_margin":..}
		 * champs absents = inchangés ; sans champ = lecture. */
		float v; int chg = 0;
		if (json_get_float(line, "freeze_db",   &v) >= 0 && v >= 3.0f && v <= 40.0f)
			{ g_bmx.freeze_db = v; chg = 1; }
		if (json_get_float(line, "risk_decay",  &v) >= 0 && v >= 0.0f && v <= 2.0f)
			{ g_bmx.risk_decay = v; chg = 1; }
		if (json_get_float(line, "risk_margin", &v) >= 0 && v >= 0.0f && v <= 12.0f)
			{ g_bmx.risk_margin = v; chg = 1; }
		if (json_get_float(line, "gate_db", &v) >= 0 && v >= 3.0f && v <= 30.0f)
			{ g_bmx.gate_db = v; chg = 1; }
		if (chg)   /* pollé en lecture par la GUI : dirty SEULEMENT si set */
			atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"automix_tune\",\"freeze_db\":%.1f,"
			"\"risk_decay\":%.3f,\"risk_margin\":%.1f,\"gate_db\":%.1f}\n",
			g_bmx.freeze_db, g_bmx.risk_decay, g_bmx.risk_margin,
			g_bmx.gate_db);

	} else if (json_has_op(line, "set_vspatial")) {
		/* V13.9 — spatializer voix (widener Lauridsen LEAD+CHŒURS) :
		 * {"op":"set_vspatial","on":0/1,"amount":0..100,"delay_ms":3..40}
		 * champs absents = inchangés ; toujours renvoie l'état courant. */
		int iv; float v; int chg = 0;
		if (json_get_int(line, "on", &iv) >= 0) {
			atomic_store_explicit(&g_vspat.on, iv ? 1 : 0,
					      memory_order_relaxed);
			chg = 1;
		}
		if (json_get_float(line, "amount", &v) >= 0 && v >= 0.0f && v <= 100.0f) {
			atomic_store_explicit(&g_vspat.amount_mq, (int)(v * 10.0f + 0.5f),
					      memory_order_relaxed);
			chg = 1;
		}
		if (json_get_float(line, "delay_ms", &v) >= 0 && v >= 3.0f && v <= 40.0f) {
			atomic_store_explicit(&g_vspat.delay_smp,
					      (int)(v * SAMPLE_RATE / 1000.0f),
					      memory_order_relaxed);
			chg = 1;
		}
		if (chg)   /* pollé en lecture par la GUI : dirty SEULEMENT si set */
			atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_vspatial\",\"on\":%d,"
			"\"amount\":%.0f,\"delay_ms\":%.1f}\n",
			atomic_load_explicit(&g_vspat.on, memory_order_relaxed),
			atomic_load_explicit(&g_vspat.amount_mq, memory_order_relaxed) / 10.0,
			atomic_load_explicit(&g_vspat.delay_smp, memory_order_relaxed)
				* 1000.0 / SAMPLE_RATE);

	} else if (json_has_op(line, "set_balance")) {
		/* V13.9 — BALANCE AUTO (table quadrants) : tient LUFS=lufs_tgt ET
		 * écart voix−musique = e_tgt en bougeant les gains de groupe.
		 * {"op":"set_balance","on":0/1,"lufs_tgt":-30..-6,"e_tgt":-6..12}
		 * absent=inchangé. Renvoie l'état + gains groupe + LUFS mesuré. */
		int iv; float v; int chg = 0;
		if (json_get_int(line, "on", &iv) >= 0)
			{ g_bmx.balance_on = iv ? 1 : 0; chg = 1; }
		if (json_get_float(line, "lufs_tgt", &v) >= 0 && v >= -30.0f && v <= -6.0f)
			{ g_bmx.bal_lufs_tgt = v; chg = 1; }
		if (json_get_float(line, "e_tgt", &v) >= 0 && v >= -6.0f && v <= 12.0f)
			{ g_bmx.bal_e_tgt = v; chg = 1; }
		if (json_get_float(line, "c_tgt", &v) >= 0 && v >= -6.0f && v <= 12.0f)
			{ g_bmx.bal_c_tgt = v; chg = 1; }
		if (chg)   /* pollé en lecture par la GUI : dirty SEULEMENT si set */
			atomic_store(&g_presets_dirty, 1);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_balance\",\"on\":%d,"
			"\"lufs_tgt\":%.1f,\"e_tgt\":%.1f,\"c_tgt\":%.1f,"
			"\"voice_db\":%.1f,\"choir_db\":%.1f,\"music_db\":%.1f,"
			"\"lufs\":%.1f}\n",
			g_bmx.balance_on, g_bmx.bal_lufs_tgt, g_bmx.bal_e_tgt,
			g_bmx.bal_c_tgt, g_bmx.g_voice_db, g_bmx.g_choir_db,
			g_bmx.g_music_db,
			atomic_load_explicit(&g_mk.lufs_c, memory_order_relaxed) * 0.01);

	} else if (json_has_op(line, "get_meters_lite")) {
		/* V10-N2 : peaks seuls (in/out/fx), SANS le payload analyzer
		 * (~4.8 KB) — pour l'app native mixer-console qui poll à 30 Hz
		 * et n'affiche pas encore de spectre. */
		int n = 0;
		n += snprintf(reply + n, sizeof(reply) - n, "{\"ok\":true,\"in\":[");
		for (int i = 0; i < N_INPUT_TOTAL && n < (int)sizeof(reply); i++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%u", i ? "," : "",
				      atomic_load_explicit(&g_st.peak_in[i], memory_order_relaxed));
		n += snprintf(reply + n, sizeof(reply) - n, "],\"out\":[");
		for (int o = 0; o < N_OUTPUT_TOTAL && n < (int)sizeof(reply); o++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%u", o ? "," : "",
				      atomic_load_explicit(&g_st.peak_out[o], memory_order_relaxed));
		n += snprintf(reply + n, sizeof(reply) - n, "],\"fx\":[");
		for (int b = 0; b < N_BUS_FX_CH && n < (int)sizeof(reply); b++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%u", b ? "," : "",
				      atomic_load_explicit(&g_st.peak_fx[b], memory_order_relaxed));
		n += snprintf(reply + n, sizeof(reply) - n, "]}\n");
		write(fd, reply, n);

	} else if (json_has_op(line, "get_meters")) {
		/* E7.1 + E7.5 : retourne peaks + analyzer (spectrum + scope) en
		 * un seul round-trip, consommé par mixer-gui-http /api/stream.
		 * Conversion dBFS peaks côté client : 20*log10(peak/2147483648).
		 */
		static char reply[16384];
		int n = 0;
		n += snprintf(reply + n, sizeof(reply) - n, "{\"ok\":true,\"in\":[");
		for (int i = 0; i < N_INPUT_TOTAL && n < (int)sizeof(reply); i++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%u", i ? "," : "",
				      atomic_load_explicit(&g_st.peak_in[i], memory_order_relaxed));
		n += snprintf(reply + n, sizeof(reply) - n, "],\"out\":[");
		for (int o = 0; o < N_OUTPUT_TOTAL && n < (int)sizeof(reply); o++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%u", o ? "," : "",
				      atomic_load_explicit(&g_st.peak_out[o], memory_order_relaxed));
		n += snprintf(reply + n, sizeof(reply) - n, "],\"fx\":[");
		for (int b = 0; b < N_BUS_FX_CH && n < (int)sizeof(reply); b++)
			n += snprintf(reply + n, sizeof(reply) - n, "%s%u", b ? "," : "",
				      atomic_load_explicit(&g_st.peak_fx[b], memory_order_relaxed));
		n += snprintf(reply + n, sizeof(reply) - n, "],\"analyzer\":[");
		for (int t = 0; t < N_TAPS && n < (int)sizeof(reply); t++) {
			int k = atomic_load_explicit(&g_taps[t].kind, memory_order_relaxed);
			int aa = atomic_load_explicit(&g_taps[t].a, memory_order_relaxed);
			int bb = atomic_load_explicit(&g_taps[t].b, memory_order_relaxed);
			int8_t  spec[TAP_BINS_OUT];
			int16_t scope[TAP_SCOPE_N * 2];
			float   rms_dB;
			pthread_mutex_lock(&g_taps[t].out_lock);
			memcpy(spec,  g_taps[t].out_spec,  sizeof(spec));
			memcpy(scope, g_taps[t].out_scope, sizeof(scope));
			rms_dB = g_taps[t].out_rms_dB;
			pthread_mutex_unlock(&g_taps[t].out_lock);
			n += snprintf(reply + n, sizeof(reply) - n,
				      "%s{\"k\":%d,\"a\":%d,\"b\":%d,\"rms\":%.1f,\"s\":[",
				      t ? "," : "", k, aa, bb, rms_dB);
			for (int i = 0; i < TAP_BINS_OUT && n < (int)sizeof(reply); i++)
				n += snprintf(reply + n, sizeof(reply) - n, "%s%d",
					      i ? "," : "", (int)spec[i]);
			n += snprintf(reply + n, sizeof(reply) - n, "],\"x\":[");
			for (int i = 0; i < TAP_SCOPE_N * 2 && n < (int)sizeof(reply); i++)
				n += snprintf(reply + n, sizeof(reply) - n, "%s%d",
					      i ? "," : "", (int)scope[i]);
			n += snprintf(reply + n, sizeof(reply) - n, "]}");
		}
		n += snprintf(reply + n, sizeof(reply) - n, "]}\n");
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "set_tap")) {
		int t, k, a, b;
		if (json_get_int(line, "tap",  &t) < 0 ||
		    json_get_int(line, "kind", &k) < 0 ||
		    t < 0 || t >= N_TAPS) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad set_tap args\"}\n");
			return;
		}
		if (json_get_int(line, "a", &a) < 0) a = 0;
		if (json_get_int(line, "b", &b) < 0) b = -1;
		int amax = 0;
		switch (k) {
		case TAP_KIND_NONE:    amax = 0;             break;
		case TAP_KIND_INPUT:   amax = N_INPUT_TOTAL; break;
		case TAP_KIND_BUS_PRE: amax = N_BUS_FX_CH;   break;
		case TAP_KIND_OUTPUT:  amax = N_OUTPUT_TOTAL;break;
		default:
			dprintf(fd, "{\"ok\":false,\"err\":\"bad kind\"}\n");
			return;
		}
		if (k != TAP_KIND_NONE &&
		    (a < 0 || a >= amax || (b >= 0 && b >= amax))) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad a/b for kind\"}\n");
			return;
		}
		atomic_store_explicit(&g_taps[t].a, a, memory_order_relaxed);
		atomic_store_explicit(&g_taps[t].b, b, memory_order_relaxed);
		atomic_store_explicit(&g_taps[t].kind, k, memory_order_release);
		dprintf(fd, "{\"ok\":true,\"op\":\"set_tap\",\"tap\":%d,\"kind\":%d,"
			    "\"a\":%d,\"b\":%d}\n", t, k, a, b);

	} else if (json_has_op(line, "get_taps")) {
		char reply[256];
		int n = snprintf(reply, sizeof(reply), "{\"ok\":true,\"taps\":[");
		for (int t = 0; t < N_TAPS; t++) {
			n += snprintf(reply + n, sizeof(reply) - n,
				      "%s{\"k\":%d,\"a\":%d,\"b\":%d}",
				      t ? "," : "",
				      atomic_load_explicit(&g_taps[t].kind, memory_order_relaxed),
				      atomic_load_explicit(&g_taps[t].a,    memory_order_relaxed),
				      atomic_load_explicit(&g_taps[t].b,    memory_order_relaxed));
		}
		snprintf(reply + n, sizeof(reply) - n, "]}\n");
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "get_drift")) {
		/* V8.1.b — drift USB↔DSP mesuré passivement par cap_uac2_thread.
		 * V8.2 — shift_ppm = correction adaptative par feedback xrun.
		 * Un seul drift partagé play/cap (même horloge USB host). */
		int x100 = atomic_load(&g_usb_drift_ppm_x100);
		int valid = atomic_load(&g_usb_drift_valid);
		int shift = atomic_load(&g_shift_ppm);
		unsigned long xc = atomic_load(&g_ring_uac2_cap.xruns);
		unsigned long xp = atomic_load(&g_ring_uac2_play.xruns);
		unsigned long dp = atomic_load(&g_ring_uac2_play.drops);
		/* V8.6 — 4 compteurs d'events ring (diag) : full+empty cap+play. */
		unsigned long cfe = atomic_load(&g_ring_uac2_cap.drops_evt);
		unsigned long cee = atomic_load(&g_ring_uac2_cap.empty_evt);
		unsigned long pfe = atomic_load(&g_ring_uac2_play.drops_evt);
		unsigned long pee = atomic_load(&g_ring_uac2_play.empty_evt);
		unsigned long ri = atomic_load(&g_dbg_corr_req_insert);
		unsigned long rd = atomic_load(&g_dbg_corr_req_drop);
		unsigned long ai = atomic_load(&g_dbg_corr_app_insert);
		unsigned long ad = atomic_load(&g_dbg_corr_app_drop);
		/* V8.32 — Stats timing : min/max globaux persistants,
		 * moyenne sur fenêtre glissante de TIMING_WINDOW_SEC buckets. */
		struct timespec n_now;
		clock_gettime(CLOCK_MONOTONIC, &n_now);
		uint64_t now_sec = (uint64_t)n_now.tv_sec;
		uint64_t wr_sum = 0, rd_sum = 0;
		uint32_t wr_n = 0, rd_n = 0;
		for (int k = 0; k < TIMING_WINDOW_SEC; k++) {
			uint64_t e = atomic_load_explicit(&g_wr_bucket_epoch[k], memory_order_relaxed);
			if (e != 0 && now_sec - e < TIMING_WINDOW_SEC) {
				wr_sum += atomic_load_explicit(&g_wr_bucket_sum[k], memory_order_relaxed);
				wr_n   += atomic_load_explicit(&g_wr_bucket_cnt[k], memory_order_relaxed);
			}
			e = atomic_load_explicit(&g_rd_bucket_epoch[k], memory_order_relaxed);
			if (e != 0 && now_sec - e < TIMING_WINDOW_SEC) {
				rd_sum += atomic_load_explicit(&g_rd_bucket_sum[k], memory_order_relaxed);
				rd_n   += atomic_load_explicit(&g_rd_bucket_cnt[k], memory_order_relaxed);
			}
		}
		uint32_t wr_avg = wr_n ? (uint32_t)(wr_sum / wr_n) : 0;
		uint32_t rd_avg = rd_n ? (uint32_t)(rd_sum / rd_n) : 0;
		uint32_t wr_min = atomic_load_explicit(&g_wr_min_us, memory_order_relaxed);
		uint32_t wr_max = atomic_load_explicit(&g_wr_max_us, memory_order_relaxed);
		uint32_t rd_min = atomic_load_explicit(&g_rd_min_us, memory_order_relaxed);
		uint32_t rd_max = atomic_load_explicit(&g_rd_max_us, memory_order_relaxed);
		if (wr_min == UINT32_MAX) wr_min = 0;
		if (rd_min == UINT32_MAX) rd_min = 0;
		unsigned long n1 = atomic_load(&g_dbg_readi_lt10);
		unsigned long n2 = atomic_load(&g_dbg_readi_10_50);
		unsigned long n3 = atomic_load(&g_dbg_readi_50_100);
		unsigned long n4 = atomic_load(&g_dbg_readi_ge100);
		/* V9.1 — wake jitter avg = sum/count (en µs) */
		long wj_sum = atomic_load(&g_wake_jitter_sum_us);
		unsigned long wj_cnt = atomic_load(&g_wake_jitter_count);
		long wj_avg = wj_cnt ? (wj_sum / (long)wj_cnt) : 0;
		long wj_max = atomic_load(&g_wake_jitter_max_us);
		unsigned long it_lt18  = atomic_load(&g_iter_lt18);
		unsigned long it_18_22 = atomic_load(&g_iter_18_22);
		unsigned long it_22_30 = atomic_load(&g_iter_22_30);
		unsigned long it_30_50 = atomic_load(&g_iter_30_50);
		unsigned long it_ge50  = atomic_load(&g_iter_ge50);

		char reply[1200];
		snprintf(reply, sizeof(reply),
		         "{\"ok\":true,\"drift_ppm\":%.2f,\"valid\":%d,"
		         "\"shift_ppm\":%d,"
		         "\"xruns_cap\":%lu,\"xruns_play\":%lu,\"drops_play\":%lu,"
		         "\"cap_full_evt\":%lu,\"cap_empty_evt\":%lu,"
		         "\"play_full_evt\":%lu,\"play_empty_evt\":%lu,"
		         "\"corr_req_insert\":%lu,\"corr_app_insert\":%lu,"
		         "\"corr_req_drop\":%lu,\"corr_app_drop\":%lu,"
		         "\"readi_lt10\":%lu,\"readi_10_50\":%lu,"
		         "\"readi_50_100\":%lu,\"readi_ge100\":%lu,"
		         "\"cc_called\":%lu,\"cc_nonzero\":%lu,\"corr_acc_max\":%d,"
		         "\"uac2_cap_fill\":%u,\"uac2_play_fill\":%u,"
		         "\"uac2_cap_mode\":%d,\"uac2_play_mode\":%d,"
		         "\"uac2_cap_warm\":%d,\"uac2_play_warm\":%d,"
		         "\"wr_us_min\":%u,\"wr_us_max\":%u,\"wr_us_avg\":%u,"
		         "\"rd_us_min\":%u,\"rd_us_max\":%u,\"rd_us_avg\":%u,"
		         "\"wake_jit_max_us\":%ld,\"wake_jit_avg_us\":%ld,"
		         "\"iter_lt18\":%lu,\"iter_18_22\":%lu,"
		         "\"iter_22_30\":%lu,\"iter_30_50\":%lu,\"iter_ge50\":%lu}\n",
		         (double)x100 / 100.0, valid, shift,
		         xc, xp, dp, cfe, cee, pfe, pee,
		         ri, ai, rd, ad, n1, n2, n3, n4,
		         atomic_load(&g_dbg_cc_called),
		         atomic_load(&g_dbg_cc_nonzero),
		         atomic_load(&g_dbg_corr_acc_max),
		         uac2_ring_fill(&g_ring_uac2_cap),
		         uac2_ring_fill(&g_ring_uac2_play),
		         atomic_load(&g_uac2_cap_mode),
		         atomic_load(&g_uac2_play_mode),
		         atomic_load(&g_uac2_cap_warm),
		         atomic_load(&g_uac2_play_warm),
		         wr_min, wr_max, wr_avg,
		         rd_min, rd_max, rd_avg,
		         wj_max, wj_avg,
		         it_lt18, it_18_22, it_22_30, it_30_50, it_ge50);
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "apply_drift_as_shift")) {
		/* V8.14 — force shift_ppm = round(drift_ppm) en un coup,
		 * sans attendre que shift_controller_thread accumule. */
		int x100 = atomic_load(&g_usb_drift_ppm_x100);
		int shift_target = (x100 >= 0) ? (x100 + 50) / 100
		                               : (x100 - 50) / 100;
		atomic_store(&g_shift_ppm, shift_target);
		dprintf(fd,
		        "{\"ok\":true,\"op\":\"apply_drift_as_shift\","
		        "\"shift_ppm\":%d,\"drift_ppm_x100\":%d}\n",
		        shift_target, x100);

	} else if (json_has_op(line, "reset_drift_stats")) {
		/* V8.12 — Reset complet des stats drift/ring : remet à zéro
		 * shift, drift mesuré, et TOUS les compteurs (xruns/drops/events)
		 * sur cap et play. Utile pour repartir d'une base propre après
		 * un démarrage transient, sans redémarrer mixer-pro. */
		atomic_store(&g_shift_ppm, 0);
		atomic_store(&g_usb_drift_ppm_x100, 0);
		atomic_store(&g_usb_drift_valid, 0);
		atomic_store(&g_ring_uac2_cap.xruns,      0);
		atomic_store(&g_ring_uac2_cap.drops,      0);
		atomic_store(&g_ring_uac2_cap.drops_evt,  0);
		atomic_store(&g_ring_uac2_cap.empty_evt,  0);
		atomic_store(&g_ring_uac2_play.xruns,     0);
		atomic_store(&g_ring_uac2_play.drops,     0);
		atomic_store(&g_ring_uac2_play.drops_evt, 0);
		atomic_store(&g_ring_uac2_play.empty_evt, 0);
		/* V8.32 — reset stats timing wr/rd (min/max + buckets) */
		atomic_store(&g_wr_min_us, UINT32_MAX);
		atomic_store(&g_wr_max_us, 0);
		atomic_store(&g_rd_min_us, UINT32_MAX);
		atomic_store(&g_rd_max_us, 0);
		for (int k = 0; k < TIMING_WINDOW_SEC; k++) {
			atomic_store(&g_wr_bucket_sum[k], 0);
			atomic_store(&g_wr_bucket_cnt[k], 0);
			atomic_store(&g_wr_bucket_epoch[k], 0);
			atomic_store(&g_rd_bucket_sum[k], 0);
			atomic_store(&g_rd_bucket_cnt[k], 0);
			atomic_store(&g_rd_bucket_epoch[k], 0);
		}
		/* V9.1 — reset histogram prof_iter + wake_jitter */
		atomic_store(&g_iter_lt18, 0);
		atomic_store(&g_iter_18_22, 0);
		atomic_store(&g_iter_22_30, 0);
		atomic_store(&g_iter_30_50, 0);
		atomic_store(&g_iter_ge50, 0);
		atomic_store(&g_wake_jitter_max_us, 0);
		atomic_store(&g_wake_jitter_sum_us, 0);
		atomic_store(&g_wake_jitter_count, 0);
		dprintf(fd, "{\"ok\":true,\"op\":\"reset_drift_stats\"}\n");

	} else if (json_has_op(line, "reset_fx")) {
		int bus;
		if (json_get_int(line, "bus", &bus) < 0 ||
		    bus < 0 || bus >= N_BUS_FX) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad reset_fx args\"}\n");
			return;
		}
		pthread_mutex_lock(&g_st.target_lock);
		g_st.fx_engines[bus].reset(&g_st.fx_engines[bus]);
		pthread_mutex_unlock(&g_st.target_lock);
		dprintf(fd, "{\"ok\":true,\"op\":\"reset_fx\",\"bus\":%d}\n", bus);

	} else if (json_has_op(line, "reset")) {
		pthread_mutex_lock(&g_st.target_lock);
		memset(g_st.send_target,   0, sizeof(g_st.send_target));
		memset(g_st.master_target, 0, sizeof(g_st.master_target));
		for (int b = 0; b < N_BUS_FX_CH; b++)
			g_st.fx_bus_target[b] = 1.0f;
		for (int i = 0; i < N_INPUT_TOTAL; i++)
			g_st.input_target[i] = 1.0f;
		g_st.mute_mask = 0;
		pthread_mutex_unlock(&g_st.target_lock);
		dprintf(fd, "{\"ok\":true,\"op\":\"reset\"}\n");

	} else {
		dprintf(fd, "{\"ok\":false,\"err\":\"unknown op\"}\n");
	}
}

static void *control_thread(void *arg)
{
	(void)arg;
	/* V9.0 — pin sur cores 0,1 (non-RT, hors des cores isolcpus audio) */
	{
		cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0, &cs); CPU_SET(1, &cs);
		pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
	}
	mlog("control_thread : SCHED_OTHER cores 0,1");
	int srv = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv < 0) { mlog("socket: %s", strerror(errno)); return NULL; }

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, MIXER_SOCK_PATH, sizeof(addr.sun_path) - 1);
	unlink(MIXER_SOCK_PATH);

	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		mlog("bind %s: %s", MIXER_SOCK_PATH, strerror(errno));
		close(srv);
		return NULL;
	}
	chmod(MIXER_SOCK_PATH, 0660);
	if (listen(srv, 8) < 0) {
		mlog("listen: %s", strerror(errno));
		close(srv);
		return NULL;
	}
	mlog("control socket listening on %s", MIXER_SOCK_PATH);

	/* V10-N1.4 — MULTI-CLIENT (poll) : l'ancienne boucle servait UN client
	 * jusqu'à EOF — une connexion persistante (app native mixer-console,
	 * meters 30 Hz) affamait tous les autres (mixer-gui-http = GUI web).
	 * Jusqu'à CTL_MAX_CLIENTS simultanés, buffer d'accumulation PAR client
	 * (V9.4.3 : 16 KB pour les blobs DRC hex ; l'ancien buffer static
	 * unique aurait d'ailleurs été une corruption en multi-client).
	 * handle_cmd (dprintf bloquant) inchangé : clients locaux de confiance,
	 * risque d'un client-qui-ne-lit-pas identique à l'existant. */
#define CTL_MAX_CLIENTS 8
	static struct {
		int fd;
		size_t pos;
		char buf[16384];
	} cl[CTL_MAX_CLIENTS];
	for (int i = 0; i < CTL_MAX_CLIENTS; i++)
		cl[i].fd = -1;

	while (atomic_load(&g_st.running)) {
		struct pollfd pfd[1 + CTL_MAX_CLIENTS];
		int idx_of[1 + CTL_MAX_CLIENTS];
		nfds_t nf = 0;
		pfd[nf].fd = srv;
		pfd[nf].events = POLLIN;
		idx_of[nf++] = -1;
		for (int i = 0; i < CTL_MAX_CLIENTS; i++) {
			if (cl[i].fd < 0)
				continue;
			pfd[nf].fd = cl[i].fd;
			pfd[nf].events = POLLIN;
			idx_of[nf++] = i;
		}

		int pr = poll(pfd, nf, 500);
		if (pr < 0) {
			if (errno == EINTR) continue;
			mlog("poll: %s", strerror(errno));
			break;
		}
		if (pr == 0)
			continue;

		for (nfds_t k = 0; k < nf; k++) {
			if (!(pfd[k].revents & (POLLIN | POLLERR | POLLHUP)))
				continue;

			if (idx_of[k] < 0) {          /* socket serveur : accept */
				int c = accept(srv, NULL, NULL);
				if (c < 0)
					continue;
				int slot = -1;
				for (int i = 0; i < CTL_MAX_CLIENTS; i++)
					if (cl[i].fd < 0) { slot = i; break; }
				if (slot < 0) {
					dprintf(c, "{\"ok\":false,\"err\":\"too many clients\"}\n");
					close(c);
					continue;
				}
				cl[slot].fd = c;
				cl[slot].pos = 0;
				continue;
			}

			int i = idx_of[k];
			ssize_t n = read(cl[i].fd, cl[i].buf + cl[i].pos,
					 sizeof(cl[i].buf) - 1 - cl[i].pos);
			if (n <= 0) {                 /* EOF ou erreur : libère */
				close(cl[i].fd);
				cl[i].fd = -1;
				continue;
			}
			cl[i].pos += (size_t)n;
			cl[i].buf[cl[i].pos] = 0;
			char *line = cl[i].buf, *next;
			while (line && *line) {
				next = strchr(line, '\n');
				if (!next) break;         /* ligne incomplète */
				*next++ = 0;
				if (*line) handle_cmd(cl[i].fd, line);
				line = next;
			}
			if (line && *line) {
				size_t rem = strlen(line);
				memmove(cl[i].buf, line, rem);
				cl[i].pos = rem;
			} else {
				cl[i].pos = 0;
			}
			/* ligne plus longue que le buffer : reset défensif */
			if (cl[i].pos >= sizeof(cl[i].buf) - 1)
				cl[i].pos = 0;
		}
	}

	for (int i = 0; i < CTL_MAX_CLIENTS; i++)
		if (cl[i].fd >= 0)
			close(cl[i].fd);
	close(srv);
	unlink(MIXER_SOCK_PATH);
	return NULL;
}

/* ============================== Persistence presets ================ */

/* V9.3.5 : sauvegarde atomique l'état des 4 bus FX dans
 * /var/lib/mixer-pro/presets.json. Écriture via .tmp + rename pour atomicité.
 * Format :
 *   {"version":1,"buses":[{"bus":0,<get_state output>}, ...]}
 *
 * Appelée par persistence_thread quand g_presets_dirty est settée par
 * set_fx_engine ou set_fx_param. mkdir -p si absent. */
static void save_presets(void)
{
	mkdir("/var/lib/mixer-pro", 0755);
	char tmp_path[256];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", PRESETS_PATH);
	FILE *f = fopen(tmp_path, "w");
	if (!f) {
		fprintf(stderr, "save_presets: fopen %s failed: %s\n",
		        tmp_path, strerror(errno));
		return;
	}
	fprintf(f, "{\"version\":1,\"buses\":[");
	/* Hold lock pour cohérence engine state vs params */
	pthread_mutex_lock(&g_st.target_lock);
	for (int b = 0; b < N_BUS_FX; b++) {
		static char body[16384];
		g_st.fx_engines[b].get_state(&g_st.fx_engines[b], body, sizeof(body));
		fprintf(f, "%s{\"bus\":%d,%s}", b == 0 ? "" : ",", b, body);
	}
	pthread_mutex_unlock(&g_st.target_lock);
	fprintf(f, "]}\n");
	fclose(f);
	/* rename atomique */
	if (rename(tmp_path, PRESETS_PATH) < 0)
		fprintf(stderr, "save_presets: rename failed: %s\n", strerror(errno));
}

/* V9.5.21 — persistance dédiée du remap mic (fichier texte 8 entiers). */
#define MIC_MAP_PATH "/var/lib/mixer-pro/mic_map"
static void save_mic_map(void)
{
	mkdir("/var/lib/mixer-pro", 0755);
	FILE *f = fopen(MIC_MAP_PATH, "w");
	if (!f) return;
	for (int i = 0; i < 8; i++)
		fprintf(f, "%d%s", atomic_load_explicit(&g_mic_map[i],
		        memory_order_relaxed), i < 7 ? " " : "\n");
	fclose(f);
}
static void load_mic_map(void)
{
	FILE *f = fopen(MIC_MAP_PATH, "r");
	if (!f) return;
	int v[8];
	if (fscanf(f, "%d %d %d %d %d %d %d %d",
	           &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7]) == 8) {
		for (int i = 0; i < 8; i++)
			if (v[i] >= 0 && v[i] < 8)
				atomic_store_explicit(&g_mic_map[i], v[i], memory_order_relaxed);
	}
	fclose(f);
}

/* V9.5.21 — persistance des gains de sortie (×1000 milli-linéaire). */
#define OUT_GAIN_PATH "/var/lib/mixer-pro/out_gain"
static void save_out_gain(void)
{
	mkdir("/var/lib/mixer-pro", 0755);
	FILE *f = fopen(OUT_GAIN_PATH, "w");
	if (!f) return;
	for (int o = 0; o < N_OUTPUT_TOTAL; o++)
		fprintf(f, "%d%s", atomic_load_explicit(&g_out_gain_m[o],
		        memory_order_relaxed), o < N_OUTPUT_TOTAL - 1 ? " " : "\n");
	fclose(f);
}
static void load_out_gain(void)
{
	FILE *f = fopen(OUT_GAIN_PATH, "r");
	if (!f) return;
	for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
		int v;
		if (fscanf(f, "%d", &v) != 1) break;
		if (v >= 0 && v <= 4000)
			atomic_store_explicit(&g_out_gain_m[o], v, memory_order_relaxed);
	}
	fclose(f);
}

/* V13.7 — persistance des 7 params de l'EQ master (mastering). Fichier dédié,
 * retro-compatible (absent → défauts smile). */
#define MASTER_EQ_PATH "/var/lib/mixer-pro/master_eq"
static void save_master_eq(void)
{
	mkdir("/var/lib/mixer-pro", 0755);
	FILE *f = fopen(MASTER_EQ_PATH, "w");
	if (!f) return;
	fprintf(f, "%.2f %.1f %.2f %.1f %.3f %.2f %.1f\n",
		g_meq_p.low_db, g_meq_p.low_hz, g_meq_p.mid_db, g_meq_p.mid_hz,
		g_meq_p.mid_q, g_meq_p.air_db, g_meq_p.air_hz);
	fclose(f);
}
static void load_master_eq(void)
{
	FILE *f = fopen(MASTER_EQ_PATH, "r");
	if (!f) return;
	float a, b, c, d, e, g, h;
	if (fscanf(f, "%f %f %f %f %f %f %f", &a, &b, &c, &d, &e, &g, &h) == 7) {
		g_meq_p.low_db = a; g_meq_p.low_hz = b; g_meq_p.mid_db = c;
		g_meq_p.mid_hz = d; g_meq_p.mid_q  = e; g_meq_p.air_db = g;
		g_meq_p.air_hz = h;
	}
	fclose(f);
}


/* V9.5.21b — persistance de l'état COMPLET du mixer (le manque n°1 de la
 * revue : la chaîne insert mastering, le mode assistant et le routage étaient
 * perdus à chaque reboot → re-setup manuel systématique).
 * Fichier texte versionné, écriture atomique (tmp + rename). */
#define MIXER_STATE_PATH "/var/lib/mixer-pro/mixer_state"
/* V13-SCENES : écrit l'état complet vers un chemin arbitraire (état
 * courant OU slot de scène — même format, même parseur au retour). */
static void save_state_to(const char *path)
{
	mkdir("/var/lib/mixer-pro", 0755);
	char tmp_path[256];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	FILE *f = fopen(tmp_path, "w");
	if (!f) return;

	pthread_mutex_lock(&g_st.target_lock);
	fprintf(f, "version 1\n");
	fprintf(f, "insert %d\n", g_insert_spec_n);
	for (int i = 0; i < g_insert_spec_n; i++)
		fprintf(f, "%s %s\n", g_insert_spec_engine[i],
		        g_insert_spec_uri[i][0] ? g_insert_spec_uri[i] : "-");
	fprintf(f, "assistant %d %d\n",
	        atomic_load_explicit(&g_assistant_mode,   memory_order_relaxed),
	        atomic_load_explicit(&g_assistant_source, memory_order_relaxed));
	/* V12-AMX */
	fprintf(f, "automix %d %.1f %.4f\n", g_st.automix_on,
	        g_st.automix_resp_ms, g_st.automix_floor);
	fprintf(f, "automix_members");
	for (int i = 0; i < N_INPUT_REAL; i++)
		fprintf(f, " %d", g_st.automix_member[i]);
	fprintf(f, "\nautomix_weights");
	for (int i = 0; i < N_INPUT_REAL; i++)
		fprintf(f, " %.4f", g_st.automix_weight[i]);
	fprintf(f, "\n");
	fprintf(f, "mute_mask %u\n", g_st.mute_mask);
	fprintf(f, "input_gains");
	for (int i = 0; i < N_INPUT_TOTAL; i++)
		fprintf(f, " %.4f", g_st.input_target[i]);
	fprintf(f, "\nfx_bus");
	for (int b = 0; b < N_BUS_FX_CH; b++)
		fprintf(f, " %.4f", g_st.fx_bus_target[b]);
	fprintf(f, "\nmaster\n");
	for (int s = 0; s < N_INPUT_TOTAL; s++) {
		for (int o = 0; o < N_OUTPUT_TOTAL; o++)
			fprintf(f, "%.4f%s", g_st.master_target[s][o],
			        o < N_OUTPUT_TOTAL - 1 ? " " : "\n");
	}
	/* V12-EXP (fin de fichier — absent des états antérieurs) */
	for (int i = 0; i < N_EXP_CH; i++)
		fprintf(f, "expander %d %d %.1f %.1f %.1f %.0f %.0f %.0f\n",
			i, g_exp[i].on, g_exp[i].thr_db, g_exp[i].ratio,
			g_exp[i].atk_ms, g_exp[i].rel_ms, g_exp[i].range_db,
			g_exp[i].hold_ms);
	/* V13-COMP + BANDMIX (littéraux avec espace de tête au load) */
	for (int i = 0; i < N_EXP_CH; i++)
		fprintf(f, "comp %d %d %.1f %.1f %.1f %.0f %.1f\n",
			i, g_cmp[i].on, g_cmp[i].thr_db, g_cmp[i].ratio,
			g_cmp[i].atk_ms, g_cmp[i].rel_ms, g_cmp[i].makeup_db);
	for (int i = 0; i < N_EXP_CH; i++)
		fprintf(f, "bandmix %d %d %.6e\n", i, g_bmx.role[i],
			g_bmx.ref_valid ? g_bmx.ref_share[i] : 0.0f);
	fprintf(f, "bandmix_live %d %d %d\n", g_bmx.live, g_bmx.ref_valid,
		g_bmx.autolive);   /* V13.5 : 3e champ autolive (rétro-compat) */
	fprintf(f, "vfocus %d %.0f %.1f\n", g_vf.on,
		g_vf.amount * 100.0f, g_vf.max_cut_db);
	/* V13.9 (revue F15 + fiabilisation n°4) : persistance des réglages
	 * V13.7-V13.9 — EQ master, tunables automix, balance, spatializer,
	 * solo auto. Lignes ignorées par les anciens loaders (rétro-compat). */
	fprintf(f, "master_eq %.1f %.1f %.1f %.1f %.2f %.1f %.1f\n",
		g_meq_p.low_hz, g_meq_p.low_db, g_meq_p.mid_hz, g_meq_p.mid_db,
		g_meq_p.mid_q, g_meq_p.air_hz, g_meq_p.air_db);
	fprintf(f, "automix_tune %.1f %.3f %.1f %.1f\n", g_bmx.freeze_db,
		g_bmx.risk_decay, g_bmx.risk_margin, g_bmx.gate_db);
	fprintf(f, "balance %d %.1f %.1f %.1f\n", g_bmx.balance_on,
		g_bmx.bal_lufs_tgt, g_bmx.bal_e_tgt, g_bmx.bal_c_tgt);
	fprintf(f, "vspatial %d %d %d\n",
		(int)atomic_load(&g_vspat.on),
		(int)atomic_load(&g_vspat.amount_mq),
		(int)atomic_load(&g_vspat.delay_smp));
	fprintf(f, "solo_auto %d\n", g_bmx.solo_auto);
	/* V13.1 : matrice des sends par tranche (départs FX) — trouvé absent
	 * par la campagne de validation. En FIN de fichier, une ligne par
	 * tranche, parsé par la boucle fgets des loaders (états antérieurs
	 * sans ces lignes = compatibles). */
	for (int s = 0; s < N_INPUT_TOTAL; s++) {
		fprintf(f, "sends %d", s);
		for (int b = 0; b < N_BUS_FX_CH; b++)
			fprintf(f, " %.4f", g_st.send_target[s][b]);
		fprintf(f, "\n");
	}
	/* V13.3 : liens stéréo (8 paires) */
	fprintf(f, "links");
	for (int i = 0; i < N_LINK_PAIRS; i++)
		fprintf(f, " %d", atomic_load(&g_link[i]));
	fprintf(f, "\n");
	pthread_mutex_unlock(&g_st.target_lock);

	fclose(f);
	rename(tmp_path, path);
}

static void save_mixer_state(void)
{
	save_state_to(MIXER_STATE_PATH);
}

/* ============ V13-SCENES — rappel de profil sans coupure audio ============
 * Même format que mixer_state (GARDER EN PHASE avec load_mixer_state).
 * Le fichier est lu EN MÉMOIRE puis parsé via fmemopen : aucune I/O
 * disque sous target_lock. L'insert chain (LV2, lourde) est ré-initiée
 * HORS lock puis swappée (pattern set_insert) seulement si le spec
 * diffère du courant. Les gains atterrissent dans les TARGETS → les
 * valeurs réelles glissent via smooth_gains (aucun clic). */
/* ===== Parseur COMMUN des lignes d'état (revue code 2026-07-28, lot 5b) =====
 * Boucle fgets/sscanf partagée par load_mixer_state (boot) et scene_apply
 * (rappel live) — était dupliquée à l'identique dans les deux (77 lignes),
 * chaque évolution devait être faite 2 fois (source de divergence).
 * APPELANT responsable du verrouillage (les deux appellent sous target_lock).
 * ATTENTION ordre des tests sscanf : les littéraux mangent les PRÉFIXES des
 * mots-clés voisins ("bandmix" avale le début de "bandmix_live") et
 * désynchronisent le flux. Tester le mot-clé long AVANT le court. */
static void parse_state_lines(FILE *f)
{
	char bl[160];
	int src, on, role, live, rv, al = 0;
	float thr, ratio, atk, rel, mk, shr, sv[8];
	int lk[8];
	while (fgets(bl, sizeof(bl), f)) {
		if (sscanf(bl, "comp %d %d %f %f %f %f %f",
			   &src, &on, &thr, &ratio, &atk, &rel,
			   &mk) == 7)
			cmp_configure(src, on, thr, ratio, atk, rel, mk);
		else if (sscanf(bl, "bandmix_live %d %d %d",
				&live, &rv, &al) >= 2) {
			g_bmx.ref_valid = rv ? 1 : 0;
			g_bmx.live = (live && rv) ? 1 : 0;
			g_bmx.autolive = al ? 1 : 0;   /* V13.5 */
		} else if (sscanf(bl, "bandmix %d %d %f",
				  &src, &role, &shr) == 3 &&
			   src >= 0 && src < N_EXP_CH &&
			   role >= 0 && role < BR_NROLES) {
			g_bmx.role[src] = role;
			g_bmx.ref_share[src] = shr;
		} else if (sscanf(bl, "vfocus %d %f %f",
				  &on, &atk, &rel) == 3) {
			g_vf.on = on ? 1 : 0;
			if (atk >= 0.0f && atk <= 100.0f)
				g_vf.amount = atk / 100.0f;
			if (rel >= 0.0f && rel <= 12.0f)
				g_vf.max_cut_db = rel;
		/* V13.9 (revue F15 + fiabilisation n°4) : restauration des
		 * réglages V13.7-V13.9 — mêmes plages de validation que les
		 * ops live ; meq_recalc = bascule crossfadée sans clic. */
		} else if (sscanf(bl, "master_eq %f %f %f %f %f %f %f",
				  &sv[0], &sv[1], &sv[2], &sv[3],
				  &sv[4], &sv[5], &sv[6]) == 7) {
			g_meq_p.low_hz = sv[0]; g_meq_p.low_db = sv[1];
			g_meq_p.mid_hz = sv[2]; g_meq_p.mid_db = sv[3];
			g_meq_p.mid_q  = sv[4];
			g_meq_p.air_hz = sv[5]; g_meq_p.air_db = sv[6];
			meq_recalc();
			save_master_eq();   /* fichier dédié cohérent */
		} else if (sscanf(bl, "automix_tune %f %f %f %f",
				  &sv[0], &sv[1], &sv[2], &sv[3]) == 4) {
			if (sv[0] >= 3.0f  && sv[0] <= 40.0f) g_bmx.freeze_db   = sv[0];
			if (sv[1] >= 0.0f  && sv[1] <= 2.0f)  g_bmx.risk_decay  = sv[1];
			if (sv[2] >= 0.0f  && sv[2] <= 12.0f) g_bmx.risk_margin = sv[2];
			if (sv[3] >= 3.0f  && sv[3] <= 30.0f) g_bmx.gate_db     = sv[3];
		} else if (sscanf(bl, "balance %d %f %f %f",
				  &on, &sv[0], &sv[1], &sv[2]) == 4) {
			g_bmx.balance_on = on ? 1 : 0;
			if (sv[0] >= -30.0f && sv[0] <= -6.0f) g_bmx.bal_lufs_tgt = sv[0];
			if (sv[1] >= -6.0f  && sv[1] <= 12.0f) g_bmx.bal_e_tgt   = sv[1];
			if (sv[2] >= -6.0f  && sv[2] <= 12.0f) g_bmx.bal_c_tgt   = sv[2];
		} else if (sscanf(bl, "vspatial %d %d %d",
				  &on, &src, &role) == 3) {
			atomic_store(&g_vspat.on, on ? 1 : 0);
			if (src >= 0 && src <= 1000)
				atomic_store(&g_vspat.amount_mq, src);
			if (role >= 144 && role <= 1920)   /* 3..40 ms @48k */
				atomic_store(&g_vspat.delay_smp, role);
		} else if (sscanf(bl, "solo_auto %d", &on) == 1) {
			g_bmx.solo_auto = on ? 1 : 0;
		} else if (sscanf(bl, "sends %d %f %f %f %f %f %f %f %f",
				  &src, &sv[0], &sv[1], &sv[2], &sv[3],
				  &sv[4], &sv[5], &sv[6], &sv[7]) == 9 &&
			   src >= 0 && src < N_INPUT_TOTAL) {
			/* V13.1 : départs FX par tranche */
			for (int b = 0; b < N_BUS_FX_CH && b < 8; b++)
				if (sv[b] >= 0.0f && sv[b] <= 8.0f)
					g_st.send_target[src][b] = sv[b];
		} else if (sscanf(bl, "links %d %d %d %d %d %d %d %d",
				  &lk[0], &lk[1], &lk[2], &lk[3],
				  &lk[4], &lk[5], &lk[6], &lk[7]) == 8) {
			/* V13.3 : liens stéréo */
			for (int i = 0; i < N_LINK_PAIRS; i++)
				atomic_store(&g_link[i], lk[i] ? 1 : 0);
		}
	}
}

static int scene_apply(const char *path)
{
	FILE *df = fopen(path, "r");
	if (!df)
		return -1;
	char *buf = malloc(65536);
	if (!buf) { fclose(df); return -1; }
	size_t bn = fread(buf, 1, 65535, df);
	fclose(df);
	buf[bn] = '\0';
	FILE *f = fmemopen(buf, bn, "r");
	if (!f) { free(buf); return -1; }

	int ver = 0;
	if (fscanf(f, "version %d\n", &ver) != 1 || ver != 1) {
		fclose(f); free(buf);
		return -1;
	}
	/* --- insert spec → local (application différée hors lock) --- */
	static char eng[FX_CHAIN_MAX][32], uri[FX_CHAIN_MAX][256];
	int n_ins = 0, n_decl = 0;
	if (fscanf(f, "insert %d\n", &n_decl) == 1 &&
	    n_decl > 0 && n_decl <= FX_CHAIN_MAX) {
		int ok = 1;
		for (int i = 0; i < n_decl; i++) {
			if (fscanf(f, "%31s %255s\n", eng[i], uri[i]) != 2)
				{ ok = 0; break; }
			if (!strcmp(uri[i], "-"))
				uri[i][0] = '\0';
		}
		if (ok) n_ins = n_decl;
	}
	int am = 0, as = 0;
	if (fscanf(f, "assistant %d %d\n", &am, &as) == 2) {
		atomic_store_explicit(&g_assistant_mode,   am ? 1 : 0, memory_order_relaxed);
		atomic_store_explicit(&g_assistant_source, as ? 1 : 0, memory_order_relaxed);
	}

	/* --- le reste sous lock (parse depuis la MÉMOIRE, pas le disque) --- */
	pthread_mutex_lock(&g_st.target_lock);
	{
		int aon;
		float aresp, afloor;
		if (fscanf(f, " automix %d %f %f\n", &aon, &aresp, &afloor) == 3) {
			g_st.automix_on = aon ? 1 : 0;
			if (aresp >= 10.0f && aresp <= 2000.0f)
				g_st.automix_resp_ms = aresp;
			if (afloor > 0.0f && afloor <= 1.0f)
				g_st.automix_floor = afloor;
			if (fscanf(f, " automix_members") == 0)
				for (int i = 0; i < N_INPUT_REAL; i++) {
					int v;
					if (fscanf(f, "%d", &v) != 1) break;
					g_st.automix_member[i] = v ? 1 : 0;
				}
			if (fscanf(f, " automix_weights") == 0)
				for (int i = 0; i < N_INPUT_REAL; i++) {
					float v;
					if (fscanf(f, "%f", &v) != 1) break;
					if (v >= 0.01f && v <= 100.0f)
						g_st.automix_weight[i] = v;
				}
		}
	}
	{
		unsigned mm = 0;
		if (fscanf(f, " mute_mask %u\n", &mm) == 1)
			g_st.mute_mask = mm;
	}
	if (fscanf(f, " input_gains") == 0)
		for (int i = 0; i < N_INPUT_TOTAL; i++) {
			float v;
			if (fscanf(f, "%f", &v) != 1) break;
			if (v >= 0.0f && v <= 8.0f) g_st.input_target[i] = v;
		}
	if (fscanf(f, " fx_bus") == 0)
		for (int b = 0; b < N_BUS_FX_CH; b++) {
			float v;
			if (fscanf(f, "%f", &v) != 1) break;
			if (v >= 0.0f && v <= 8.0f) g_st.fx_bus_target[b] = v;
		}
	if (fscanf(f, " master") == 0)
		for (int s = 0; s < N_INPUT_TOTAL; s++)
			for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
				float v;
				if (fscanf(f, "%f", &v) != 1) goto tail;
				if (v >= 0.0f && v <= 8.0f)
					g_st.master_target[s][o] = v;
			}
tail:
	{
		int src, on;
		float thr, ratio, atk, rel, rng, hold;
		while (fscanf(f, " expander %d %d %f %f %f %f %f %f\n",
			      &src, &on, &thr, &ratio, &atk, &rel,
			      &rng, &hold) == 8)
			exp_configure(src, on, thr, ratio, atk, rel, rng, hold);
	}
	parse_state_lines(f);   /* boucle commune boot+scène */
	pthread_mutex_unlock(&g_st.target_lock);
	fclose(f);
	free(buf);

	/* --- insert chain : ré-init seulement si le spec diffère --- */
	int same = (n_ins == g_insert_spec_n);
	for (int i = 0; same && i < n_ins; i++)
		same = !strcmp(eng[i], g_insert_spec_engine[i]) &&
		       !strcmp(uri[i], g_insert_spec_uri[i]);
	if (!same) {
		if (n_ins == 0) {
			pthread_mutex_lock(&g_st.target_lock);
			int was = atomic_exchange(&g_insert_active, 0);
			g_insert_spec_n = 0;
			pthread_mutex_unlock(&g_st.target_lock);
			if (was) fx_free(&g_insert_chain);
		} else {
			struct fx_chain_spec specs[FX_CHAIN_MAX];
			for (int i = 0; i < n_ins; i++) {
				specs[i].engine = eng[i];
				specs[i].uri    = uri[i];
			}
			fx_engine_t chain = { 0 };
			if (fx_init_chain(&chain, (float)SAMPLE_RATE,
					  specs, n_ins)) {
				pthread_mutex_lock(&g_st.target_lock);
				fx_engine_t old = g_insert_chain;
				int was = atomic_load(&g_insert_active);
				for (int i = 0; i < n_ins; i++) {
					snprintf(g_insert_spec_engine[i], 32,
						 "%s", eng[i]);
					snprintf(g_insert_spec_uri[i], 256,
						 "%s", uri[i]);
				}
				g_insert_spec_n = n_ins;
				g_insert_chain = chain;
				atomic_store(&g_insert_active, 1);
				pthread_mutex_unlock(&g_st.target_lock);
				if (was) fx_free(&old);
			} else
				mlog("scene: insert chain init FAILED (spec gardé)");
		}
	}
	atomic_store(&g_presets_dirty, 1);   /* la scène devient l'état courant */
	mlog("scene: profil appliqué (%s)", path);
	return 0;
}

/* Appelée dans main() AVANT le démarrage des threads (pas de lock requis,
 * fx_init_chain initialise le monde lilv à la demande). */
static void load_mixer_state(void)
{
	FILE *f = fopen(MIXER_STATE_PATH, "r");
	if (!f) return;
	int ver = 0;
	if (fscanf(f, "version %d\n", &ver) != 1 || ver != 1) {
		fclose(f);
		return;
	}
	int n_ins = 0;
	if (fscanf(f, "insert %d\n", &n_ins) == 1 &&
	    n_ins > 0 && n_ins <= FX_CHAIN_MAX) {
		struct fx_chain_spec specs[FX_CHAIN_MAX];
		int ok = 1;
		for (int i = 0; i < n_ins; i++) {
			if (fscanf(f, "%31s %255s\n", g_insert_spec_engine[i],
			           g_insert_spec_uri[i]) != 2) { ok = 0; break; }
			if (!strcmp(g_insert_spec_uri[i], "-"))
				g_insert_spec_uri[i][0] = '\0';
			specs[i].engine = g_insert_spec_engine[i];
			specs[i].uri    = g_insert_spec_uri[i];
		}
		if (ok) {
			fx_engine_t chain = {0};
			if (fx_init_chain(&chain, (float)SAMPLE_RATE, specs, n_ins)) {
				g_insert_chain = chain;
				atomic_store(&g_insert_active, 1);
				g_insert_spec_n = n_ins;
				mlog("state: insert chain restaurée (%d plugins)", n_ins);
			} else {
				mlog("state: insert chain restore FAILED (plugins absents ?)");
				g_insert_spec_n = 0;
			}
		}
	}
	int am = 0, as = 0;
	if (fscanf(f, "assistant %d %d\n", &am, &as) == 2) {
		atomic_store_explicit(&g_assistant_mode,   am ? 1 : 0, memory_order_relaxed);
		atomic_store_explicit(&g_assistant_source, as ? 1 : 0, memory_order_relaxed);
	}
	/* V12-AMX (optionnel — absent des états antérieurs) */
	{
		int aon;
		float aresp, afloor;
		if (fscanf(f, " automix %d %f %f\n", &aon, &aresp, &afloor) == 3) {
			g_st.automix_on = aon ? 1 : 0;
			if (aresp >= 10.0f && aresp <= 2000.0f)
				g_st.automix_resp_ms = aresp;
			if (afloor > 0.0f && afloor <= 1.0f)
				g_st.automix_floor = afloor;
			if (fscanf(f, " automix_members") == 0)
				for (int i = 0; i < N_INPUT_REAL; i++) {
					int v;
					if (fscanf(f, "%d", &v) != 1) break;
					g_st.automix_member[i] = v ? 1 : 0;
				}
			if (fscanf(f, " automix_weights") == 0)
				for (int i = 0; i < N_INPUT_REAL; i++) {
					float v;
					if (fscanf(f, "%f", &v) != 1) break;
					if (v >= 0.01f && v <= 100.0f)
						g_st.automix_weight[i] = v;
				}
		}
	}
	unsigned mm = 0;
	/* Espace de tête OBLIGATOIRE : la boucle automix_weights ci-dessus lit
	 * exactement N_INPUT_REAL floats et laisse le '\n' non consommé — un
	 * littéral sans skip d'espace échoue alors sans rien consommer et
	 * désynchronise TOUT le reste du parse (faders/mute/routing/expander
	 * perdus au boot — régression V12-AMX corrigée ici). */
	if (fscanf(f, " mute_mask %u\n", &mm) == 1)
		g_st.mute_mask = mm;
	if (fscanf(f, " input_gains") == 0) {
		for (int i = 0; i < N_INPUT_TOTAL; i++) {
			float v;
			if (fscanf(f, "%f", &v) != 1) break;
			if (v >= 0.0f && v <= 8.0f) g_st.input_target[i] = v;
		}
	}
	if (fscanf(f, " fx_bus") == 0) {
		for (int b = 0; b < N_BUS_FX_CH; b++) {
			float v;
			if (fscanf(f, "%f", &v) != 1) break;
			if (v >= 0.0f && v <= 8.0f) g_st.fx_bus_target[b] = v;
		}
	}
	if (fscanf(f, " master") == 0) {
		for (int s = 0; s < N_INPUT_TOTAL; s++)
			for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
				float v;
				if (fscanf(f, "%f", &v) != 1) goto done;
				if (v >= 0.0f && v <= 8.0f) g_st.master_target[s][o] = v;
			}
	}
	/* V12-EXP (optionnel) — exp_configure re-précalcule et clampe */
	{
		int src, on;
		float thr, ratio, atk, rel, rng, hold;
		while (fscanf(f, " expander %d %d %f %f %f %f %f %f\n",
			      &src, &on, &thr, &ratio, &atk, &rel,
			      &rng, &hold) == 8)
			exp_configure(src, on, thr, ratio, atk, rel, rng, hold);
	}
	parse_state_lines(f);   /* boucle commune boot+scène */
done:
	fclose(f);
	mlog("state: mixer_state restauré (assistant=%d/%d mute=0x%x)", am, as, mm);
}

static void *persistence_thread(void *arg)
{
	(void)arg;
	while (atomic_load(&g_st.running)) {
		sleep(1);
		midix_try_map();   /* V12-MIDIX : mmap hors RT, retry 1 Hz */
		bmx_tick();        /* V13-BANDMIX : soundcheck + keeper 1 Hz */
		if (atomic_exchange(&g_presets_dirty, 0)) {
			save_presets();
			save_mic_map();      /* V9.5.21 */
			save_out_gain();     /* V9.5.21 */
			save_mixer_state();  /* V9.5.21b */
		}
	}
	/* Final save au shutdown si dirty */
	if (atomic_load(&g_presets_dirty)) {
		save_presets();
		save_mic_map();
		save_out_gain();
		save_mixer_state();
	}
	return NULL;
}

/* ============================== Signal handling ==================== */

static void on_signal(int sig)
{
	(void)sig;
	atomic_store(&g_st.running, 0);
	/* E6.h : débloquer play_thread qui peut être en read(eventfd) bloquant */
	if (g_st.ring_event_fd >= 0) {
		uint64_t one = 1;
		(void)write(g_st.ring_event_fd, &one, sizeof(one));
	}
}

/* ============================== Main =============================== */

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--no-uac2"))  g_skip_uac2  = 1;
		else if (!strcmp(argv[i], "--no-phone")) g_skip_phone = 1;
		else if (!strcmp(argv[i], "--no-asrc"))  atomic_store(&g_no_asrc, 1);
		else if (!strcmp(argv[i], "--fixed-shift") && i+1 < argc) {
			int v = atoi(argv[++i]);
			atomic_store(&g_shift_ppm, v);
			atomic_store(&g_shift_fixed, 1);
		}
		else if (!strcmp(argv[i], "--dump-usb-cap") && i+1 < argc) {
			g_usb_cap_dump = fopen(argv[++i], "wb");
			if (g_usb_cap_dump)
				setvbuf(g_usb_cap_dump, NULL, _IOFBF, 1024*1024);
			else perror("dump-usb-cap fopen");
		}
		else if (!strcmp(argv[i], "--dump-dsp-play") && i+1 < argc) {
			g_dsp_play_dump = fopen(argv[++i], "wb");
			if (g_dsp_play_dump)
				setvbuf(g_dsp_play_dump, NULL, _IOFBF, 1024*1024);
			else perror("dump-dsp-play fopen");
		}
		else if (!strcmp(argv[i], "--help")) {
			fprintf(stderr,
				"usage: %s [--no-uac2] [--no-phone] [--no-asrc] [--dump-usb-cap PATH]\n"
				"  --no-uac2       : skip UAC2Gadget PCMs (host PC absent)\n"
				"  --no-phone      : skip Phone aloop PCMs\n"
				"  --no-asrc       : disable ASRC (compute_correction returns 0)\n"
				"  --dump-usb-cap  : dump raw S32_LE 8ch frames popped from ring_uac2_cap\n",
				argv[0]);
			return 0;
		}
	}

	mlog("mixer-pro " MIXER_VERSION " starting (skip_uac2=%d skip_phone=%d)",
	     g_skip_uac2, g_skip_phone);

	for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
		atomic_store(&g_out_gain_m[o], 1000);   /* gain sortie ×1.0 par défaut */
		g_out_gain_cur[o] = 1.0f;
	}
	load_mic_map();      /* V9.5.21 — restaure le remap mic persisté */
	load_out_gain();     /* V9.5.21 — restaure les gains de sortie persistés */
	for (int o = 0; o < N_OUTPUT_TOTAL; o++)   /* pas de rampe au boot */
		g_out_gain_cur[o] = atomic_load(&g_out_gain_m[o]) * 0.001f;
	load_master_eq();    /* V13.7 — restaure l'EQ master persistée */
	meq_init();          /* précalcule les biquads (banque active, pas de fondu) */

	/* Reset matrices = identity (all 0, then fx_bus_target = 1.0) */
	memset(&g_st.send_gain,     0, sizeof(g_st.send_gain));
	memset(&g_st.send_target,   0, sizeof(g_st.send_target));
	memset(&g_st.master_gain,   0, sizeof(g_st.master_gain));
	memset(&g_st.master_target, 0, sizeof(g_st.master_target));
	for (int b = 0; b < N_BUS_FX_CH; b++) {
		g_st.fx_bus_gain[b]   = 1.0f;
		g_st.fx_bus_target[b] = 1.0f;
	}
	/* E7.2 : strip gain = unity gain par défaut (1.0 = 0 dB), 26 inputs */
	for (int i = 0; i < N_INPUT_TOTAL; i++) {
		g_st.input_gain[i]   = 1.0f;
		g_st.input_target[i] = 1.0f;
	}
	g_st.mute_mask = 0;
	/* V12-AMX : défauts — off, gains unité, poids 1, resp 100 ms,
	 * plancher −15 dB (part de gain minimale d'un membre) */
	g_st.automix_on = 0;
	g_st.automix_resp_ms = 100.0f;
	g_st.automix_floor = 0.1778f;
	for (int i = 0; i < N_INPUT_TOTAL; i++) {
		g_st.automix_member[i] = 0;
		g_st.automix_weight[i] = 1.0f;
		g_st.automix_env[i] = 0.0f;
		g_st.automix_gain[i] = 1.0f;
		g_st.automix_gtarget[i] = 1.0f;
		g_st.keeper_gain[i] = 1.0f;     /* V13-BANDMIX */
		g_st.keeper_target[i] = 1.0f;
		g_st.presence_gain[i] = 1.0f;   /* V13.9 balance auto */
		g_st.presence_target[i] = 1.0f;
	}
	for (int i = 0; i < N_EXP_CH; i++)
		g_bmx.solo_base[i] = -999.0f;   /* V13.9 solo v2 : base à apprendre */
	/* V12-EXP : défauts gates (off) — avant load_mixer_state qui écrase */
	for (int i = 0; i < N_EXP_CH; i++)
		exp_configure(i, 0, -50.0f, 3.0f, 5.0f, 150.0f, 40.0f, 50.0f);
	/* V13-COMP : défauts compresseurs (off) */
	for (int i = 0; i < N_EXP_CH; i++)
		cmp_configure(i, 0, -18.0f, 3.0f, 15.0f, 150.0f, 0.0f);
	/* V13-VFOCUS : précalcul des bandes (fréquences fixes) */
	vf_init();
	pthread_mutex_init(&g_st.target_lock, NULL);
	atomic_store(&g_st.running, 1);

	/* V9.5.21b — restaure l'état complet (insert + assistant + routage)
	 * APRÈS l'init des défauts g_st (sinon écrasé), AVANT les threads. */
	load_mixer_state();

	/* V13.6/13.7 — si AUTOMIX LIVE persisté actif, réarme les étages auto
	 * que le loader ne rallume pas (EQ placement + EQ master + makeup). */
	if (g_bmx.autolive) {
		for (int i = 0; i < N_EXP_CH; i++)
			g_eqx.role_of[i] = -1;
		atomic_store(&g_eqx.on, 1);
		atomic_store(&g_master_on, 1);
	}

	/* V12-SMP : charge la banque de samples (avant threads, pas de lock) */
	mkdir("/var/lib/ala", 0755);
	mkdir(SMP_DIR, 0755);
	smp_scan(0);

	/* V12-LOOP-PRO : buffers loopstation — LOOP_TRACKS × 40 s stéréo
	 * (6 × 14,6 Mo = 88 Mo), alloués une fois, jamais libérés. Défaut :
	 * piste t → voie mic t+1, gain 0 dB, mono (src_b=-1 → dup L/R). */
	for (int t = 0; t < LOOP_TRACKS; t++) {
		g_tr[t].buf   = calloc((size_t)LOOP_MAX_FRAMES * 2, sizeof(float));
		g_tr[t].src_a = t < N_INPUT_MICS ? t : 0;
		g_tr[t].src_b = -1;
		g_tr[t].gain  = 1.0f;
		atomic_store(&g_tr[t].rec_start, REC_START_NONE);
		if (!g_tr[t].buf)
			mlog("loop: alloc piste %d ÉCHEC — looper dégradé", t);
	}

	/* E6.h : eventfd pour signaler le play_thread depuis l'audio_thread.
	 * EFD_SEMAPHORE-like accumule les writes ; on lit en bloc.
	 */
	g_st.ring_event_fd = eventfd(0, EFD_CLOEXEC);
	if (g_st.ring_event_fd < 0) {
		mlog("ERROR: eventfd failed: %s", strerror(errno));
		return 1;
	}

	/* Init FX engines : 0=compressor, 1=reverb, 2=delay, 3=eq */
	if (!fx_init_compressor(&g_st.fx_engines[0], (float)SAMPLE_RATE) ||
	    !fx_init_reverb    (&g_st.fx_engines[1], (float)SAMPLE_RATE) ||
	    !fx_init_delay     (&g_st.fx_engines[2], (float)SAMPLE_RATE) ||
	    !fx_init_eq        (&g_st.fx_engines[3], (float)SAMPLE_RATE)) {
		mlog("ERROR: fx_init failed");
		return 1;
	}
	mlog("FX engines : 0=compressor 1=reverb 2=delay 3=eq");

	/* E7.5 : init analyzer taps storage + raise the run flag before
	 * starting the analyzer thread (which polls it). */
	analyzer_taps_init(g_taps);
	atomic_store(&g_running_flag_for_analyzer, 1);

	/* Open ALSA streams (skip selon flags command-line).
	 * V8.0-E1 : UAC2 + Phone graceful-degrade. Si le PCM n'existe pas
	 * (gadget pas bindé, câble USB absent au boot, recipe désactivé),
	 * on log un warning et on bascule en mode skip — le DSP reste
	 * opérationnel seul. DSP capture/playback restent fatal (board
	 * inutilisable sans). */
	if (pcm_open(&g_st.cap_dsp,   PCM_DSP_CAP,   N_INPUT_MICS,   SND_PCM_STREAM_CAPTURE)  < 0) goto err;
	if (pcm_open(&g_st.play_dsp,  PCM_DSP_PLAY,  N_OUTPUT_DSP,   SND_PCM_STREAM_PLAYBACK) < 0) goto err;
	if (!g_skip_uac2) {
		if (pcm_open(&g_st.cap_uac2,  PCM_UAC2_CAP,  N_INPUT_STEMS,  SND_PCM_STREAM_CAPTURE)  < 0 ||
		    pcm_open(&g_st.play_uac2, PCM_UAC2_PLAY, N_OUTPUT_UAC2,  SND_PCM_STREAM_PLAYBACK) < 0) {
			mlog("UAC2Gadget PCM unavailable (gadget not bound or USB unplugged) — running without UAC2");
			g_skip_uac2 = 1;
		}
	}
	if (!g_skip_phone) {
		if (pcm_open(&g_st.cap_phone, PCM_PHONE_CAP, N_INPUT_PHONE,  SND_PCM_STREAM_CAPTURE)  < 0 ||
		    pcm_open(&g_st.play_phone,PCM_PHONE_PLAY,N_OUTPUT_PHONE, SND_PCM_STREAM_PLAYBACK) < 0) {
			mlog("Phone aloop PCM unavailable — running without Phone");
			g_skip_phone = 1;
		}
	}

	/* Lock memory for RT */
	mlockall(MCL_CURRENT | MCL_FUTURE);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	pthread_t th_audio, th_ctrl, th_play, th_analyzer;
	pthread_t th_cap_uac2, th_play_uac2, th_shift_ctl, th_persist;
	pthread_create(&th_ctrl, NULL, control_thread, NULL);
	pthread_create(&th_play, NULL, play_thread, NULL);   /* E6.g Phase 2 */
	pthread_create(&th_audio, NULL, audio_thread, NULL);
	pthread_create(&th_analyzer, NULL, analyzer_thread, NULL);  /* E7.5 */
	/* V9.3.5 : thread persistence presets (debounced 1s) */
	pthread_create(&th_persist, NULL, persistence_thread, NULL);
	/* V9.5.12 : SHM tap USB IN pour daemon mixer-ml-inference (process séparé).
	 * Crée /dev/shm/mixer-pro-tap-usb. audio_thread y écrit en continu. */
	extern int mixer_pro_shm_tap_init(void);
	mixer_pro_shm_tap_init();
	/* V8.1 : threads UAC2 dédiés (isolation USB ↔ DSP) */
	if (!g_skip_uac2) {
		pthread_create(&th_cap_uac2,  NULL, cap_uac2_thread,  NULL);
		pthread_create(&th_play_uac2, NULL, play_uac2_thread, NULL);
	}
	/* V8.26 — shift_controller_thread DÉSACTIVÉ : shift_ppm est piloté par
	 * cap_uac2_thread via drift précis (HW htstamp). */
	(void)th_shift_ctl;
	/* pthread_create(&th_shift_ctl, NULL, shift_controller_thread, NULL); */

	pthread_join(th_audio, NULL);
	pthread_join(th_play, NULL);
	pthread_join(th_ctrl, NULL);
	pthread_join(th_persist, NULL);  /* V9.3.5 : final save dans le thread */
	/* V8.26 — shift_controller_thread désactivé (cf création) */
	if (!g_skip_uac2) {
		pthread_join(th_cap_uac2, NULL);
		pthread_join(th_play_uac2, NULL);
	}
	atomic_store(&g_running_flag_for_analyzer, 0);
	pthread_join(th_analyzer, NULL);
	analyzer_taps_destroy(g_taps);

	snd_pcm_close(g_st.cap_dsp.pcm);
	if (!g_skip_uac2)  snd_pcm_close(g_st.cap_uac2.pcm);
	if (!g_skip_phone) snd_pcm_close(g_st.cap_phone.pcm);
	snd_pcm_close(g_st.play_dsp.pcm);
	if (!g_skip_uac2)  snd_pcm_close(g_st.play_uac2.pcm);
	if (!g_skip_phone) snd_pcm_close(g_st.play_phone.pcm);
	for (int b = 0; b < N_BUS_FX; b++)
		fx_free(&g_st.fx_engines[b]);
	if (g_st.ring_event_fd >= 0) close(g_st.ring_event_fd);
	pthread_mutex_destroy(&g_st.target_lock);
	mlog("mixer-pro exit clean");
	return 0;

err:
	mlog("mixer-pro startup failed");
	return 1;
}
