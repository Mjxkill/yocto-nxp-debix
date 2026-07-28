// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * automix — AUTOMIX LIVE : Dugan + BANDMIX + EQ de placement (voir automix.h).
 * Code déplacé tel quel depuis mixer-pro.c (V14.0 étape 2, extraction pure).
 * Seule adaptation : struct anonyme g_bmx nommée bmx_state (extern) —
 * initialisation identique.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "state.h"       /* g_st (faders, keeper, presence, automix_*) */
#include "util.h"        /* mlog */
#include "dsp_bq.h"      /* eqx_hpf / eqx_peak */
#include "strip_dyn.h"   /* exp_configure / cmp_configure / g_exp / g_cmp */
#include "master.h"      /* g_mk (LUFS), MASTER_LUFS_TGT, g_master_on */
#include "automix.h"

/* ========= V13-BANDMIX — assistant auto-mix musique =========
 * Soundcheck mesuré par tranche → calcul déterministe par RÔLE (gain
 * staging, gate, comp, fader de départ) → suivi live LENT (keeper ±3 dB,
 * parts de loudness vs référence verrouillée, priorité voix). Le cerveau
 * vit dans le plan de contrôle (persistence_thread 1 Hz) ; l'audio ne
 * fait que publier les mean-squares et appliquer keeper_gain (multiplié
 * dans la chaîne automix). ARCHI_V13_BANDMIX.md. */

/* mean square par bloc (brut) + EWMA τ≈3 s, publiés par l'audio
 * (bits float dans un u32). Le contrôle utilise l'EWMA (anti-aliasing —
 * lire 1 bloc de 2 ms par seconde échantillonne 0,2 % du signal). */
_Atomic uint32_t g_ms_in[N_EXP_CH];
_Atomic uint32_t g_ms_avg[N_EXP_CH];
float            g_ms_sm[N_EXP_CH];   /* privé audio_thread */

/* bmx_ms / bmx_avg : inline dans automix.h */

const char *const BR_NAMES[BR_NROLES] = {
	"off", "lead", "choir", "kick", "snare", "drums",
	"bass", "guitar", "keys", "line" };

/* presets par rôle : cible mix (dB rel. lead), gate, comp */
const struct bmx_preset BMX_P[BR_NROLES] = {
	[BR_OFF]    = { 0 },
	[BR_LEAD]   = {  0.0f, 1,  2.0f,  80, 1, -18, 3.0f,  15, 150 },
	[BR_CHOIR]  = { -4.0f, 1,  2.0f,  80, 1, -18, 3.0f,  15, 150 },
	[BR_KICK]   = { -2.0f, 1, 20.0f,  60, 1, -14, 4.0f,   5,  80 },
	[BR_SNARE]  = { -3.0f, 1, 20.0f,  60, 1, -15, 3.0f,   5, 100 },
	[BR_DRUMS]  = { -6.0f, 0,  0,      0, 1, -16, 2.0f,  10, 150 },
	[BR_BASS]   = { -3.0f, 0,  0,      0, 1, -18, 4.0f,  20, 250 },
	[BR_GUITAR] = { -5.0f, 0,  0,      0, 1, -17, 2.5f,  15, 180 },
	[BR_KEYS]   = { -5.0f, 0,  0,      0, 1, -17, 2.0f,  20, 200 },
	[BR_LINE]   = { -5.0f, 0,  0,      0, 0, 0, 0, 0, 0 },
};

/* V13.6 : décalage de seuil comp (dB au-dessus du loudness courant de la
 * source) pour l'AUTOMIX LIVE — seuil = al_ref[role] + offset. Négatif =
 * plus serré (tient la source), positif = ne prend que les crêtes. */
static const float COMP_OFF[BR_NROLES] = {
	[BR_OFF] = 0, [BR_LEAD] = -2, [BR_CHOIR] = -2, [BR_KICK] = +2,
	[BR_SNARE] = +2, [BR_DRUMS] = +2, [BR_BASS] = -3, [BR_GUITAR] = -1,
	[BR_KEYS] = -1, [BR_LINE] = 0,
};

/* ===== V13.6 : EQ logiciel de PLACEMENT par voix (autolive) =====
 * Cascade de 2 biquads RBJ par tranche (HPF + 1 cloche), coefs par RÔLE,
 * états par tranche. Entre gate et comp. Coefs recalculés au changement
 * de rôle seulement. Statique (place les instruments) ; complète le
 * vfocus dynamique. ARCHI_V13.6_AUTOMIX_COMP_EQ.md — g_eqx défini ici,
 * eqx_render (qui lit g_bmx.role) plus bas, APRÈS la struct g_bmx. */
/* 3 biquads par voix = parité avec les mics TAC (mode 3 Biquads/Ch) :
 * HPF + 2 cloches (présence/creusement + modelage). */
/* struct eqx_bq + designers RBJ (eqx_hpf, rbj_peak_core, eqx_peak,
 * meq_shelf) : déplacés dans dsp_bq.c/dsp_bq.h (V14.0 étape 0). */
