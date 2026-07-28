// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * state — état central partagé du mixer (struct mixer_state + g_st).
 *
 * Extraction V14.0 (étape 0, ARCHI_V14_RESTRUCTURATION.md) : les types et
 * l'instance globale g_st sortent de mixer-pro.c pour être visibles des
 * modules (util, automix, audio_loop…). La DÉFINITION de g_st reste dans
 * mixer-pro.c ; ce header ne porte que les types et l'extern.
 *
 * Règles d'accès (inchangées) :
 *   - *_target : écrits par le thread control sous g_st.target_lock ;
 *   - *_gain / env / états : écrits UNIQUEMENT par l'audio_thread (slew) ;
 *   - stats : atomics relaxed (usage visuel/diagnostic).
 * Ce header grossira d'externs au fil des étapes 1-4, au rythme où les
 * modules extraits en ont réellement besoin — jamais en avance.
 */
#ifndef MIXER_STATE_H
#define MIXER_STATE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <alsa/asoundlib.h>

#include "mixer-pro.h"
#include "effects.h"

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

	/* V12-AMX — automix Dugan (gain sharing). L'auto-gain COMPOSE avec
	 * le fader (multiplicateur séparé, jamais input_target). Non-membre
	 * ⇒ automix_gain ≡ 1.0 (chemin identique à avant). Énergie mesurée
	 * POST-fader (E_i × ig²) : une tranche baissée ne vole pas de part
	 * de gain aux micros actifs (raffinement critic). */
	int   automix_on;                       /* global (écrit ctl, lu audio) */
	float automix_resp_ms;                  /* slew des gains (déf. 100) */
	float automix_floor;                    /* plancher lin (déf. −15 dB) */
	int   automix_member[N_INPUT_TOTAL];
	float automix_weight[N_INPUT_TOTAL];    /* lin (déf. 1.0) */
	float automix_env[N_INPUT_TOTAL];       /* enveloppe énergie (audio) */
	float automix_gain[N_INPUT_TOTAL];      /* lissé, appliqué (audio) */
	float automix_gtarget[N_INPUT_TOTAL];   /* cible Dugan par bloc */
	/* V13-BANDMIX : trim du keeper live (±3 dB, slew lent, jamais
	 * les faders). Multiplié partout où automix_gain l'est. */
	float keeper_gain[N_INPUT_TOTAL];
	float keeper_target[N_INPUT_TOTAL];
	/* V13.9 — BALANCE AUTO : gain de « présence » par voie (voix lead /
	 * chœurs tenus à un écart cible au-dessus du lit musique). Multiplié
	 * dans le master comme keeper_gain. 1.0 = neutre (instruments). */
	float presence_gain[N_INPUT_TOTAL];
	float presence_target[N_INPUT_TOTAL];

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

extern struct mixer_state g_st;   /* définie dans mixer-pro.c */

/* Flag « état à sauver » de la persistance (armé sur toute écriture
 * opérateur/automation, consommé par persistence_thread). Défini dans
 * mixer-pro.c ; rejoindra persist.c à l'étape 2e. */
extern atomic_int g_presets_dirty;

/* V9.5.21 — remap des 8 mics DSP + trims de sortie + spec insert : état
 * transversal (écrit par les ops, lu par l'audio ET la persistance).
 * Définis dans mixer-pro.c ; l'insert rejoindra son module à l'étape 3/4. */
extern atomic_int g_mic_map[8];
extern atomic_int g_out_gain_m[N_OUTPUT_TOTAL];
extern char g_insert_spec_engine[FX_CHAIN_MAX][32];
extern char g_insert_spec_uri[FX_CHAIN_MAX][256];
extern int  g_insert_spec_n;
extern fx_engine_t g_insert_chain;
extern atomic_int  g_insert_active;
extern atomic_int  g_insert_bypass;   /* V13-SCENES : bypass runtime */
/* V9.5.12 — mode Mixer Assistant (0=passthrough, 1=mastering) */
extern _Atomic int g_assistant_mode;
extern _Atomic int g_assistant_source;   /* 0=HW IN, 1=USB IN */

#endif /* MIXER_STATE_H */
