// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dsp_bq — biquads RBJ du moteur (Audio EQ Cookbook).
 *
 * API commune de CALCUL DE COEFFICIENTS. Un seul type de biquad dans tout
 * le moteur (revue 2026-07-28, lot 5b : vf_bq fusionnée dans eqx_bq) ; une
 * seule formule peaking (rbj_peak_core) — servie ici pour le chemin config
 * (cos/sin calculés) ET le chemin RT vfocus (cos/alpha précalculés).
 *
 * Les boucles de RENDU restent dans les modules appelants : chacun possède
 * ses états et son schéma de bascule (double-banque, crossfade…) — seul le
 * design des coefficients est commun. Fonctions pures, aucun état global.
 *
 * Extraction V14.0 (étape 0, ARCHI_V14_RESTRUCTURATION.md) depuis
 * mixer-pro.c — noms conservés (extraction pure, zéro renommage).
 */
#ifndef DSP_BQ_H
#define DSP_BQ_H

struct eqx_bq { float b0, b1, b2, a1, a2; };

/* HPF Butterworth Q=0.707. fc<=0 → biquad neutre (passthrough). */
void eqx_hpf(struct eqx_bq *q, float fc);

/* NOYAU peaking RBJ commun (revue 2026-07-28, lot 5b) : une seule formule
 * dans le moteur. Prend cos(w) et alpha déjà calculés pour servir aussi le
 * chemin RT du vfocus (cos/sin précalculés à l'init, appel par bloc). */
void rbj_peak_core(struct eqx_bq *q, float cw, float al, float A);

/* Peaking par fréquence/gain/Q. fc<=0 ou gain 0 → neutre. */
void eqx_peak(struct eqx_bq *q, float fc, float gdb, float Q);

/* Shelf RBJ S=1 (low si high=0, high si high=1). fc<=0 ou gain 0 → neutre. */
void meq_shelf(struct eqx_bq *q, float fc, float gdb, int high);

#endif /* DSP_BQ_H */