/* champs : voir struct eqx_state (automix.h) */
struct eqx_state g_eqx;

/* presets : HPF Hz (0=off) ; 2 cloches freq/gain dB/Q (gain 0 = neutre) */
static const struct {
	float hpf;
	float f1, g1, q1;   /* cloche 1 : présence / creusement voix */
	float f2, g2, q2;   /* cloche 2 : modelage (boue/air/corps) */
} EQX_P[BR_NROLES] = {
	[BR_OFF]    = { 0 },
	/* V13.9 : HPF voix relevés — la voix ne bave plus dans le territoire
	 * basse/kick. Lead 250 Hz validé oreille (150 « mieux, pousse encore »). */
	[BR_LEAD]   = { 250, 3500, +3.0f, 0.9f,  500,  -2.0f, 1.0f },  /* présence + dé-boue */
	[BR_CHOIR]  = { 180, 4000, +1.5f, 0.9f,  400,  -1.5f, 1.0f },
	[BR_KICK]   = { 0,   70,   +2.5f, 0.9f,  400,  -3.0f, 1.2f },  /* poids + creux carton */
	[BR_SNARE]  = { 120, 4000, +2.0f, 0.9f,  250,  +1.5f, 1.0f },  /* claquant + corps */
	[BR_DRUMS]  = { 200, 6000, +1.5f, 0.9f,  500,  -1.5f, 1.0f },  /* air + dé-boue */
	[BR_BASS]   = { 30,  80,   +1.5f, 1.0f,  3500, -2.0f, 1.0f },  /* grave + dégage voix */
	[BR_GUITAR] = { 120, 3000, -3.0f, 1.0f,  300,  -2.0f, 1.0f },  /* creuse voix + dé-boue */
	[BR_KEYS]   = { 120, 3000, -3.0f, 1.0f,  300,  -2.0f, 1.0f },  /* creuse voix + dé-boue */
	[BR_LINE]   = { 80,  0, 0, 0,  0, 0, 0 },
};

void eqx_config(int i, int role)   /* control thread (rare) */
{
	/* calcule dans la banque INACTIVE puis bascule atomiquement : l'audio ne
	 * lit jamais des coefs à moitié écrits. Les états NE sont PAS vidés → le
	 * filtre glisse vers les nouveaux coefs sans saut d'échantillon (crack). */
	int nb = !atomic_load_explicit(&g_eqx.bank[i], memory_order_relaxed);
	eqx_hpf(&g_eqx.bq[nb][i][0], EQX_P[role].hpf);
	eqx_peak(&g_eqx.bq[nb][i][1], EQX_P[role].f1, EQX_P[role].g1, EQX_P[role].q1);
	eqx_peak(&g_eqx.bq[nb][i][2], EQX_P[role].f2, EQX_P[role].g2, EQX_P[role].q2);
	atomic_store_explicit(&g_eqx.bank[i], nb, memory_order_release);
	g_eqx.role_of[i] = role;
}


/* champs : voir struct bmx_state (automix.h) */
struct bmx_state g_bmx = { .meas_src = -1, .freeze_db = 12.0f, .risk_decay = 0.05f,
	    .risk_margin = 3.0f, .gate_db = 15.0f, .balance_on = 1,
	    .bal_lufs_tgt = -14.0f, .bal_e_tgt = 3.0f, .bal_c_tgt = 1.5f,
	    .prog_peak = -120.0f,
	    /* solo_auto OFF par défaut : une automation non validée à l'oreille
	     * ne s'active pas toute seule (suspect pompage) — opt-in au bouton */
	    .solo_src = -1, .solo_auto = 0, .solo_db = -1.0f };

/* V13.6 : EQ de placement (audio_thread, entre gate et comp) — lit
 * g_bmx.role donc défini APRÈS g_bmx. Biquads forme II transposée. */
void eqx_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	if (!atomic_load_explicit(&g_eqx.on, memory_order_relaxed))
		return;
	for (int i = 0; i < N_EXP_CH; i++) {
		int role = g_bmx.role[i];
		if (role == BR_OFF)
			continue;
		if (g_eqx.role_of[i] != role)   /* rôle changé : recalcule */
			eqx_config(i, role);
		int bk = atomic_load_explicit(&g_eqx.bank[i], memory_order_acquire);
		for (int b = 0; b < EQX_BQ; b++) {
			struct eqx_bq *q = &g_eqx.bq[bk][i][b];
			float z1 = g_eqx.st[i][b][0], z2 = g_eqx.st[i][b][1];
			float *x = in_block[i];
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float in = x[f];
				float y = q->b0 * in + z1;
				z1 = q->b1 * in - q->a1 * y + z2;
				z2 = q->b2 * in - q->a2 * y;
				x[f] = y;
			}
			g_eqx.st[i][b][0] = z1;
			g_eqx.st[i][b][1] = z2;
		}
	}
}


