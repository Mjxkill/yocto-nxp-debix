// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * master — V13.7 : étage MASTER — EQ mastering + makeup piloté LUFS (BS.1770).
 *
 * Dans l'audio_thread, AVANT l'insert (spectral_env/exciter/limiter_native)
 * pour que le limiteur EXISTANT tienne les crêtes :
 *   out 0/1 → EQ master (3 biquads) → makeup (piloté LUFS) → insert → out_gain
 * Mètre short-term LUFS K-pondéré (K-weighting ITU-R BS.1770) sur la sortie
 * réelle, lu par bmx_tick qui asservit le makeup vers MASTER_LUFS_TGT.
 * Lié à AUTOMIX LIVE (g_master_on). ARCHI_V13.7_MASTER_LUFS_EQ.md.
 *
 * Extraction V14.0 (étape 2, ARCHI_V14_RESTRUCTURATION.md §10.3) depuis
 * mixer-pro.c — code déplacé tel quel. L'APPLICATION du filtre (crossfade
 * double-banque) et le mètre K-pondéré restent inline dans audio_thread
 * jusqu'à l'étape 3 — d'où l'état exposé ici (bank/st/fading/xf) ; il
 * redeviendra privé quand le rendu migrera. Ops : étape 4.
 */
#ifndef MIXER_MASTER_H
#define MIXER_MASTER_H

#include <stdatomic.h>

#include "mixer-pro.h"   /* SAMPLE_RATE */
#include "dsp_bq.h"      /* struct eqx_bq */

#define MASTER_LUFS_TGT   (-14.0f)
#define MASTER_MK_MAX_DB   (36.0f)   /* makeup = gain-staging global (stems faibles) */
#define MASTER_MK_MIN_DB   (-6.0f)
#define LUFS_ST_A          (1.0f / (3.0f * (float)SAMPLE_RATE))   /* short-term ~3 s */

extern _Atomic int g_master_on;          /* étage master actif (autolive) */

/* --- EQ master : 3 biquads RBJ (low shelf / -500 bell / high shelf), double
 *     buffer pour bascule sans lock depuis le control thread --- */
extern struct eqx_bq g_meq_bank[2][3];
extern _Atomic int    g_meq_active;         /* banque active (steady) */
extern _Atomic int    g_meq_pending;        /* banque à fondre (-1 = aucune) */
extern float          g_meq_st[2][2][3][2]; /* [banque][L/R][biquad][z] */
#define MEQ_XF_LEN 2400                     /* crossfade coefs ~50 ms (anti-clic) */
extern int            g_meq_fading;         /* audio-owned : fondu en cours */
extern int            g_meq_xf;             /* audio-owned : position du fondu */
extern struct meq_params {               /* params (control thread) */
	float low_hz, low_db;
	float mid_hz, mid_db, mid_q;
	float air_hz, air_db;
} g_meq_p;

/* --- makeup LUFS --- */
extern struct mk_state {
	float k1[2][2], k2[2][2];   /* K-weighting : 2 biquads BS.1770 × L/R */
	float ms;                   /* EWMA puissance K-pondérée (short-term) */
	float mk_db;                /* makeup courant en dB (état bmx_tick) */
	_Atomic int makeup_mq;      /* cible makeup ×1000 linéaire (→ audio) */
	float makeup_cur;           /* gain lissé (audio_thread) */
	_Atomic int lufs_c;         /* LUFS short-term ×100 publié (→ bmx_tick) */
} g_mk;

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

/* CHANGEMENT LIVE : calcule dans la banque inactive et signale un crossfade
 * ~50 ms à l'audio — zéro clic, même en passant par un gain 0. */
void meq_recalc(void);
/* BOOT / ENABLE : coefs direct dans la banque active, pas de fondu. */
void meq_init(void);

/* traite les 3 biquads d'une banque pour 1 échantillon (états mis à jour).
 * Inline : appelé par échantillon dans le fondu audio_thread. */
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


/* V14.0 étape 4 : ops control du module (dispatcher control.c).
 * Retourne 1 si l'op est traitée, 0 sinon. */
int master_handle_op(int fd, const char *line);

#endif /* MIXER_MASTER_H */
