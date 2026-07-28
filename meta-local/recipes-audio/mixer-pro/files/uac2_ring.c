// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * uac2_ring — isolation USB UAC2 (voir uac2_ring.h).
 * Code déplacé tel quel depuis mixer-pro.c (V14.0 étape 3, extraction pure).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <alsa/asoundlib.h>

#include "state.h"       /* g_st (PCMs, running) */
#include "util.h"        /* mlog, pcm_open, pcm_recover */
#include "uac2_ring.h"

/* V8.1.b — Mesure passive du drift USB ↔ DSP (un seul drift, car même
 * horloge USB host pour cap et play). Le thread cap_uac2_thread compte
 * combien de samples il reçoit par seconde de wall-clock (monotonic),
 * compare à 48000 Hz nominal, déduit le drift en ppm.
 * Smoothed via EMA pour stabilité d'affichage.
 * Pas de correction algorithmique — purement informationnel pour le user. */
_Atomic int    g_usb_drift_ppm_x100 = 0; /* drift_ppm × 100 = 0.01 ppm precision */
_Atomic int    g_usb_drift_valid = 0;    /* 0 = pas encore mesuré */

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
_Atomic int g_shift_ppm = 0;
/* V8.8 — bypass ASRC complet (test isolement, --no-asrc) */
_Atomic int g_no_asrc = 0;
/* V8.18 — mode test : si !=0, shift_controller_thread n'écrit plus dans
 * g_shift_ppm. Permet de figer shift à une valeur arbitraire via --fixed-shift
 * pour caractériser la correction expérimentalement. */
_Atomic int g_shift_fixed = 0;

/* V8.19 — diag ASRC : compte les corrections demandées vs réellement appliquées,
 * ainsi que la distribution des n retournés par readi (pour repérer les
 * mini-bursts qui font no-op smooth_*_middle si n < ASRC_K+2). */
_Atomic unsigned long g_dbg_corr_req_insert = 0;
_Atomic unsigned long g_dbg_corr_req_drop   = 0;
_Atomic unsigned long g_dbg_corr_app_insert = 0;
_Atomic unsigned long g_dbg_corr_app_drop   = 0;
_Atomic unsigned long g_dbg_readi_lt10   = 0;
_Atomic unsigned long g_dbg_readi_10_50  = 0;
_Atomic unsigned long g_dbg_readi_50_100 = 0;
_Atomic unsigned long g_dbg_readi_ge100  = 0;
/* V8.21 — debug call de compute_correction depuis cap_uac2_thread */
_Atomic unsigned long g_dbg_cc_called   = 0;
_Atomic unsigned long g_dbg_cc_nonzero  = 0;
_Atomic int           g_dbg_corr_acc_max = 0;
/* V8.22 — régulation fill-based par ring (mode = UAC2_MODE_IDLE défini plus bas) */
_Atomic int g_uac2_cap_warm   = 0;   /* 1 quand fill cap atteint TARGET */
_Atomic int g_uac2_play_warm  = 0;   /* 1 quand fill play atteint TARGET */
_Atomic int g_uac2_cap_mode   = 0;
_Atomic int g_uac2_play_mode  = 0;

/* V8.32 — Timing avec moyenne glissante 10 sec (10 buckets de 1 sec).
 * Min/max globaux persistants, reset uniquement par reset_drift_stats. */
_Atomic uint64_t g_wr_bucket_sum[TIMING_WINDOW_SEC] = {0};
_Atomic uint32_t g_wr_bucket_cnt[TIMING_WINDOW_SEC] = {0};
_Atomic uint64_t g_wr_bucket_epoch[TIMING_WINDOW_SEC] = {0};
_Atomic uint64_t g_rd_bucket_sum[TIMING_WINDOW_SEC] = {0};
_Atomic uint32_t g_rd_bucket_cnt[TIMING_WINDOW_SEC] = {0};
_Atomic uint64_t g_rd_bucket_epoch[TIMING_WINDOW_SEC] = {0};
_Atomic uint32_t g_wr_min_us = UINT32_MAX;
_Atomic uint32_t g_wr_max_us = 0;
_Atomic uint32_t g_rd_min_us = UINT32_MAX;
_Atomic uint32_t g_rd_max_us = 0;

/* V9.1 — instrumentation jitter audio_thread :
 *   - Histogramme prof_iter_us en 5 buckets (cible <1.8 ms = 95%+ idéal)
 *   - Wake-up jitter : retard entre t_next ABSTIME et reprise effective
 *   - Outlier log si iter > 3 ms : breakdown wake/cap/mix/push
 */
_Atomic unsigned long g_iter_lt18  = 0;  /* < 1.8 ms */
_Atomic unsigned long g_iter_18_22 = 0;  /* 1.8 ms - 2.2 ms (cible) */
_Atomic unsigned long g_iter_22_30 = 0;  /* 2.2 ms - 3 ms */
_Atomic unsigned long g_iter_30_50 = 0;  /* 3 ms - 5 ms */
_Atomic unsigned long g_iter_ge50  = 0;  /* > 5 ms (très bad) */
_Atomic long g_wake_jitter_max_us  = 0;
_Atomic long g_wake_jitter_sum_us  = 0;
_Atomic unsigned long g_wake_jitter_count = 0;
struct timespec  g_last_wr_ts = {0};
struct timespec  g_last_rd_ts = {0};
/* V8.15 — dump raw USB cap data après readi, avant tout traitement.
 * Permet de voir ce que l'USB livre exactement. */
FILE *g_usb_cap_dump = NULL;
/* V8.16 — dump play_dsp_buf après matrix mix, juste avant writei vers DSP play. */
FILE *g_dsp_play_dump = NULL;

/* Forward defines pour helpers ci-dessous (vraies définitions plus bas) */
/* defines UAC2_* + paliers : uac2_ring.h */

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

/* uac2_ring_t : défini dans uac2_ring.h */

uac2_ring_t g_ring_uac2_cap;
uac2_ring_t g_ring_uac2_play;

/* Pop N frames du ring, ou silence si moins disponibles. Retourne nb pop. */
int uac2_ring_pop_n(uac2_ring_t *r, int32_t *out, int n)
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
int uac2_ring_pop_period(uac2_ring_t *r, int32_t *out)
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
void uac2_ring_push_n(uac2_ring_t *r, const int32_t *in, int n)
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
void uac2_ring_push_period(uac2_ring_t *r, const int32_t *in)
{
	uac2_ring_push_n(r, in, PERIOD_FRAMES);
}

/* V8.29 — Push ATOMIQUE de 1 period (96 frames). Retourne 1 si push OK,
 * 0 si ring plein (free_space < 96). Dans ce cas, RIEN n'est écrit, wr
 * ne bouge pas, et drops_evt s'incrémente pour comptage. Le caller doit
 * réessayer plus tard avec les MÊMES samples (pas de troncature). */
int uac2_ring_try_push_period(uac2_ring_t *r, const int32_t *in)
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
unsigned uac2_ring_fill(uac2_ring_t *r)
{
	unsigned wi = atomic_load_explicit(&r->wr, memory_order_relaxed);
	unsigned ri = atomic_load_explicit(&r->rd, memory_order_relaxed);
	return wi - ri;   /* unsigned wrap OK */
}

/* V8.6 — shift_ppm piloté uniquement par les events du ring CAP.
 * play full/empty restent comptés mais n'affectent plus shift, car le play
 * USB peut être non consommé (Bitwig ouvre cap sans ouvrir play). */
void *shift_controller_thread(void *arg)
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
void *cap_uac2_thread(void *arg)
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
void *play_uac2_thread(void *arg)
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