/* --- tick 1 Hz (persistence_thread) : soundcheck + lock + keeper --- */
void bmx_tick(void)
{
	/* 1. soundcheck : échantillonne la tranche mesurée */
	int src = atomic_load(&g_bmx.meas_src);
	if (src >= 0) {
		float ms  = bmx_ms(src);    /* bloc brut : crête */
		float avg = bmx_avg(src);   /* EWMA 3 s : moyenne/floor */
		g_bmx.acc_ms += avg;
		g_bmx.nblk_s++;
		if (ms > g_bmx.peak_max) g_bmx.peak_max = ms;
		if (g_bmx.warm++ >= 3 && avg < g_bmx.minsm)
			g_bmx.minsm = avg;
		(void)g_bmx.sm;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - g_bmx.meas_t0.tv_sec >= 12) {
			struct bmx_meas *m = &g_bmx.m[src];
			float avg = g_bmx.nblk_s ?
				(float)(g_bmx.acc_ms / g_bmx.nblk_s) : 0.0f;
			m->rms_avg_db = 10.0f * log10f(avg + 1e-12f);
			m->peak_db    = 10.0f * log10f(g_bmx.peak_max + 1e-12f);
			m->floor_db   = 10.0f * log10f(g_bmx.minsm + 1e-12f);
			m->done = 1;
			atomic_store(&g_bmx.meas_src, -1);
			mlog("bandmix: mesure src %d — rms %.1f dB, floor %.1f dB",
			     src, m->rms_avg_db, m->floor_db);
		}
	}

	/* 2. loudness long terme (τ ≈ 10 s) pour lock + keeper.
	 * lt_ms = POST-fader (parts du mix) ; lt_pre = PRÉ-fader (même
	 * référentiel que floor_db mesuré → détection silence cohérente). */
	const float kl = 0.095f;   /* 1 - exp(-1/10) */
	for (int i = 0; i < N_EXP_CH; i++) {
		float ms = bmx_avg(i);   /* EWMA 3 s — anti-aliasing */
		/* BOUCLE FERMÉE : la mesure inclut keeper_gain, sinon le
		 * correcteur ne voit jamais l'effet de ses corrections et
		 * l'intégrateur file aux butées (observé au 1er test). */
		float g = g_st.input_gain[i] * g_st.keeper_gain[i];
		g_bmx.lt_ms[i] += kl * (ms * g * g - g_bmx.lt_ms[i]);
		g_bmx.lt_pre[i] += kl * (ms - g_bmx.lt_pre[i]);
	}

	/* 3. capture de référence (VERROUILLER, 30 ticks) */
	if (g_bmx.locking) {
		for (int i = 0; i < N_EXP_CH; i++)
			if (g_bmx.role[i] != BR_OFF)
				g_bmx.lock_acc[i] += g_bmx.lt_ms[i];
		if (++g_bmx.lock_ticks >= 30) {
			double tot = 1e-12;
			for (int i = 0; i < N_EXP_CH; i++)
				tot += g_bmx.lock_acc[i];
			for (int i = 0; i < N_EXP_CH; i++)
				g_bmx.ref_share[i] =
					(float)(g_bmx.lock_acc[i] / tot);
			g_bmx.ref_valid = 1;
			g_bmx.locking = 0;
			atomic_store(&g_presets_dirty, 1);
			mlog("bandmix: équilibre verrouillé");
		}
	}

	/* 4a. AUTOMIX LIVE (V13.5) : nivellement ANCRÉ SUR LA VOIX, sans saut.
	 * Une ancre suit le loudness de la voix lead quand elle chante et se
	 * GÈLE pendant les breaks (pas de saut de balance). Chaque source
	 * active est amenée à (ancre + offset de rôle) d'après son loudness
	 * intrinsèque pré-fader → balance exacte relative à la voix, gains
	 * modérés (la voix reste ≈ à l'unité, elle EST la référence). Le fader
	 * reste un biais utilisateur. Silence auto par peak-hold. */
	if (g_bmx.autolive) {
		/* ancre = loudness voix lissé ; sinon max des sources actives */
		float Llead = -120.0f, Lmax = -120.0f;
		int   have_lead = 0, any = 0;
		float pre_db[N_EXP_CH];
		int   act[N_EXP_CH];
		for (int i = 0; i < N_EXP_CH; i++) {
			act[i] = 0;
			if (g_bmx.role[i] == BR_OFF) continue;
			float pre = 10.0f * log10f(g_bmx.lt_pre[i] + 1e-12f);
			pre_db[i] = pre;
			if (pre > g_bmx.al_ref[i]) g_bmx.al_ref[i] = pre;
			else g_bmx.al_ref[i] -= 0.5f;
			/* V13.8 : mémoire du risque LENTE (monte instantané, oublie
			 * 0,05 dB/s ≈ 3 dB/min) → tient une source longtemps */
			if (pre > g_bmx.risk[i]) g_bmx.risk[i] = pre;
			else g_bmx.risk[i] -= g_bmx.risk_decay;
			if (pre < -60.0f || pre < g_bmx.al_ref[i] - g_bmx.freeze_db) {
				g_bmx.act_ticks[i] = 0;   /* gel → prochaine
							   * activité = reprise */
				continue;   /* < 1/4 du max reçu : gelée (V13.9) */
			}
			act[i] = 1; any = 1;
			if (g_bmx.act_ticks[i] < 1000) g_bmx.act_ticks[i]++;
			/* V13.9 — solo v2 : ligne de base (monte τ 60 s, descend
			 * τ 20 s ; jamais entretenue par la voie en solo, sinon
			 * le solo remonterait sa propre base). */
			if (g_bmx.solo_base[i] < -500.0f)
				g_bmx.solo_base[i] = pre;
			else if (i != g_bmx.solo_src)
				g_bmx.solo_base[i] +=
					((pre > g_bmx.solo_base[i])
					 ? (1.0f / 60.0f) : (1.0f / 20.0f))
					* (pre - g_bmx.solo_base[i]);
			if (pre > Lmax) Lmax = pre;
			if (g_bmx.role[i] == BR_LEAD && pre > Llead) {
				Llead = pre; have_lead = 1;
			}
		}
		if (!any) return;
		float anchor_now = have_lead ? Llead : Lmax;
		/* ancre lissée ; gelée si pas de voix (have_lead=0 → on garde) */
		if (g_bmx.al_anchor < -110.0f) g_bmx.al_anchor = anchor_now;
		else if (have_lead)
			g_bmx.al_anchor += 0.30f * (anchor_now - g_bmx.al_anchor);
		else if (g_bmx.al_anchor < -110.0f)   /* jamais de voix : suit max */
			g_bmx.al_anchor += 0.10f * (anchor_now - g_bmx.al_anchor);

		/* V13.9 — DÉTECTION AUTO DE SOLO (v2) : voix muette ET une voie
		 * musique monte ≥ +6 dB AU-DESSUS DE SA PROPRE LIGNE DE BASE
		 * (médiane lente) — le vrai signal d'un soliste : il joue plus
		 * fort que lui-même, pas plus fort que la batterie (v1 ne se
		 * déclenchait jamais en solo accompagné). Engage 2 ticks ;
		 * release : voix de retour ou élévation < +3 dB (2 ticks). Ne
		 * touche jamais un solo posé MANUELLEMENT (bouton GUI). */
		if (g_bmx.solo_auto) {
			if (g_bmx.solo_src < 0) {
				int best = -1; float bex = 6.0f;
				for (int i = 0; i < N_EXP_CH; i++) {
					int r = g_bmx.role[i];
					if (!act[i] || r == BR_OFF ||
					    r == BR_LEAD || r == BR_CHOIR)
						continue;
					float ex = pre_db[i] - g_bmx.solo_base[i];
					if (ex > bex) { bex = ex; best = i; }
				}
				if (!have_lead && best >= 0) {
					g_bmx.solo_off_cnt = 0;
					if (++g_bmx.solo_on_cnt >= 2) {
						g_bmx.solo_src = best;
						g_bmx.solo_is_auto = 1;
						g_bmx.solo_on_cnt = 0;
					}
				} else
					g_bmx.solo_on_cnt = 0;
			} else if (g_bmx.solo_is_auto) {
				int s = g_bmx.solo_src;
				int keep = !have_lead && act[s] &&
					   (pre_db[s] - g_bmx.solo_base[s] > 3.0f);
				if (keep)
					g_bmx.solo_off_cnt = 0;
				else if (++g_bmx.solo_off_cnt >= 2) {
					g_bmx.solo_src = -1;
					g_bmx.solo_is_auto = 0;
					g_bmx.solo_off_cnt = 0;
				}
			}
		}

		for (int i = 0; i < N_EXP_CH; i++) {
			if (!act[i]) continue;
			/* V13.9 — SOLO : la voie monte à l'ancre + solo_db, sans
			 * plafond risque, slew rapide 3 dB/tick (montée ~2 s). */
			if (i == g_bmx.solo_src) {
				float tgt = g_bmx.al_anchor + g_bmx.solo_db
					  - pre_db[i];
				if (tgt >  18.0f) tgt =  18.0f;
				if (tgt < -24.0f) tgt = -24.0f;
				float d = tgt - g_bmx.kdb[i];
				if (d >  3.0f) d =  3.0f;
				if (d < -3.0f) d = -3.0f;
				g_bmx.kdb[i] += d;
				continue;
			}
			float place = g_bmx.al_anchor + BMX_P[g_bmx.role[i]].mix_db;
			float tgt = place - pre_db[i];
			/* V13.8/V13.9 — MÉMOIRE DU RISQUE, recentrée sur son but :
			 * l'ANTI-BLAST DE REPRISE. Le plafond (à son niveau fort
			 * mémorisé la source ne dépasse pas place+3) ne s'applique
			 * QUE dans les 5 premières secondes après un retour de
			 * silence. Une source qui JOUE en continu est équilibrée
			 * à sa place sans frein — sinon les instruments dynamiques
			 * (cuivres) restaient 8-10 dB sous leur place en permanence. */
			if (g_bmx.act_ticks[i] <= 5) {
				float risk_cap = place + g_bmx.risk_margin
					       - g_bmx.risk[i];
				if (tgt > risk_cap) tgt = risk_cap;
			}
			if (tgt >  18.0f) tgt =  18.0f;
			if (tgt < -24.0f) tgt = -24.0f;
			float d = tgt - g_bmx.kdb[i];   /* slew ≤1 dB/tick, zm 0,5 */
			if (d > 0.5f)  g_bmx.kdb[i] += (d > 1.0f ? 1.0f : d);
			if (d < -0.5f) g_bmx.kdb[i] += (d < -1.0f ? -1.0f : d);
		}

		/* V13.9 — BALANCE AUTO (table utilisateur). Ramène EN MÊME TEMPS le
		 * master à −14 LUFS ET l'écart voix−musique à +3 dB, en bougeant UN
		 * SEUL gain de groupe par tick selon le quadrant :
		 *   LUFS<−14 & E<3 → monter VOIX    | LUFS<−14 & E>3 → monter MUSIQUE
		 *   LUFS>−14 & E<3 → baisser MUSIQUE | LUFS>−14 & E>3 → baisser VOIX
		 * GEL : dans un creux (prog < crête récente −3 dB) ou groupe muet, on
		 * ne monte JAMAIS → fin de morceau / passage calme restent calmes.
		 * VOIX = LEAD · MUSIQUE = instruments. Les CHŒURS ont leur propre
		 * asservissement d'écart (bal_c_tgt, +1,5 dB) subordonné : leur gain
		 * suit musique+cible, même gel — ils ne pilotent pas le LUFS.
		 * Remplace le chase makeup. */
		if (g_bmx.balance_on) {
			/* RÈGLE UNIVERSELLE « pas de signal → on ne bouge rien » :
			 * chaque groupe n'est sommé que sur ses voies ACTIVES au sens
			 * du gel keeper (act[] : pre ≥ al_ref − freeze_db). Un groupe
			 * sans voie active = h?=0 → ses gains sont GELÉS (ni montée
			 * ni descente) — on n'asservit JAMAIS du bruit de fond. */
			float Pv = 0.0f, Pc = 0.0f, Pm = 0.0f;
			for (int i = 0; i < N_EXP_CH; i++) {
				int r = g_bmx.role[i];
				if (r == BR_OFF || !act[i]) continue;
				if      (r == BR_LEAD)  Pv += g_bmx.lt_ms[i];
				else if (r == BR_CHOIR) Pc += g_bmx.lt_ms[i];
				else                    Pm += g_bmx.lt_ms[i];
			}
			int hv = (Pv > 1e-6f), hm = (Pm > 1e-6f), hc = (Pc > 1e-6f);
			/* peak-hold du programme pour le gel (décroît 0,5 dB/s) */
			float prog = 10.0f * log10f(Pv + Pc + Pm + 1e-12f);
			if (prog > g_bmx.prog_peak) g_bmx.prog_peak = prog;
			else                        g_bmx.prog_peak -= 0.5f;
			/* « au niveau fort » = programme à moins de 3 dB sous sa crête
			 * récente. Sous ce seuil = creux (pause, fin, passage calme) :
			 * on n'autorise PLUS aucune MONTÉE (la descente reste permise). */
			int loud = (prog >= g_bmx.prog_peak - 3.0f);
			float lufs = atomic_load_explicit(&g_mk.lufs_c,
					memory_order_relaxed) * 0.01f;
			if (hv && hm && lufs > -50.0f) {
				/* E = écart RÉEL en sortie : loudness pré-présence (lt_ms)
				 * + les gains de groupe déjà appliqués → boucle fermée
				 * (sinon l'axe écart file aux butées). LUFS l'est déjà. */
				float E = (10.0f * log10f(Pv) - 10.0f * log10f(Pm))
					+ (g_bmx.g_voice_db - g_bmx.g_music_db);
				float lerr = lufs - g_bmx.bal_lufs_tgt;   /* >0 trop fort */
				float eerr = E - g_bmx.bal_e_tgt;          /* >0 voix haute */
				const float DB = 1.0f;                     /* deadband */
				/* STAGING INITIAL (exigence scène : volume utilisable
				 * tout de suite) : 8 dB/s jusqu'au 1er lock ±2 dB,
				 * puis vitesses douces 3/1 dB/s (anti-pompage). */
				if (!g_bmx.bal_staged && fabsf(lerr) <= 2.0f)
					g_bmx.bal_staged = 1;
				float st = !g_bmx.bal_staged ? 8.0f
					 : (fabsf(lerr) > 6.0f) ? 3.0f : 1.0f;
				float dv = 0.0f, dm = 0.0f;
				if (lerr < -DB) {              /* trop faible → MONTER */
					if (eerr > DB) dm = +st;      /* voix trop haute → musique */
					else           dv = +st;      /* sinon → voix */
				} else if (lerr > DB) {       /* trop fort → BAISSER */
					if (eerr > DB) dv = -st;      /* voix trop haute → voix */
					else           dm = -st;      /* sinon → musique */
				} else {                      /* LUFS ok → écart seul */
					if      (eerr >  DB) dv = -1.0f;
					else if (eerr < -DB) dv = +1.0f;
				}
				/* GEL DES MONTÉES dans un creux : ne JAMAIS monter quand
				 * le programme baisse (fin de morceau / passage calme). */
				if (!loud) { if (dv > 0.0f) dv = 0.0f;
					     if (dm > 0.0f) dm = 0.0f; }
				/* garde silence : ne JAMAIS monter un groupe muet */
				if (dv > 0.0f && !hv) dv = 0.0f;
				if (dm > 0.0f && !hm) dm = 0.0f;
				g_bmx.g_voice_db += dv;
				g_bmx.g_music_db += dm;
				if (g_bmx.g_voice_db >  36.0f) g_bmx.g_voice_db =  36.0f;
				if (g_bmx.g_voice_db < -24.0f) g_bmx.g_voice_db = -24.0f;
				if (g_bmx.g_music_db >  36.0f) g_bmx.g_music_db =  36.0f;
				if (g_bmx.g_music_db < -24.0f) g_bmx.g_music_db = -24.0f;
			}
			/* CHŒURS : asservissement d'écart subordonné — tient les
			 * chœurs à musique + bal_c_tgt (boucle fermée, slew ≤1 dB/
			 * tick), même gel : jamais de montée dans un creux. */
			if (hc && hm && lufs > -50.0f) {
				float Ec = (10.0f * log10f(Pc) - 10.0f * log10f(Pm))
					 + (g_bmx.g_choir_db - g_bmx.g_music_db);
				float d = g_bmx.bal_c_tgt - Ec;   /* >0 → monter */
				float cs = g_bmx.bal_staged ? 1.0f : 8.0f;
				if (d >  cs) d =  cs;
				if (d < -cs) d = -cs;
				if (d > 0.0f && !loud) d = 0.0f;  /* gel des montées */
				g_bmx.g_choir_db += d;
				if (g_bmx.g_choir_db >  36.0f) g_bmx.g_choir_db =  36.0f;
				if (g_bmx.g_choir_db < -24.0f) g_bmx.g_choir_db = -24.0f;
			}
		}

		/* V13.6 : compresseur auto par rôle, seuil ADAPTATIF (al_ref =
		 * mémoire de crête → tient le trop-fort, agit au sample). */
		pthread_mutex_lock(&g_st.target_lock);
		for (int i = 0; i < N_EXP_CH; i++) {
			g_st.keeper_target[i] = powf(10.0f, g_bmx.kdb[i] / 20.0f);
			int r = g_bmx.role[i];
			/* V13.9 — gain de groupe (balance auto) : lead/chœurs/musique */
			g_st.presence_target[i] =
				(!g_bmx.balance_on || r == BR_OFF) ? 1.0f
				: (r == BR_LEAD)
					? powf(10.0f, g_bmx.g_voice_db / 20.0f)
				: (r == BR_CHOIR)
					? powf(10.0f, g_bmx.g_choir_db / 20.0f)
					: powf(10.0f, g_bmx.g_music_db / 20.0f);
			/* V13.9 — GATE AUTO : seuil adaptatif = al_ref − gate_db
			 * (al_ref = crête mémorisée de LA voie → le seuil suit la
			 * source ; la repisse, bien plus basse, n'ouvre pas).
			 * Rôles gate_on seulement (LEAD/CHŒURS/KICK/SNARE) ;
			 * ré-armé chaque tick (état préservé si déjà on). */
			if (r != BR_OFF && BMX_P[r].gate_on) {
				float gthr = g_bmx.al_ref[i] - g_bmx.gate_db;
				if (gthr < -80.0f) gthr = -80.0f;
				if (gthr > -20.0f) gthr = -20.0f;
				exp_configure(i, 1, gthr, BMX_P[r].gate_ratio,
					      2.0f, 150.0f, 40.0f,
					      BMX_P[r].gate_hold);
			} else if (g_exp[i].on) {
				/* rôle sans gate (ou off) : DÉSARME la gate auto
				 * héritée d'un rôle précédent (symétrique au comp
				 * — sinon gate fantôme après changement de rôle) */
				exp_configure(i, 0, g_exp[i].thr_db,
					      g_exp[i].ratio, g_exp[i].atk_ms,
					      g_exp[i].rel_ms, g_exp[i].range_db,
					      g_exp[i].hold_ms);
			}
			if (r == BR_OFF || !BMX_P[r].comp_on) {
				if (g_cmp[i].on)
					cmp_configure(i, 0, g_cmp[i].thr_db,
						g_cmp[i].ratio, g_cmp[i].atk_ms,
						g_cmp[i].rel_ms, g_cmp[i].makeup_db);
				continue;
			}
			float thr = g_bmx.al_ref[i] + COMP_OFF[r];
			if (thr < -50.0f) thr = -50.0f;
			if (thr >  -3.0f) thr =  -3.0f;
			cmp_configure(i, 1, thr, BMX_P[r].c_ratio,
				      BMX_P[r].c_atk, BMX_P[r].c_rel, 0.0f);
		}
		pthread_mutex_unlock(&g_st.target_lock);
		/* V13.7 — makeup LUFS. V13.9 : si la BALANCE AUTO est active, c'est
		 * ELLE qui tient −14 LUFS (via les gains de groupe) → le makeup reste
		 * NEUTRE. Deux correcteurs sur le même LUFS = pompage : on n'en garde
		 * qu'un. Le chase makeup ne sert que si la balance est coupée. */
		if (g_bmx.balance_on) {
			g_mk.mk_db = 0.0f;
			atomic_store_explicit(&g_mk.makeup_mq, 1000,
					      memory_order_relaxed);
		} else {
			float lufs = atomic_load_explicit(&g_mk.lufs_c,
					memory_order_relaxed) * 0.01f;
			if (lufs > -50.0f) {
				float want = MASTER_LUFS_TGT - lufs + g_mk.mk_db;
				if (want > MASTER_MK_MAX_DB) want = MASTER_MK_MAX_DB;
				if (want < MASTER_MK_MIN_DB) want = MASTER_MK_MIN_DB;
				float d = want - g_mk.mk_db;
				/* slew adaptatif : 4 dB/s si loin, 1 dB/s près (anti-pompage) */
				float lim = (fabsf(d) > 4.0f) ? 4.0f : 1.0f;
				if (d >  lim) d =  lim;
				if (d < -lim) d = -lim;
				g_mk.mk_db += d;
				atomic_store_explicit(&g_mk.makeup_mq,
					(int)(powf(10.0f, g_mk.mk_db / 20.0f) * 1000.0f),
					memory_order_relaxed);
			}
		}
		return;
	}

	/* 4b. keeper verrouillé : parts courantes vs référence lockée, ±3 dB,
	 * zone morte 1 dB, tranches silencieuses ignorées, priorité voix */
	if (!g_bmx.live || !g_bmx.ref_valid)
		return;
	double tot = 1e-12;
	for (int i = 0; i < N_EXP_CH; i++)
		if (g_bmx.role[i] != BR_OFF)
			tot += g_bmx.lt_ms[i];
	float lead_err = 0.0f;
	for (int i = 0; i < N_EXP_CH; i++)
		if (g_bmx.role[i] == BR_LEAD && g_bmx.ref_share[i] > 1e-9f)
			lead_err = 10.0f * log10f(
				(float)(g_bmx.lt_ms[i] / tot) /
				g_bmx.ref_share[i] + 1e-12f);
	for (int i = 0; i < N_EXP_CH; i++) {
		if (g_bmx.role[i] == BR_OFF || g_bmx.ref_share[i] < 1e-9f)
			continue;
		float pre_db = 10.0f * log10f(g_bmx.lt_pre[i] + 1e-12f);
		float thr_sil = g_bmx.m[i].done
			? fminf(g_bmx.m[i].floor_db + 6.0f,
				g_bmx.m[i].rms_avg_db - 15.0f)
			: -70.0f;
		if (pre_db < thr_sil)
			continue;
		float err = 10.0f * log10f(
			(float)(g_bmx.lt_ms[i] / tot) / g_bmx.ref_share[i]
			+ 1e-12f);
		if (err > -1.0f && err < 1.0f)
			continue;
		float step = 0.5f;
		if (g_bmx.role[i] == BR_LEAD && lead_err < -2.0f)
			step = 1.0f;
		else if (lead_err < -2.0f && err > 0.0f)
			step = 0.25f;
		g_bmx.kdb[i] += (err > 0 ? -step : step);
		if (g_bmx.kdb[i] > 3.0f)  g_bmx.kdb[i] = 3.0f;
		if (g_bmx.kdb[i] < -3.0f) g_bmx.kdb[i] = -3.0f;
	}
	pthread_mutex_lock(&g_st.target_lock);
	for (int i = 0; i < N_EXP_CH; i++)
		g_st.keeper_target[i] = powf(10.0f, g_bmx.kdb[i] / 20.0f);
	pthread_mutex_unlock(&g_st.target_lock);
}

