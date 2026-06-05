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
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
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

/* ============================== State ============================== */

struct alsa_pcm {
	const char *name;
	snd_pcm_t  *pcm;
	int         channels;
	int         is_capture;
};

struct mixer_state {
	/* ALSA streams */
	struct alsa_pcm cap_dsp, cap_uac2, cap_phone;
	struct alsa_pcm play_dsp, play_uac2, play_phone;

	/* Matrices — gain courant (utilisé par le DSP), gain cible (set par socket) */
	float send_gain[N_INPUT_TOTAL][N_BUS_FX_CH];
	float send_target[N_INPUT_TOTAL][N_BUS_FX_CH];

	float master_gain[N_INPUT_TOTAL][N_OUTPUT_TOTAL];
	float master_target[N_INPUT_TOTAL][N_OUTPUT_TOTAL];

	float fx_bus_gain[N_BUS_FX_CH];   /* gain bus output (post-effet, dry/wet implicite) */
	float fx_bus_target[N_BUS_FX_CH];

	/* E7.2 : strip gain par input (DAW channel fader). S'applique AVANT
	 * sends + master, donc affecte uniformément FX sends et master routing.
	 * Indexé 0..N_INPUT_TOTAL-1 = 18 inputs réels + 8 returns.
	 */
	float input_gain[N_INPUT_TOTAL];
	float input_target[N_INPUT_TOTAL];

	/* E6.e : 1 moteur d'effet par bus (4 bus × stéréo, géré par fx_engine).
	 * Defaults : 0=compressor, 1=reverb, 2=delay, 3=eq.
	 */
	fx_engine_t fx_engines[N_BUS_FX];

	uint32_t mute_mask;              /* bit i = mute src i (32 bits, 26 src réels < 32 OK) */

	/* Smoothing : ramp counter par cellule = bof, on fait un ramp global frame-par-frame */
	uint32_t ramp_pos;               /* 0..GAIN_RAMP_FRAMES, 0 = pas de ramp en cours */

	/* Lock pour writes depuis le thread control */
	pthread_mutex_t target_lock;

	/* Stats */
	atomic_ulong frames_processed;
	atomic_ulong xrun_count;
	atomic_int   running;

	/* E6.f profiling : timings en microsecondes du dernier cycle complet.
	 * Permet d'identifier le hotspot (cap_read vs mix vs play_write).
	 */
	atomic_long  last_cap_read_us;
	atomic_long  last_mix_us;
	atomic_long  last_play_write_us;
	atomic_long  last_iter_us;

	/* E6.g Phase 2 : ring buffer SPSC (single producer = thread audio,
	 * single consumer = thread play DSP). Interleaved 8 ch S32_LE.
	 * write_idx avance par thread audio, read_idx par thread play.
	 * Lockfree : ARM64 atomic 32-bit suffit (uint32 aligned).
	 */
	int32_t      ring_buf[RING_FRAMES * N_OUTPUT_DSP];
	atomic_uint  ring_write_idx;
	atomic_uint  ring_read_idx;
	atomic_ulong ring_drops;          /* nb de samples écrasés (ring full) */

	/* E6.h : eventfd signalé par audio_thread après push, attendu par
	 * play_thread → wakeup immédiat sans polling nanosleep.
	 */
	int          ring_event_fd;

	/* E7.1 : peak meters par voie (uint32 raw abs S32_LE).
	 * Calculés post-mix dans audio_thread, lus par control_thread (op get_meters).
	 * memory_order_relaxed suffit : usage purement visuel, pas de synchro corrélée.
	 * Decay backend ≈ 12 dB/s appliqué par bloc 2 ms (× 0.9375).
	 */
	atomic_uint  peak_in[N_INPUT_TOTAL];    /* 26 voies */
	atomic_uint  peak_out[N_OUTPUT_TOTAL];  /* 18 voies */
	atomic_uint  peak_fx[N_BUS_FX_CH];      /* 8 voies post-FX (returns) */
};

