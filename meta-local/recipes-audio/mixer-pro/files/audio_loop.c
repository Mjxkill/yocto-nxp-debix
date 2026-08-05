// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * audio_loop — cœur temps réel : audio_thread + play_thread (voir
 * audio_loop.h). Code déplacé tel quel depuis mixer-pro.c (V14.0 étape 3b,
 * extraction pure).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#include "state.h"       /* g_st, taps, insert, mic_map, out_gain, assistant */
#include "util.h"        /* mlog, pcm_recover, s32<->f */
#include "dsp_block.h"   /* mac/mul_block_n4 (NEON) */
#include "strip_dyn.h"   /* exp/cmp/duck chaîne de tranche */
#include "automix.h"     /* eqx_render, automix_update, g_ms_* */
#include "master.h"      /* meq_chain, g_meq_*, g_mk, K-weighting */
#include "voice.h"       /* duck_render, vspat_render */
#include "antilarsen.h"  /* al_render (V15) */
#include "sampler.h"     /* smp_render */
#include "looper.h"      /* loop_render */
#include "midix.h"       /* midix_render */
#include "uac2_ring.h"   /* rings + timing + warm */
#include "analyzer.h"    /* analyzer_tap_write */
#include "mixer_pro_shm_tap.h"
#include "audio_loop.h"

/* Options command-line : skip une ou plusieurs paires PCMs (pratique en dev
 * quand le host PC USB est absent ou que l'aloop n'est pas chargée). */
int g_skip_uac2  = 0;
int g_skip_phone = 0;

/* gain de sortie LISSÉ, écrit uniquement par l'audio_thread (critic dfeb668d :
 * appliquer la cible brute par pas de 0.5 dB = zipper noise audible).
 * alpha 1/16 par période 2 ms → tau ≈ 32 ms. Init 1.0 par main. */
float g_out_gain_cur[N_OUTPUT_TOTAL];

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

void *audio_thread(void *arg)
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
		/* V15 : notchs anti-larsen logiciels (voies flaguées, posés par
		 * le daemon via ops — enable off = zéro coût) */
		al_render(in_block);
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
void *play_thread(void *arg)
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