/* --- CALCULER LE MIX : applique staging + gate + comp + faders --- */
void bmx_calc(void)
{
	for (int i = 0; i < N_EXP_CH; i++) {
		int r = g_bmx.role[i];
		if (r == BR_OFF || !g_bmx.m[i].done)
			continue;
		const struct bmx_preset *P = &BMX_P[r];
		const struct bmx_meas *m = &g_bmx.m[i];
		/* gain staging + cible de mix : rms → −20 dBFS puis offset
		 * de rôle, le tout dans le fader (sémantique console E7.2),
		 * garde-crête −6 dBFS (crête ≈ rms_max, marge 3 dB) */
		float g_db = (-20.0f - m->rms_avg_db) + P->mix_db - 6.0f;
		if (m->peak_db + g_db > -6.0f)
			g_db = -6.0f - m->peak_db;
		float g = powf(10.0f, g_db / 20.0f);
		if (g < 0.02f) g = 0.02f;
		if (g > 4.0f)  g = 4.0f;
		pthread_mutex_lock(&g_st.target_lock);
		g_st.input_target[i] = g;
		/* gate : seuil = floor mesuré + 8 dB, plafonné rms − 10 */
		if (P->gate_on) {
			float thr = m->floor_db + 8.0f;
			if (thr > m->rms_avg_db - 10.0f)
				thr = m->rms_avg_db - 10.0f;
			if (thr < -80.0f) thr = -80.0f;
			exp_configure(i, 1, thr, P->gate_ratio, 2.0f,
				      150.0f, 40.0f, P->gate_hold);
		} else {
			exp_configure(i, 0, g_exp[i].thr_db, g_exp[i].ratio,
				      g_exp[i].atk_ms, g_exp[i].rel_ms,
				      g_exp[i].range_db, g_exp[i].hold_ms);
		}
		/* comp : preset de rôle (seuil relatif au niveau stagé) */
		if (P->comp_on)
			cmp_configure(i, 1, P->c_thr, P->c_ratio,
				      P->c_atk, P->c_rel, 0.0f);
		else
			cmp_configure(i, 0, g_cmp[i].thr_db, g_cmp[i].ratio,
				      g_cmp[i].atk_ms, g_cmp[i].rel_ms,
				      g_cmp[i].makeup_db);
		pthread_mutex_unlock(&g_st.target_lock);
	}
	atomic_store(&g_presets_dirty, 1);
	mlog("bandmix: mix calculé");
}