/* E6.f : sanity check atomicité (suggestion critic #2) :
 * sur ARM64 aligned 4-byte float load/store sont atomiques de facto.
 */
_Static_assert(sizeof(float) == 4, "float must be 4 bytes for atomicity assumption");
_Static_assert(_Alignof(float) <= 4, "float alignment compatible with atomicity");

static struct mixer_state g_st;

/* Forward decl pour les threads UAC2 (mlog défini plus bas) */
static void mlog(const char *fmt, ...);

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
/* g_no_asrc déclaré plus haut près de g_shift_ppm */

/* ============================== Logging ============================ */

static void mlog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ============================== ALSA helpers ======================= */

static int pcm_open(struct alsa_pcm *p, const char *name, int channels,
		    snd_pcm_stream_t dir)
{
	int err;
	snd_pcm_hw_params_t *hw;

	p->name = name;
	p->channels = channels;
	p->is_capture = (dir == SND_PCM_STREAM_CAPTURE);

	err = snd_pcm_open(&p->pcm, name, dir, 0);
	if (err < 0) {
		mlog("open(%s, %s): %s", name,
		     p->is_capture ? "capture" : "playback", snd_strerror(err));
		return err;
	}

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(p->pcm, hw);
	snd_pcm_hw_params_set_access(p->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(p->pcm, hw, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(p->pcm, hw, channels);
	unsigned rate = SAMPLE_RATE;
	snd_pcm_hw_params_set_rate_near(p->pcm, hw, &rate, NULL);
	snd_pcm_uframes_t period = PERIOD_FRAMES, buffer = BUFFER_FRAMES;
	snd_pcm_hw_params_set_period_size_near(p->pcm, hw, &period, NULL);
	snd_pcm_hw_params_set_buffer_size_near(p->pcm, hw, &buffer);
	err = snd_pcm_hw_params(p->pcm, hw);
	if (err < 0) {
		mlog("hw_params(%s): %s", name, snd_strerror(err));
		return err;
	}

	/* sw_params : forcer start_threshold = 1 period pour que le PLAY démarre
	 * dès le 1er write (sinon auto-start au buffer plein → jamais en mode
	 * "write one period at a time"). Idem côté cap : avail_min = 1 period.
	 */
	snd_pcm_sw_params_t *sw;
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(p->pcm, sw);
	snd_pcm_sw_params_set_start_threshold(p->pcm, sw,
		p->is_capture ? 1 : (snd_pcm_uframes_t)period);
	snd_pcm_sw_params_set_avail_min(p->pcm, sw, (snd_pcm_uframes_t)period);
	/* V8.26 — activer le HW timestamping pour mesurer drift précis
	 * via snd_pcm_status_get_audio_htstamp(). */
	snd_pcm_sw_params_set_tstamp_mode(p->pcm, sw, SND_PCM_TSTAMP_ENABLE);
	snd_pcm_sw_params_set_tstamp_type(p->pcm, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC);
	err = snd_pcm_sw_params(p->pcm, sw);
	if (err < 0) {
		mlog("sw_params(%s): %s", name, snd_strerror(err));
		return err;
	}

	mlog("opened %s : %s %dch S32_LE @ %u Hz period=%lu buffer=%lu",
	     name, p->is_capture ? "cap" : "play", channels, rate,
	     period, buffer);
	return 0;
}

static int pcm_recover(snd_pcm_t *pcm, int err)
{
	atomic_fetch_add(&g_st.xrun_count, 1);
	return snd_pcm_recover(pcm, err, 1);
}

/* ============================== Mixer core ========================= */

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
		const float ig = g_st.input_gain[i];
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
		const float ig = g_st.input_gain[s];
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

		/* Convert S32 → float, déinterleave par channel */
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			for (int i = 0; i < N_INPUT_MICS; i++)
				in_block[i][f] = s32_to_f(cap_dsp_buf[f * N_INPUT_MICS + i]);
			for (int i = 0; i < N_INPUT_STEMS; i++)
				in_block[N_INPUT_MICS + i][f] = s32_to_f(cap_uac2_buf[f * N_INPUT_STEMS + i]);
			for (int i = 0; i < N_INPUT_PHONE; i++)
				in_block[N_INPUT_MICS + N_INPUT_STEMS + i][f] =
					s32_to_f(cap_phone_buf[f * N_INPUT_PHONE + i]);
		}

		/* MIX BLOCK — 1 appel pour 96 frames (vs 96 calls × 1 frame) */
		mix_block(in_block, out_block, bus_pre_block, ret_post_block, PERIOD_FRAMES);

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

		/* Peaks : max(abs) sur N samples par channel */
		for (int i = 0; i < N_INPUT_REAL; i++) {
			float m = 0.0f;
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float v = in_block[i][f] < 0 ? -in_block[i][f] : in_block[i][f];
				if (v > m) m = v;
			}
			pk_in[i] = (uint32_t)(m * 2147483647.0f);
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
	return strncmp(p, op, strlen(op)) == 0;
}

