// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * strip_dyn — dynamique par tranche + lien stéréo.
 *
 *  - V12-EXP  : downward expander/gate in-place sur in_block[0..15], AVANT
 *    smp/loop/automix/mix — une seule vérité du signal de tranche
 *    (ARCHI_V12_EXPANDER.md) ;
 *  - V13-COMP : compresseur natif par tranche, APRÈS le gate (ordre console
 *    standard gate→comp), prérequis de l'assistant BANDMIX
 *    (ARCHI_V13_BANDMIX.md) ;
 *  - V13.3    : lien stéréo de paires fixes (2k, 2k+1) — miroir des
 *    écritures fader/mute/gate/comp/automix dans les handlers socket,
 *    JAMAIS dans l'audio (ARCHI_V13.3_STEREO_LINK.md).
 *
 * Patron RT commun : enveloppe crête par bloc 2 ms, coefs attack/release
 * PRÉCALCULÉS à la config (jamais d'expf en RT), rampe de gain linéaire
 * intra-bloc (zipper-free), GR publié en atomic pour la GUI, off = zéro
 * coût. Render appelés par l'audio_thread SOUS target_lock ; configure par
 * le control thread SOUS target_lock.
 *
 * Extraction V14.0 (étape 2, ARCHI_V14_RESTRUCTURATION.md §10.1) depuis
 * mixer-pro.c — code déplacé tel quel. g_exp/g_cmp/g_link restent exposés
 * pour les ops et l'assistant bandmix (migration ops : étape 4).
 */
#ifndef MIXER_STRIP_DYN_H
#define MIXER_STRIP_DYN_H

#include <stdatomic.h>
#include <stdint.h>

#include "mixer-pro.h"   /* N_INPUT_MICS/STEMS/REAL, PERIOD_FRAMES */

#define N_EXP_CH (N_INPUT_MICS + N_INPUT_STEMS)   /* 16 tranches réelles */

/* --- V13.3 lien stéréo --- */
#define N_LINK_PAIRS 8
extern _Atomic int g_link[N_LINK_PAIRS];
/* -1 si non lié, sinon l'indice de la tranche jumelle (src^1). Inline :
 * appelé dans chaque handler socket d'écriture de tranche. */
static inline int link_partner(int src)
{
	if (src < 0 || src >= 2 * N_LINK_PAIRS)
		return -1;
	return atomic_load_explicit(&g_link[src / 2],
				    memory_order_relaxed) ? (src ^ 1) : -1;
}

/* --- V12-EXP expandeur/gate --- */
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
extern struct exp_ch g_exp[N_EXP_CH];

/* Précalculs + clamp des plages — control thread, SOUS target_lock. */
void exp_configure(int src, int on, float thr_db, float ratio,
		   float atk_ms, float rel_ms, float range_db, float hold_ms);
void exp_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES]);

/* --- V13-COMP compresseur de tranche --- */
struct cmp_ch {
	int   on;
	int   releasing;                   /* extinction douce : ramp gain→1 */
	float thr_db, ratio, makeup_db;   /* config user */
	float atk_ms, rel_ms;
	float thr_lin, ka, kr, makeup_lin; /* précalc (control thread) */
	float env, gain;                   /* état audio */
	_Atomic uint32_t gr_mdb;           /* réduction courante milli-dB */
};
extern struct cmp_ch g_cmp[N_EXP_CH];

void cmp_configure(int src, int on, float thr_db, float ratio,
		   float atk_ms, float rel_ms, float makeup_db);
/* PAS un doublon du compresseur d'effects.c — algorithmes distincts à
 * dessein (crête/bloc + loi dB + rampe anti-zipper vs enveloppe/échantillon
 * + loi linéaire). Voir la note complète dans strip_dyn.c. */
void cmp_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES]);

#endif /* MIXER_STRIP_DYN_H */