/* V12-AMX — calcul Dugan par bloc (appelé par audio_thread AVANT mix_block).
 * Énergie post-fader : e_i = mean(x²) × ig². Enveloppe asymétrique
 * (attack 10 ms, release 200 ms — parole). Cible : part d'énergie
 * pondérée, plancher automix_floor, non-membres ≡ 1.0. */
void automix_update(const float in_block[N_INPUT_REAL][PERIOD_FRAMES],
			   uint32_t N)
{
	if (!g_st.automix_on)
		return;
	const float ka = 1.0f - expf(-2.0f / 10.0f);    /* attack 10 ms/2 ms */
	const float kr = 1.0f - expf(-2.0f / 200.0f);   /* release 200 ms */
	float wsum = 0.0f;

	for (int i = 0; i < N_INPUT_REAL; i++) {
		if (!g_st.automix_member[i])
			continue;
		float acc = 0.0f;
		const float *x = in_block[i];
		for (uint32_t f = 0; f < N; f++)
			acc += x[f] * x[f];
		const float ig = g_st.input_gain[i];
		float e = (acc / (float)N) * ig * ig;
		if (g_st.mute_mask & (1u << i))
			e = 0.0f;
		float *env = &g_st.automix_env[i];
		*env += (e > *env ? ka : kr) * (e - *env);
		wsum += *env * g_st.automix_weight[i];
	}

	const float eps = 1e-12f;
	for (int i = 0; i < N_INPUT_REAL; i++) {
		if (!g_st.automix_member[i]) {
			g_st.automix_gtarget[i] = 1.0f;
			continue;
		}
		float share = (g_st.automix_env[i] * g_st.automix_weight[i] + eps)
			      / (wsum + eps * 8.0f);
		/* Dugan : atténuation en dB = 10·log10(part d'énergie) →
		 * multiplicateur d'AMPLITUDE = sqrt(part). 2 micros égaux =
		 * −3 dB chacun (NOM constant), conforme au standard. */
		float g = sqrtf(share);
		if (g < g_st.automix_floor)
			g = g_st.automix_floor;
		if (g > 1.0f)
			g = 1.0f;
		g_st.automix_gtarget[i] = g;
	}
}