static void handle_cmd(int fd, const char *line)
{
	/* V9.3.3 : 16 KB pour get_fx avec params + ranges (NPU). */
	static char reply[16384];

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
		pthread_mutex_lock(&g_st.target_lock);
		g_st.input_target[src] = gain;
		pthread_mutex_unlock(&g_st.target_lock);
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
		pthread_mutex_lock(&g_st.target_lock);
		if (mute)
			g_st.mute_mask |= (1u << src);
		else
			g_st.mute_mask &= ~(1u << src);
		pthread_mutex_unlock(&g_st.target_lock);
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"op\":\"set_mute\",\"src\":%d,\"mute\":%d}\n",
			 src, mute);
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
		static char body[16384];
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
		if (!strcmp(engine, "compressor")) ok = fx_init_compressor(&new_eng, (float)SAMPLE_RATE);
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

		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"op\":\"set_fx_engine\",\"bus\":%d,"
			 "\"engine\":\"%s\",\"uri\":\"%s\"}\n",
			 bus, engine, uri);
		write(fd, reply, strlen(reply));

	} else if (json_has_op(line, "list_lv2_plugins")) {
		/* V9.2 — Énumère les plugins LV2 RT-safe disponibles. */
		static char lv2_buf[65536];
		int n = fx_lv2_list_uris(lv2_buf, sizeof(lv2_buf));
		dprintf(fd, "{\"ok\":true,\"op\":\"list_lv2_plugins\",\"plugins\":%s}\n",
		        n > 0 ? lv2_buf : "[]");

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
	if (listen(srv, 4) < 0) {
		mlog("listen: %s", strerror(errno));
		close(srv);
		return NULL;
	}
	mlog("control socket listening on %s", MIXER_SOCK_PATH);

	while (atomic_load(&g_st.running)) {
		int cli = accept(srv, NULL, NULL);
		if (cli < 0) {
			if (errno == EINTR) continue;
			mlog("accept: %s", strerror(errno));
			break;
		}
		char buf[1024];
		ssize_t n;
		while ((n = read(cli, buf, sizeof(buf) - 1)) > 0) {
			buf[n] = 0;
			/* Une commande par ligne */
			char *line = buf, *next;
			while (line && *line) {
				next = strchr(line, '\n');
				if (next) *next++ = 0;
				if (*line)
					handle_cmd(cli, line);
				line = next;
			}
		}
		close(cli);
	}

	close(srv);
	unlink(MIXER_SOCK_PATH);
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
	pthread_mutex_init(&g_st.target_lock, NULL);
	atomic_store(&g_st.running, 1);

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
	pthread_t th_cap_uac2, th_play_uac2, th_shift_ctl;
	pthread_create(&th_ctrl, NULL, control_thread, NULL);
	pthread_create(&th_play, NULL, play_thread, NULL);   /* E6.g Phase 2 */
	pthread_create(&th_audio, NULL, audio_thread, NULL);
	pthread_create(&th_analyzer, NULL, analyzer_thread, NULL);  /* E7.5 */
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
