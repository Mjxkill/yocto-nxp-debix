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
#include "asrc.h"

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

/* Forward decl pour les threads UAC2 (mlog + helpers conv définis plus bas) */
static void mlog(const char *fmt, ...);
static inline float    s32_to_f(int32_t s);
static inline int32_t  f_to_s32(float f);

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
#define UAC2_RING_PERIODS  8
#define UAC2_RING_FRAMES   (PERIOD_FRAMES * UAC2_RING_PERIODS)
#define UAC2_CH            8   /* = N_INPUT_STEMS = N_OUTPUT_UAC2 */

typedef struct {
	int32_t      buf[UAC2_RING_FRAMES * UAC2_CH];
	atomic_uint  wr;
	atomic_uint  rd;
	atomic_ulong drops;
	atomic_ulong xruns;
} uac2_ring_t;

static uac2_ring_t g_ring_uac2_cap;
static uac2_ring_t g_ring_uac2_play;

/* V8.2 — ASRC instances (drift correction USB ↔ DSP).
 *   asrc_uac2_cap  : convertit ring USB-rate → audio_thread DSP-rate
 *   asrc_uac2_play : convertit audio_thread DSP-rate → ring USB-rate
 * Both updated every audio_thread iteration with ring fill measurement.
 */
static asrc_t g_asrc_uac2_cap;
static asrc_t g_asrc_uac2_play;

/* Fill (frames disponibles à la lecture) — utilisé par PID ASRC. */
static int uac2_ring_fill(uac2_ring_t *r)
{
	unsigned wi = atomic_load_explicit(&r->wr, memory_order_acquire);
	unsigned ri = atomic_load_explicit(&r->rd, memory_order_relaxed);
	return (int)(wi - ri);   /* unsigned wrap OK */
}

/* Peek jusqu'à `max_n` frames sans avancer le read pointer.
 * Retourne le nombre effectivement copié (≤ max_n). */
static int uac2_ring_peek(uac2_ring_t *r, int32_t *out, int max_n)
{
	unsigned wi = atomic_load_explicit(&r->wr, memory_order_acquire);
	unsigned ri = atomic_load_explicit(&r->rd, memory_order_relaxed);
	unsigned avail = wi - ri;
	int n = (int)avail < max_n ? (int)avail : max_n;
	for (int f = 0; f < n; f++) {
		unsigned slot = (ri + f) % UAC2_RING_FRAMES;
		memcpy(&out[f * UAC2_CH], &r->buf[slot * UAC2_CH],
		       UAC2_CH * sizeof(int32_t));
	}
	return n;
}

/* Avance le read pointer de `n` frames (commit après asrc_run). */
static void uac2_ring_advance(uac2_ring_t *r, int n)
{
	unsigned ri = atomic_load_explicit(&r->rd, memory_order_relaxed);
	atomic_store_explicit(&r->rd, ri + (unsigned)n, memory_order_release);
}

/* Pop N frames dans `out`. Si pas assez disponibles, complète zeros et
 * retourne le nombre effectivement extrait. Avance le rd. */
static int uac2_ring_pop_n(uac2_ring_t *r, int32_t *out, int n)
{
	int got = uac2_ring_peek(r, out, n);
	if (got > 0) uac2_ring_advance(r, got);
	if (got < n) {
		memset(&out[got * UAC2_CH], 0,
		       (n - got) * UAC2_CH * sizeof(int32_t));
	}
	return got;
}

/* Push N frames depuis `in`. Si ring n'a pas la place pour les N, ne push
 * QUE ce qui rentre (skip-new policy). Pas de drop oldest qui crée des
 * discontinuités audibles à chaque push saturé. Le PID a la responsabilité
 * de drainer le ring via le ratio ASRC ; en attendant, l'audio en cours
 * de lecture reste continu et les nouveaux samples sont sacrifiés. */
static void uac2_ring_push_n(uac2_ring_t *r, const int32_t *in, int n)
{
	unsigned wi = atomic_load_explicit(&r->wr, memory_order_relaxed);
	unsigned ri = atomic_load_explicit(&r->rd, memory_order_acquire);
	unsigned used = wi - ri;
	int can_push = (int)(UAC2_RING_FRAMES - used);
	if (can_push <= 0) {
		atomic_fetch_add(&r->drops, (unsigned long)n);
		return;
	}
	if (n > can_push) {
		atomic_fetch_add(&r->drops, (unsigned long)(n - can_push));
		n = can_push;
	}
	for (int f = 0; f < n; f++) {
		unsigned slot = (wi + f) % UAC2_RING_FRAMES;
		memcpy(&r->buf[slot * UAC2_CH], &in[f * UAC2_CH],
		       UAC2_CH * sizeof(int32_t));
	}
	atomic_store_explicit(&r->wr, wi + (unsigned)n, memory_order_release);
}