/* V9.3 : mix_block — process N samples en 1 passe (vs mix_frame × N).
 *
 * Buffers in/out organisés par channel-major (in[ch][frame]) pour permettre
 * au compilo d'auto-vectoriser les boucles inner `for (f=0..N-1)` en NEON.
 *
 * Phases :
 *   A. Sends : in_block[26][N] × send_gain[26][8] → bus_in[8][N]
 *   B. FX    : fx_engines[b].process_block(bus_in, bus_out, N) × 4 bus
 *      → bus_out[8][N] (post-FX), puis × fx_bus_gain → ret_block[8][N]
 *   C. Master: (in_block + ret_block) × master_gain[34][18] → out_block[18][N]
 *
 * Tous les paramètres (input_gain, send_gain, master_gain, fx_bus_gain) sont
 * lus une fois en début de block (snapshot post-smooth_gains). Pour smooth
 * intra-block sur des changements rapides, voir TODO V9.4.
 */
/* V9.3.1 : buffers internes mix_block en static BSS (pas stack).
 * Appelée uniquement depuis audio_thread (1 thread), donc thread-safe sans lock.
 * Taille : 3 × N_BUS_FX_CH × PERIOD_FRAMES × 4 + N_RETURN_CH × PERIOD_FRAMES × 4
 *       = 3 × 8 × 96 × 4 + 8 × 96 × 4 = 12288 octets = 12 KB en BSS. */
