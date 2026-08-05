// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * antilarsen — V15 : notchs anti-larsen LOGICIELS par voie micro.
 *
 * Actuation de l'anti-larsen v2 (ARCHI_V15_ANTILARSEN_V2.md) : le daemon
 * anti-larsen détecte (FFT taps NPU) et DÉCIDE (sonde → verdict par la
 * boucle → coupable) ; ce module APPLIQUE des notchs RBJ par voie flaguée,
 * sans jamais toucher au TAC (cause racine des plops v1, règle 2026-07-28).
 *
 * - Voies « source larsen possible » flaguées par l'OPÉRATEUR (jamais
 *   deviné) — seules elles portent des notchs ; les autres gardent un
 *   chemin audio strictement identique (off = zéro coût).
 * - Notch = eqx_peak à gain négatif et Q élevé (profondeur contrôlée) ;
 *   AL_SLOTS notchs max par voie.
 * - Anti-plop : double-banque + bascule atomique, états JAMAIS vidés
 *   (pattern eqx validé lot 5b) — pose et retrait sans clic.
 * - Appliqué sur in_block APRÈS le gate (exp) et AVANT l'EQ de placement.
 * - OFF par défaut (invariant : automation non validée = opt-in).
 *
 * Le module possède ses ops control (pattern V14) : larsen_enable,
 * larsen_flag, larsen_notch, larsen_release, larsen_cfg, larsen_status.
 * Flags + réglages persistés (ligne « larsen » du mixer_state) ; les
 * notchs eux-mêmes sont DYNAMIQUES (jamais persistés — la salle change).
 */
#ifndef MIXER_ANTILARSEN_H
#define MIXER_ANTILARSEN_H

#include <stdatomic.h>

#include "mixer-pro.h"    /* N_INPUT_REAL, PERIOD_FRAMES */
#include "dsp_bq.h"       /* struct eqx_bq */
#include "strip_dyn.h"    /* N_EXP_CH */

#define AL_SLOTS 4        /* notchs max par voie (décision 2026-08-04) */

struct al_voice {
	_Atomic int flag;                      /* opérateur : source possible */
	_Atomic int nact;                      /* slots occupés (0 = skip RT) */
	struct eqx_bq bq[2][AL_SLOTS];         /* double-banque (bascule s.clic) */
	_Atomic int   bank;                    /* banque active */
	float st[AL_SLOTS][2];                 /* états audio, JAMAIS vidés */
	/* méta des slots (control thread, sous target_lock) */
	float freq_hz[AL_SLOTS];               /* 0 = slot libre */
	float depth_db[AL_SLOTS];              /* négatif (−12 sonde) */
};

extern struct al_state {
	_Atomic int enable;                    /* kill-switch global (déf. 0) */
	float q;                               /* Q des notchs (déf. 8) */
	float depth_max_db;                    /* plafond (déf. −24) */
	struct al_voice v[N_EXP_CH];
} g_al;

/* Rendu (audio_thread, SOUS target_lock, entre exp_render et eqx_render).
 * Zéro coût si enable=0 ou aucune voie flaguée avec notch actif. */
void al_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES]);

/* ops control du module (dispatcher control.c) — 1 = traitée, 0 sinon */
int antilarsen_handle_op(int fd, const char *line);

#endif /* MIXER_ANTILARSEN_H */