/* Pop 1 period dans `out`. Renvoie 1 si succès, 0 si ring vide (out zeroed). */
static int uac2_ring_pop_period(uac2_ring_t *r, int32_t *out)
{
	unsigned wi = atomic_load_explicit(&r->wr, memory_order_acquire);
	unsigned ri = atomic_load_explicit(&r->rd, memory_order_relaxed);
	unsigned avail = wi - ri;   /* unsigned wrap OK */

	if (avail < PERIOD_FRAMES) {
		memset(out, 0, PERIOD_FRAMES * UAC2_CH * sizeof(int32_t));
		return 0;
	}

	for (unsigned f = 0; f < PERIOD_FRAMES; f++) {
		unsigned slot = (ri + f) % UAC2_RING_FRAMES;
		memcpy(&out[f * UAC2_CH], &r->buf[slot * UAC2_CH],
		       UAC2_CH * sizeof(int32_t));
	}
	atomic_store_explicit(&r->rd, ri + PERIOD_FRAMES,
			      memory_order_release);
	return 1;
}

/* Push 1 period depuis `in`. Si ring full, advance rd (drop oldest). */
static void uac2_ring_push_period(uac2_ring_t *r, const int32_t *in)
{
	unsigned wi = atomic_load_explicit(&r->wr, memory_order_relaxed);
	unsigned ri = atomic_load_explicit(&r->rd, memory_order_acquire);
	unsigned used = wi - ri;

	if (used + PERIOD_FRAMES > UAC2_RING_FRAMES) {
		unsigned drop = used + PERIOD_FRAMES - UAC2_RING_FRAMES;
		atomic_store_explicit(&r->rd, ri + drop, memory_order_release);
		atomic_fetch_add(&r->drops, drop);
	}

	for (unsigned f = 0; f < PERIOD_FRAMES; f++) {
		unsigned slot = (wi + f) % UAC2_RING_FRAMES;
		memcpy(&r->buf[slot * UAC2_CH], &in[f * UAC2_CH],
		       UAC2_CH * sizeof(int32_t));
	}
	atomic_store_explicit(&r->wr, wi + PERIOD_FRAMES,
			      memory_order_release);
}

/* V8.2.c — Drop/insert drift correction PRÉDICTIF (anticipation).
 *
 * Principe (validé utilisateur) : les paquets USB et le ring mixer sont
 * TOUJOURS 96 samples fixed. Le drift host_rate ↔ dsp_rate est absorbé
 * par drop ou insert d'1 échantillon isolé quand nécessaire, AVANT que
 * le buffer local USB ne se remplisse ou se vide.
 *
 * Anticipation :
 *   - on mesure en continu l'écart `local_fill - setpoint` lissé en EMA
 *   - on accumule cette dérive dans un accumulateur fractional
 *   - quand acc franchit ±1, on planifie un drop ou un insert sur le
 *     prochain paquet de 96 (correction = ±1 sample par packet)
 *   - le drop = skip 1 sample en milieu de packet (= consomme 97 du
 *     local_buf pour produire 96 output)
 *   - l'insert = moyenne de 2 voisins en milieu de packet (= consomme
 *     95 du local_buf pour produire 96 output)
 *
 * À 100 ppm de drift : 1 correction toutes les ~200 ms = inaudible. */
#define USB_LOCAL_BUF_FRAMES   (PERIOD_FRAMES * 4)   /* = 384 = 8 ms */
#define USB_LOCAL_SETPOINT     (PERIOD_FRAMES * 1.0f)/* 1 period cible */
#define DRIFT_EMA_ALPHA        0.002f  /* tau ~1 sec @ 500 Hz */
#define DRIFT_GAIN             0.01f   /* correction_per_packet = drift * GAIN */

/* V8.2.c — Helper drop/insert : produit EXACTEMENT PERIOD_FRAMES output
 * depuis (PERIOD_FRAMES + correction) input. correction ∈ {-1, 0, +1}.
 *   correction = +1 : drop  (consomme 97 input, skip 1 sample au milieu)
 *   correction =  0 : direct (copy 96 → 96)
 *   correction = -1 : insert (consomme 95 input, moyenne 2 voisins au milieu)
 */
