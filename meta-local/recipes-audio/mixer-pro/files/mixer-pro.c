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
#include "audio_loop.h" /* V14.0 étape 3b : audio_thread + play_thread */
#include "control.h"    /* V14.0 étape 4 : dispatcher ops + helpers JSON */

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
/* g_skip_uac2 / g_skip_phone : déplacés dans audio_loop.c (V14.0 étape 3b) */

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
/* g_out_gain_cur : audio-owned, déplacé dans audio_loop.c (V14.0 étape 3b) */

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
/* SCENE_SLOTS / SCENE_DIR : persist.h (V14.0 étape 4) */
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

/* Mixer core (smooth_gains + mix_block) + audio_thread + play_thread :
 * déplacés dans audio_loop.c/audio_loop.h (V14.0 étape 3b). */

/* Control socket (json helpers + handle_cmd cœur + control_thread) :
 * déplacé dans control.c/control.h (V14.0 étape 4b). */

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
