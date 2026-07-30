// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * voice — traitement de la voix : V13-VFOCUS + V13.9 VOICE SPATIALIZER.
 *
 *  - VFOCUS « place à la voix » (unmasking spectral) : dynamic EQ
 *    sidechainé — la musique (rôles instrument du bandmix) est creusée
 *    UNIQUEMENT dans les bandes où la voix (lead/choir) a de l'énergie,
 *    UNIQUEMENT quand elle chante. 5 bandes peaking RBJ fixes
 *    (250/500/1k/2k/4k, Q 1,4). Zéro alloc, zéro transcendante par sample.
 *    ARCHI_V13_VOICEFOCUS.md.
 *  - SPATIALIZER : widener décorrélé (Lauridsen) — side = copie retardée
 *    ~18 ms injectée ±out0/out1, mono-compatible, amount=0 transparent.
 *
 * Extraction V14.0 (étape 2, ARCHI_V14_RESTRUCTURATION.md §10.4) depuis
 * mixer-pro.c — code déplacé tel quel (structs anonymes nommées pour les
 * externs, inits identiques). g_vf/g_vspat restent exposés pour les ops et
 * la persistance (migration : étapes 2e/4).
 */
#ifndef MIXER_VOICE_H
#define MIXER_VOICE_H

#include <stdatomic.h>
#include <stdint.h>

#include "mixer-pro.h"    /* N_INPUT_REAL, N_OUTPUT_TOTAL, PERIOD_FRAMES */
#include "dsp_bq.h"       /* struct eqx_bq */
#include "strip_dyn.h"    /* N_EXP_CH */

/* --- V13-VFOCUS --- */
#define VF_BANDS 5

extern struct vf_state {
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
} g_vf;

/* précalcul des bandes (main, avant les threads) */
void vf_init(void);
/* Rendu (audio_thread, SOUS target_lock, après cmp_render) : analyse le
 * sidechain voix et creuse les tranches musique. */
void duck_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES]);

/* --- V13.9 VOICE SPATIALIZER --- */
#define VSPAT_RING     4096          /* ≥ délai max (85 ms @48k = 4080) */
#define VSPAT_DLY_DEF  18            /* ms par défaut */

extern struct vspat_state {
	_Atomic int   on;
	_Atomic int   amount_mq;     /* cible ×1000 (0..1000 → gain side 0..1) */
	_Atomic int   delay_smp;     /* retard en samples */
	float         amount_cur;    /* gain lissé (audio_thread) */
	int           wpos;
	float         ring[VSPAT_RING];
} g_vspat;

/* Rendu (audio_thread, APRÈS mix_block et AVANT l'EQ/limiter master) */
void vspat_render(const float in_block[N_INPUT_REAL][PERIOD_FRAMES],
		  float out_block[N_OUTPUT_TOTAL][PERIOD_FRAMES], int N);


/* V14.0 étape 4 : ops control du module (dispatcher control.c).
 * Retourne 1 si l'op est traitée, 0 sinon. */
int voice_handle_op(int fd, const char *line);

#endif /* MIXER_VOICE_H */
