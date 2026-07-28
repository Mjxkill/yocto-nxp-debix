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
#include "sampler.h"  /* V14.0 étape 1 : sampleur V12-SMP (page PADS) */
#include "looper.h"   /* V14.0 étape 1 : loopstation V12-LOOP-PRO */
#include "midix.h"    /* V14.0 étape 1 : expandeur MIDI V12-MIDIX */
#include "strip_dyn.h" /* V14.0 étape 2 : gate + comp de tranche + lien stéréo */
#include "master.h"    /* V14.0 étape 2 : EQ mastering + makeup LUFS */
#include "automix.h"   /* V14.0 étape 2 : AUTOMIX LIVE (Dugan+BANDMIX+EQ placement) */
#include "voice.h"     /* V14.0 étape 2 : vfocus + spatializer voix */
#include "persist.h"   /* V14.0 étape 2e : sérialisation état + scènes */
#include "uac2_ring.h" /* V14.0 étape 3 : isolation USB UAC2 (rings+threads+drift) */

/* ============================== State ============================== */
/* struct alsa_pcm + struct mixer_state : déplacées dans state.h (V14.0
 * étape 0). L'instance globale reste définie ICI — state.h ne porte que
 * les types et l'extern. */

struct mixer_state g_st;   /* instance unique — extern dans state.h */

/* V8.x UAC2 : rings + threads + drift/ASRC + timing — déplacés dans
 * uac2_ring.c/uac2_ring.h (V14.0 étape 3). */

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
atomic_int g_presets_dirty = 0;   /* extern dans state.h (V14.0) */

/* V9.5.21 — remap des 8 mics DSP : in_block[i] = slot TDM g_mic_map[i].
 * Défaut identité (0..7). Corrige un ordre de slots/câblage TAC ≠ M1..M8. */
atomic_int g_mic_map[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

/* V9.5.21 — gain de sortie par strip OUT (×1000, milli-linéaire). Trim final
 * appliqué après l'insert, avant interleave. Défaut 1000 (= ×1.0). Initialisé
 * dans main() (zero-init = silence sinon). */
atomic_int g_out_gain_m[N_OUTPUT_TOTAL];
/* gain de sortie LISSÉ, écrit uniquement par l'audio_thread (critic dfeb668d :
 * appliquer la cible brute par pas de 0.5 dB = zipper noise audible).
 * alpha 1/16 par période 2 ms → tau ≈ 32 ms. */
static float g_out_gain_cur[N_OUTPUT_TOTAL];

/* V9.5.21b — copie de la spec insert (set_insert) pour persistance : la
 * chaîne mastering + le mode assistant + le routage étaient PERDUS à chaque
 * reboot (re-setup manuel). Protégée par g_st.target_lock (écrite dans le
 * handler set_insert, lue par save_mixer_state). */
char g_insert_spec_engine[FX_CHAIN_MAX][32];
char g_insert_spec_uri[FX_CHAIN_MAX][256];
int  g_insert_spec_n = 0;

/* V9.4 — insert mastering : chaîne de N plugins sur out_0+out_1 DSP.
 * g_insert_active = 0 : bypass total, mix_block out directement vers convert.
 * g_insert_active = 1 : g_insert_chain.process_block sur out_block[0..1].
 * Init/swap protégé par target_lock (cohérent avec mix_block). */
fx_engine_t g_insert_chain;
atomic_int  g_insert_active = 0;
/* V13-SCENES : bypass runtime du mastering (chaîne conservée chaude) */
atomic_int  g_insert_bypass = 0;

/* V13-SCENES : profils complets (définis après save_state_to) */
#define SCENE_SLOTS 6
#define SCENE_DIR   "/var/lib/mixer-pro/scenes"
/* V9.5.12 — état Mixer Assistant (consommé par daemon mixer-ml-inference
 * via socket get_assistant). mixer-pro ne fait PAS d'inférence TFLite
 * (process séparé pour éviter conflit galcore + audio_thread RT99).
 *  - mode  : 0=passthrough, 1=mastering
 *  - source: 0=HW IN, 1=USB IN
 */
_Atomic int g_assistant_mode   = 0;
_Atomic int g_assistant_source = 0;
/* g_no_asrc déclaré plus haut près de g_shift_ppm */

/* Logging (mlog) + ALSA helpers (pcm_open, pcm_recover) + conversions
 * s32↔float : déplacés dans util.c/util.h (V14.0 étape 0). */

/* ============================== Mixer core ========================= */

/* V12-SMP — sampleur : déplacé dans sampler.c/sampler.h (V14.0 étape 1). */

/* V12-LOOP-PRO — loopstation : déplacée dans looper.c/looper.h (V14.0 étape 1). */
/* V13.3 lien stéréo + V12-EXP + V13-COMP : déplacés dans strip_dyn.c/h (V14.0 étape 2). */
/* smp_render : déplacé dans sampler.c (V14.0 étape 1). */

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

/* V13-BANDMIX + EQ placement : déplacés dans automix.c/h (V14.0 étape 2). */

/* V13.7 MASTER — EQ mastering + makeup LUFS : déplacé dans master.c/h
 * (V14.0 étape 2). */

/* bmx_meas + g_bmx + eqx_render : déplacés dans automix.c/h (V14.0 étape 2). */

/* V13-VFOCUS + V13.9 SPATIALIZER : déplacés dans voice.c/voice.h (V14.0 étape 2). */

static float g_mix_bus_in[N_BUS_FX_CH][PERIOD_FRAMES];
static float g_mix_bus_out[N_BUS_FX_CH][PERIOD_FRAMES];
static float g_mix_ret[N_RETURN_CH][PERIOD_FRAMES];

#include "dsp_block.h"   /* V14.0 étape 2 : mac/mul_block_n4 (NEON) */

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

/* Persistance + scènes : déplacées dans persist.c/persist.h (V14.0 étape 2e). */

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

	/* V12-LOOP-PRO : buffers loopstation (looper.c, V14.0 étape 1) */
	loop_init();

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