static void produce_96_drop_insert(const int32_t *in, int32_t *out, int correction)
{
	const int M = PERIOD_FRAMES / 2;     /* drop/insert position au milieu */
	if (correction == 0) {
		memcpy(out, in, PERIOD_FRAMES * UAC2_CH * sizeof(int32_t));
	} else if (correction == +1) {
		/* consume 97, produce 96 : copy in[0..M-1] → out[0..M-1],
		 * skip in[M], copy in[M+1..96] → out[M..95] */
		memcpy(out, in, M * UAC2_CH * sizeof(int32_t));
		memcpy(&out[M * UAC2_CH], &in[(M + 1) * UAC2_CH],
		       (PERIOD_FRAMES - M) * UAC2_CH * sizeof(int32_t));
	} else {  /* correction == -1 */
		/* consume 95, produce 96 : copy in[0..M-1] → out[0..M-1],
		 * out[M] = avg(in[M-1], in[M]), copy in[M..94] → out[M+1..95] */
		memcpy(out, in, M * UAC2_CH * sizeof(int32_t));
		for (int ch = 0; ch < UAC2_CH; ch++) {
			int64_t a = in[(M - 1) * UAC2_CH + ch];
			int64_t b = in[M * UAC2_CH + ch];
			out[M * UAC2_CH + ch] = (int32_t)((a + b) / 2);
		}
		memcpy(&out[(M + 1) * UAC2_CH], &in[M * UAC2_CH],
		       (PERIOD_FRAMES - M - 1) * UAC2_CH * sizeof(int32_t));
	}
}

/* Décide la correction à appliquer au prochain paquet.
 * Entrée : `ring_fill` = nombre de frames présents dans le ring mixer
 * (entre cap_uac2_thread et audio_thread, ou entre audio_thread et
 * play_uac2_thread). C'est l'indicateur réel du drift : si cap_uac2
 * pousse plus vite que audio_thread pop → ring fill monte → drop.
 * Setpoint = UAC2_RING_FRAMES / 2 = 384 (50% fill cible). */
static int next_correction(int ring_fill, float *fill_ema, float *corr_acc)
{
	const float setpoint = (float)(UAC2_RING_FRAMES / 2);
	float err = (float)ring_fill - setpoint;
	*fill_ema += DRIFT_EMA_ALPHA * (err - *fill_ema);
	*corr_acc += (*fill_ema) * DRIFT_GAIN;
	int correction = 0;
	if (*corr_acc >= 1.0f) {
		correction = +1;
		*corr_acc -= 1.0f;
	} else if (*corr_acc <= -1.0f) {
		correction = -1;
		*corr_acc += 1.0f;
	}
	return correction;
}

/* Thread cap UAC2 (V8.2.c) :
 *   1. snd_pcm_readi(BLOCKING) 96 frames @ host_rate dans local_buf
 *   2. décide correction = drop/insert/none selon fill_ema
 *   3. produce 96 output via produce_96_drop_insert (consomme 95/96/97)
 *   4. push 96 fixed au ring mixer
 * Paquets USB ET ring mixer : TOUJOURS 96 fixed. Drift compensé par
 * drop/insert d'1 sample isolé, anticipé sur fill_ema. */
