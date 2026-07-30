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
#include "control.h"    /* handlers d'ops (V14.0 étape 4) */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include "state.h"
#include "persist.h"

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

/* V14.0 étape 4 : ops du module — appelées par le dispatcher control.
 * Corps déplacés tels quels depuis handle_cmd (extraction pure) ;
 * retourne 1 si l'op est traitée, 0 sinon. */
int master_handle_op(int fd, const char *line)
{
	if (json_has_op(line, "master_eq")) {
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
		return 1;
	}
	return 0;
}
