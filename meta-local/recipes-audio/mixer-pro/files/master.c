// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * master — V13.7 : EQ mastering + makeup LUFS (voir master.h).
 * Code déplacé tel quel depuis mixer-pro.c (V14.0 étape 2, extraction pure).
 * Seule adaptation : les structs anonymes g_meq_p et g_mk sont nommées
 * (meq_params / mk_state) pour pouvoir être extern — initialisations
 * identiques.
 */
#include <stdatomic.h>

#include "master.h"

_Atomic int g_master_on;          /* étage master actif (autolive) */

struct eqx_bq g_meq_bank[2][3];
_Atomic int    g_meq_active;         /* banque active (steady) */
_Atomic int    g_meq_pending = -1;   /* banque à fondre (-1 = aucune) */
float          g_meq_st[2][2][3][2]; /* [banque][L/R][biquad][z] */
int            g_meq_fading;         /* audio-owned : fondu en cours */
int            g_meq_xf;             /* audio-owned : position du fondu */
struct meq_params g_meq_p = { 60.0f, +3.0f, 500.0f, -2.5f, 1.0f, 10000.0f, +3.0f };

struct mk_state g_mk = { .makeup_mq = 1000, .lufs_c = -12000 };

static void meq_compute(int bank)
{
	meq_shelf(&g_meq_bank[bank][0], g_meq_p.low_hz, g_meq_p.low_db, 0);
	eqx_peak (&g_meq_bank[bank][1], g_meq_p.mid_hz, g_meq_p.mid_db, g_meq_p.mid_q);
	meq_shelf(&g_meq_bank[bank][2], g_meq_p.air_hz, g_meq_p.air_db, 1);
}

/* CHANGEMENT LIVE : calcule dans la banque inactive (états frais) et signale
 * un crossfade à l'audio → l'ancien et le nouveau filtre sont mélangés en
 * fondu sur ~50 ms. Zéro clic, même en passant par un gain 0 (passe-tout). */
void meq_recalc(void)
{
	int nb = !atomic_load_explicit(&g_meq_active, memory_order_relaxed);
	meq_compute(nb);
	/* états : PAS de reset à zéro (sinon la cloche sonne un transitoire à sa
	 * fréquence = pop). L'audio copie l'état chaud de l'ancienne banque au
	 * démarrage du fondu → les deux filtres partent du même état. */
	atomic_store_explicit(&g_meq_pending, nb, memory_order_release);
}

/* BOOT / ENABLE : pose les coefs direct dans la banque active, pas de fondu
 * (pas d'audio en cours ou reset volontaire). */
void meq_init(void)
{
	int a = atomic_load_explicit(&g_meq_active, memory_order_relaxed);
	meq_compute(a);
	atomic_store_explicit(&g_meq_pending, -1, memory_order_relaxed);
}