static void *cap_uac2_thread(void *arg)
{
	(void)arg;
	struct sched_param sp = { .sched_priority = RT_PRIO_AUDIO };
	(void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
	mlog("cap_uac2_thread : SCHED_FIFO prio %d", RT_PRIO_AUDIO);

	int32_t local_buf[USB_LOCAL_BUF_FRAMES * UAC2_CH];
	int     local_fill = 0;
	float   fill_ema = 0.0f;
	float   corr_acc = 0.0f;

	snd_pcm_nonblock(g_st.cap_uac2.pcm, 0);
	while (atomic_load(&g_st.running)) {
		int err = snd_pcm_start(g_st.cap_uac2.pcm);
		if (err == 0 || err == -EBADFD) break;
		snd_pcm_recover(g_st.cap_uac2.pcm, err, 1);
		usleep(100000);
	}

	while (atomic_load(&g_st.running)) {
		/* 1. Read 96 du USB dans la suite du local_buf */
		if (local_fill + PERIOD_FRAMES > USB_LOCAL_BUF_FRAMES) {
			/* Local plein : skip 1 paquet (sera drop_drop_drop par
			 * EMA croissant → corrections rapides) */
			atomic_fetch_add(&g_ring_uac2_cap.drops, PERIOD_FRAMES);
			/* Sliding window : oublier les 96 plus vieux */
			memmove(local_buf,
			        &local_buf[PERIOD_FRAMES * UAC2_CH],
			        (local_fill - PERIOD_FRAMES) * UAC2_CH
			            * sizeof(int32_t));
			local_fill -= PERIOD_FRAMES;
		}
		snd_pcm_sframes_t r = snd_pcm_readi(g_st.cap_uac2.pcm,
		                                    &local_buf[local_fill * UAC2_CH],
		                                    PERIOD_FRAMES);
		if (r < 0) {
			atomic_fetch_add(&g_ring_uac2_cap.xruns, 1);
			snd_pcm_recover(g_st.cap_uac2.pcm, r, 1);
			fill_ema = 0.0f;
			corr_acc = 0.0f;
			continue;
		}
		local_fill += r;

		/* 2. Décide correction selon le ring mixer fill (drift réel) */
		int ring_fill = uac2_ring_fill(&g_ring_uac2_cap);
		int correction = next_correction(ring_fill, &fill_ema, &corr_acc);

		/* 3. Si pas assez de samples pour la correction, attendre prochaine */
		int needed = PERIOD_FRAMES + correction;  /* 95 ou 96 ou 97 */
		if (local_fill < needed) continue;

		/* 4. Produire 96 fixed + push au ring */
		int32_t out_pkt[PERIOD_FRAMES * UAC2_CH];
		produce_96_drop_insert(local_buf, out_pkt, correction);
		uac2_ring_push_n(&g_ring_uac2_cap, out_pkt, PERIOD_FRAMES);

		/* 5. Shift local_buf left by `needed` frames */
		int leftover = local_fill - needed;
		if (leftover > 0)
			memmove(local_buf,
			        &local_buf[needed * UAC2_CH],
			        leftover * UAC2_CH * sizeof(int32_t));
		local_fill = leftover;

		/* 6. Expose drift dans status via asrc_t.
		 * fill_ema en frames, converti en ppm relativement à la fréquence
		 * d'échantillonnage (1 sample = 1/48000 sec d'écart).
		 * Direction : ring_fill > setpoint = host plus rapide que DSP. */
		g_asrc_uac2_cap.ratio_ema = 1.0f + fill_ema / 48000.0f;
		atomic_store(&g_asrc_uac2_cap.status,
		             fabsf(fill_ema) > 50.0f ? 1 : 0);
	}
	mlog("cap_uac2_thread exiting");
	return NULL;
}

/* Thread play UAC2 (V8.2.c) :
 *   1. pop 96 fixed depuis ring mixer
 *   2. décide correction = drop/insert/none selon fill_ema du local_buf
 *   3. produce 96 output via produce_96_drop_insert (consomme 95/96/97)
 *   4. snd_pcm_writei(BLOCKING) 96 fixed @ host_rate
 * Paquets mixer ring ET USB write : TOUJOURS 96 fixed. */
static void *play_uac2_thread(void *arg)
{
	(void)arg;
	struct sched_param sp = { .sched_priority = RT_PRIO_AUDIO };
	(void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
	mlog("play_uac2_thread : SCHED_FIFO prio %d", RT_PRIO_AUDIO);

	int32_t local_buf[USB_LOCAL_BUF_FRAMES * UAC2_CH];
	int     local_fill = 0;
	float   fill_ema = 0.0f;
	float   corr_acc = 0.0f;

	snd_pcm_nonblock(g_st.play_uac2.pcm, 0);

	/* Prefill ALSA + local_buf avec 1 period de silence */
	int32_t silence[PERIOD_FRAMES * UAC2_CH];
	memset(silence, 0, sizeof(silence));
	for (int prime = 0; prime < N_PERIODS - 1; prime++) {
		snd_pcm_sframes_t r = snd_pcm_writei(g_st.play_uac2.pcm,
		                                     silence, PERIOD_FRAMES);
		if (r < 0) snd_pcm_recover(g_st.play_uac2.pcm, r, 1);
	}

	while (atomic_load(&g_st.running)) {
		/* 1. Pop 96 du ring mixer dans la suite du local_buf */
		if (local_fill + PERIOD_FRAMES > USB_LOCAL_BUF_FRAMES) {
			/* Local plein : skip 1 period (l'EMA va monter →
			 * corrections drop rapides) */
			atomic_fetch_add(&g_ring_uac2_play.drops, PERIOD_FRAMES);
			memmove(local_buf,
			        &local_buf[PERIOD_FRAMES * UAC2_CH],
			        (local_fill - PERIOD_FRAMES) * UAC2_CH
			            * sizeof(int32_t));
			local_fill -= PERIOD_FRAMES;
		}
		/* IMPORTANT : on échantillonne le ring fill AVANT le pop pour
		 * mesurer le drift mixer-side (audio_thread push vs play_uac2 pop).
		 * Pour le play, fill_ring élevé = mixer pousse plus vite que USB
		 * draine = host plus lent = besoin de DROP samples. */
		int ring_fill = uac2_ring_fill(&g_ring_uac2_play);
		uac2_ring_pop_period(&g_ring_uac2_play,
		                     &local_buf[local_fill * UAC2_CH]);
		local_fill += PERIOD_FRAMES;

		/* 2. V8.2.c : DISABLE play-side correction.
		 * Le drop/insert côté play_uac2_thread ne peut PAS drainer le
		 * ring mixer car le pop rate est bound by writei BLOCKING à
		 * host_rate fixe (96/cycle). Forcer correction = 0 = pass-through
		 * comme V8.1 côté play. Le drift play sera audible mais le cap
		 * (où drop/insert FONCTIONNE) restera propre. */
		(void)ring_fill;
		fill_ema = 0.0f;
		corr_acc = 0.0f;
		int correction = 0;

		/* 3. Si pas assez d'input, attendre */
		int needed = PERIOD_FRAMES + correction;
		if (local_fill < needed) continue;

		/* 4. Produire 96 fixed pour l'USB write */
		int32_t out_pkt[PERIOD_FRAMES * UAC2_CH];
		produce_96_drop_insert(local_buf, out_pkt, correction);

		/* 5. Shift local_buf left by `needed` */
		int leftover = local_fill - needed;
		if (leftover > 0)
			memmove(local_buf,
			        &local_buf[needed * UAC2_CH],
			        leftover * UAC2_CH * sizeof(int32_t));
		local_fill = leftover;

		/* 6. Write USB BLOCKING 96 fixed (PipeWire content) */
		snd_pcm_sframes_t wr = snd_pcm_writei(g_st.play_uac2.pcm,
		                                      out_pkt, PERIOD_FRAMES);
		if (wr < 0) {
			atomic_fetch_add(&g_ring_uac2_play.xruns, 1);
			snd_pcm_recover(g_st.play_uac2.pcm, wr, 1);
			fill_ema = 0.0f;
			corr_acc = 0.0f;
		}

		/* 7. Expose pour debugging via op:get_drift */
		g_asrc_uac2_play.ratio_ema = 1.0f + fill_ema / 48000.0f;
		atomic_store(&g_asrc_uac2_play.status,
		             fabsf(fill_ema) > 50.0f ? 1 : 0);
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

/* Process 1 frame du mixer. Modifié en place : in[]→out[].
 * E7.1 : `bus_out` et `ret_out` exposent les bus FX pre/post-effets pour les
 * peak meters (lus par audio_thread après la boucle frame).
 */
static void mix_frame(const float in[N_INPUT_REAL], float out[N_OUTPUT_TOTAL],
		      float bus_out[N_BUS_FX_CH], float ret_out[N_RETURN_CH])
{
	/* 1. Sends : 18 inputs → 8 bus channels (post-strip-gain E7.2) */
	float bus[N_BUS_FX_CH] = {0};
	for (int i = 0; i < N_INPUT_REAL; i++) {
		if (g_st.mute_mask & (1u << i))
			continue;
		float v_in = in[i] * g_st.input_gain[i];
		for (int b = 0; b < N_BUS_FX_CH; b++)
			bus[b] += v_in * g_st.send_gain[i][b];
	}
	if (bus_out)
		memcpy(bus_out, bus, sizeof(bus));

	/* 2. Bus FX (E6.e) : chaque paire (L,R) traverse 1 fx_engine. Le gain
	 * fx_bus_gain[L]/fx_bus_gain[R] est appliqué post-effet (wet niveau).
	 */
	float ret[N_RETURN_CH];
	for (int b = 0; b < N_BUS_FX; b++) {
		float l = bus[b * 2], r = bus[b * 2 + 1];
		float out_l, out_r;
		g_st.fx_engines[b].process(&g_st.fx_engines[b], l, r, &out_l, &out_r);
		ret[b * 2]     = out_l * g_st.fx_bus_gain[b * 2];
		ret[b * 2 + 1] = out_r * g_st.fx_bus_gain[b * 2 + 1];
	}
	if (ret_out)
		memcpy(ret_out, ret, sizeof(ret));

	/* 3. Master : 26 sources = 18 in + 8 returns → 18 outputs */
	float src[N_INPUT_TOTAL];
	memcpy(src, in, sizeof(float) * N_INPUT_REAL);
	memcpy(src + N_INPUT_REAL, ret, sizeof(float) * N_RETURN_CH);

	for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
		float v = 0;
		for (int s = 0; s < N_INPUT_TOTAL; s++) {
			if (g_st.mute_mask & (1u << s))
				continue;
			v += src[s] * g_st.input_gain[s] * g_st.master_gain[s][o];
		}
		out[o] = v;
	}
}

/* ============================== Audio loop ========================= */

static void *audio_thread(void *arg)
{
	(void)arg;

	struct sched_param sp = { .sched_priority = RT_PRIO_AUDIO };
	int rt_ok = (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0);
	mlog("audio thread : SCHED_FIFO prio %d %s", RT_PRIO_AUDIO,
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

	while (atomic_load(&g_st.running)) {
		snd_pcm_sframes_t r;

		clock_gettime(CLOCK_MONOTONIC, &t_iter_start);

		/* 1. DSP cap = horloge maître (blocking read) */
		r = snd_pcm_readi(g_st.cap_dsp.pcm, cap_dsp_buf, PERIOD_FRAMES);
		if (r < 0) { pcm_recover(g_st.cap_dsp.pcm, r); memset(cap_dsp_buf, 0, sizeof(cap_dsp_buf)); }

		/* V8.2.b : UAC2 cap path = pop 96 fixed du ring mixer.
		 * Le ring est alimenté par cap_uac2_thread qui fait l'ASRC
		 * AVANT de push : le ring véhicule TOUJOURS du 96-fixed @ DSP rate.
		 * Pas de drift visible côté audio_thread (== DSP chain inchangée). */
		if (g_skip_uac2) {
			memset(cap_uac2_buf, 0, sizeof(cap_uac2_buf));
		} else {
			uac2_ring_pop_period(&g_ring_uac2_cap, cap_uac2_buf);
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
		pthread_mutex_lock(&g_st.target_lock);
		smooth_gains();
		pthread_mutex_unlock(&g_st.target_lock);

		float in[N_INPUT_REAL];
		float out[N_OUTPUT_TOTAL];

		/* E7.1 : peaks per channel calculés frame-par-frame, agrégés en
		 * uint32_t raw abs (scaled S32). Decay backend après la loop. */
		uint32_t pk_in[N_INPUT_TOTAL] = {0};
		uint32_t pk_out[N_OUTPUT_TOTAL] = {0};
		uint32_t pk_fx[N_BUS_FX_CH] = {0};

		for (int f = 0; f < PERIOD_FRAMES; f++) {
			for (int i = 0; i < N_INPUT_MICS; i++)
				in[i] = s32_to_f(cap_dsp_buf[f * N_INPUT_MICS + i]);
			for (int i = 0; i < N_INPUT_STEMS; i++)
				in[N_INPUT_MICS + i] = s32_to_f(cap_uac2_buf[f * N_INPUT_STEMS + i]);
			for (int i = 0; i < N_INPUT_PHONE; i++)
				in[N_INPUT_MICS + N_INPUT_STEMS + i] =
					s32_to_f(cap_phone_buf[f * N_INPUT_PHONE + i]);

			float bus_pre[N_BUS_FX_CH];
			float ret_post[N_RETURN_CH];
			mix_frame(in, out, bus_pre, ret_post);

			/* E7.5 : push current frame into each active analyzer tap.
			 * Reads the per-tap kind/a/b atomically so the control_thread
			 * can re-target a tap without holding a lock. b == -1 means
			 * mono (R duplicates L). */
			for (int t = 0; t < N_TAPS; t++) {
				int kind = atomic_load_explicit(
					&g_taps[t].kind, memory_order_relaxed);
				if (kind == TAP_KIND_NONE)
					continue;
				int a = atomic_load_explicit(
					&g_taps[t].a, memory_order_relaxed);
				int b = atomic_load_explicit(
					&g_taps[t].b, memory_order_relaxed);
				float lv = 0.0f, rv = 0.0f;
				switch (kind) {
				case TAP_KIND_INPUT:
					if (a >= 0 && a < N_INPUT_REAL)        lv = in[a];
					else if (a >= N_INPUT_REAL && a < N_INPUT_TOTAL)
						lv = ret_post[a - N_INPUT_REAL];
					if (b >= 0) {
						if (b < N_INPUT_REAL)               rv = in[b];
						else if (b < N_INPUT_TOTAL)
							rv = ret_post[b - N_INPUT_REAL];
					} else rv = lv;
					break;
				case TAP_KIND_BUS_PRE:
					if (a >= 0 && a < N_BUS_FX_CH)         lv = bus_pre[a];
					if (b >= 0 && b < N_BUS_FX_CH)         rv = bus_pre[b];
					else                                    rv = lv;
					break;
				case TAP_KIND_OUTPUT:
					if (a >= 0 && a < N_OUTPUT_TOTAL)      lv = out[a];
					if (b >= 0 && b < N_OUTPUT_TOTAL)      rv = out[b];
					else                                    rv = lv;
					break;
				}
				analyzer_tap_write(&g_taps[t], lv, rv);
			}

			for (int o = 0; o < N_OUTPUT_DSP; o++)
				play_dsp_buf[f * N_OUTPUT_DSP + o] = f_to_s32(out[o]);
			for (int o = 0; o < N_OUTPUT_UAC2; o++)
				play_uac2_buf[f * N_OUTPUT_UAC2 + o] =
					f_to_s32(out[N_OUTPUT_DSP + o]);
			for (int o = 0; o < N_OUTPUT_PHONE; o++)
				play_phone_buf[f * N_OUTPUT_PHONE + o] =
					f_to_s32(out[N_OUTPUT_DSP + N_OUTPUT_UAC2 + o]);

			/* E7.1 peaks : inputs réels (18) depuis in[], returns (8)
			 * depuis ret_post[], bus pre-FX (8) depuis bus_pre[], outputs
			 * (18) depuis out[]. Tous en float [-1.0, 1.0] → scale uint32. */
			for (int i = 0; i < N_INPUT_REAL; i++) {
				float v = in[i] < 0 ? -in[i] : in[i];
				uint32_t a = (uint32_t)(v * 2147483647.0f);
				if (a > pk_in[i]) pk_in[i] = a;
			}
			for (int i = 0; i < N_RETURN_CH; i++) {
				float v = ret_post[i] < 0 ? -ret_post[i] : ret_post[i];
				uint32_t a = (uint32_t)(v * 2147483647.0f);
				if (a > pk_in[N_INPUT_REAL + i]) pk_in[N_INPUT_REAL + i] = a;
			}
			for (int b = 0; b < N_BUS_FX_CH; b++) {
				float v = bus_pre[b] < 0 ? -bus_pre[b] : bus_pre[b];
				uint32_t a = (uint32_t)(v * 2147483647.0f);
				if (a > pk_fx[b]) pk_fx[b] = a;
			}
			for (int o = 0; o < N_OUTPUT_TOTAL; o++) {
				float v = out[o] < 0 ? -out[o] : out[o];
				uint32_t a = (uint32_t)(v * 2147483647.0f);
				if (a > pk_out[o]) pk_out[o] = a;
			}
		}

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

		/* 3. E6.g Phase 2 : DSP play traité par thread séparé via ring SPSC.
		 *    Le thread audio ne fait QUE push dans le ring (rapide, atomic).
		 *    Si ring full → on écrase le plus vieux (drop policy) pour ne
		 *    jamais bloquer la cap.
		 */
		unsigned wi = atomic_load_explicit(&g_st.ring_write_idx, memory_order_relaxed);
		unsigned ri = atomic_load_explicit(&g_st.ring_read_idx,  memory_order_acquire);
		unsigned avail = wi - ri;   /* unsigned arithmetic wraps OK */
		if (avail + PERIOD_FRAMES > RING_FRAMES) {
			/* Drop policy : avance read_idx pour faire de la place */
			unsigned drop = avail + PERIOD_FRAMES - RING_FRAMES;
			atomic_store_explicit(&g_st.ring_read_idx, ri + drop,
					      memory_order_release);
			atomic_fetch_add(&g_st.ring_drops, drop);
		}
		/* Copy 96 frames × 8 ch dans le ring (avec wrap modulo RING_FRAMES) */
		for (int f = 0; f < PERIOD_FRAMES; f++) {
			unsigned slot = (wi + f) % RING_FRAMES;
			memcpy(&g_st.ring_buf[slot * N_OUTPUT_DSP],
			       &play_dsp_buf[f * N_OUTPUT_DSP],
			       N_OUTPUT_DSP * sizeof(int32_t));
		}
		atomic_store_explicit(&g_st.ring_write_idx, wi + PERIOD_FRAMES,
				      memory_order_release);

		/* E6.h : signal play_thread (eventfd compteur, write 1 = 1 nouvelle
		 * période dispo). play_thread bloque sur read(eventfd) jusqu'au signal.
		 */
		uint64_t one = 1;
		(void)write(g_st.ring_event_fd, &one, sizeof(one));

		/* V8.2.b : UAC2 play path = push 96 fixed au ring mixer.
		 * play_uac2_thread fait l'ASRC APRÈS pop : le ring véhicule
		 * TOUJOURS du 96-fixed @ DSP rate. Pas de drift visible côté
		 * audio_thread. */
		if (!g_skip_uac2) {
			uac2_ring_push_n(&g_ring_uac2_play, play_uac2_buf,
			                 PERIOD_FRAMES);
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
	struct sched_param sp = { .sched_priority = RT_PRIO_AUDIO + 1 };
	if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
		mlog("WARN: play_thread SCHED_FIFO failed: %s", strerror(errno));
	else
		mlog("play_thread : SCHED_FIFO prio %d", RT_PRIO_AUDIO + 1);

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
	char reply[512];

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
		char body[512];
		g_st.fx_engines[bus].get_state(&g_st.fx_engines[bus], body, sizeof(body));
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"bus\":%d,%s}\n", bus, body);
		write(fd, reply, strlen(reply));

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
		/* V8.2 — indicateur synchro horloges (drift en ppm + status). */
		char reply[512];
		int sc = atomic_load(&g_asrc_uac2_cap.status);
		int sp = atomic_load(&g_asrc_uac2_play.status);
		snprintf(reply, sizeof(reply),
			"{\"ok\":true,\"drift\":{"
			"\"uac2_cap\":{\"ppm\":%.2f,\"ratio\":%.7f,"
				"\"fill\":%d,\"status\":%d},"
			"\"uac2_play\":{\"ppm\":%.2f,\"ratio\":%.7f,"
				"\"fill\":%d,\"status\":%d},"
			"\"drops_uac2_cap\":%lu,\"drops_uac2_play\":%lu,"
			"\"xruns_uac2_cap\":%lu,\"xruns_uac2_play\":%lu"
			"}}\n",
			asrc_drift_ppm(&g_asrc_uac2_cap),
			asrc_ratio(&g_asrc_uac2_cap),
			uac2_ring_fill(&g_ring_uac2_cap), sc,
			asrc_drift_ppm(&g_asrc_uac2_play),
			asrc_ratio(&g_asrc_uac2_play),
			uac2_ring_fill(&g_ring_uac2_play), sp,
			atomic_load(&g_ring_uac2_cap.drops),
			atomic_load(&g_ring_uac2_play.drops),
			atomic_load(&g_ring_uac2_cap.xruns),
			atomic_load(&g_ring_uac2_play.xruns));
		write(fd, reply, strlen(reply));

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
		else if (!strcmp(argv[i], "--help")) {
			fprintf(stderr,
				"usage: %s [--no-uac2] [--no-phone]\n"
				"  --no-uac2  : skip UAC2Gadget PCMs (host PC absent)\n"
				"  --no-phone : skip Phone aloop PCMs\n", argv[0]);
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

	/* V8.2.b — Init ASRC pour drift correction UAC2.
	 * Setpoint = USB_LOCAL_SETPOINT = 1 period (96 frames) du buffer local
	 * USB ↔ ASRC interne aux threads cap_uac2_thread / play_uac2_thread. */
	asrc_init(&g_asrc_uac2_cap,  UAC2_CH, USB_LOCAL_SETPOINT);
	asrc_init(&g_asrc_uac2_play, UAC2_CH, USB_LOCAL_SETPOINT);

	/* Lock memory for RT */
	mlockall(MCL_CURRENT | MCL_FUTURE);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	pthread_t th_audio, th_ctrl, th_play, th_analyzer;
	pthread_t th_cap_uac2, th_play_uac2;
	pthread_create(&th_ctrl, NULL, control_thread, NULL);
	pthread_create(&th_play, NULL, play_thread, NULL);   /* E6.g Phase 2 */
	pthread_create(&th_audio, NULL, audio_thread, NULL);
	pthread_create(&th_analyzer, NULL, analyzer_thread, NULL);  /* E7.5 */
	/* V8.1 : threads UAC2 dédiés (isolation USB ↔ DSP) */
	if (!g_skip_uac2) {
		pthread_create(&th_cap_uac2,  NULL, cap_uac2_thread,  NULL);
		pthread_create(&th_play_uac2, NULL, play_uac2_thread, NULL);
	}

	pthread_join(th_audio, NULL);
	pthread_join(th_play, NULL);
	pthread_join(th_ctrl, NULL);
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
