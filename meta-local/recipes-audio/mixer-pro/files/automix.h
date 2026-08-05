// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * automix — le cerveau AUTOMIX LIVE : V12-AMX (Dugan) + V13-BANDMIX
 * (assistant auto-mix musique) + V13.6 EQ de placement par rôle.
 *
 * Soundcheck mesuré par tranche → calcul déterministe par RÔLE (gain
 * staging, gate, comp, fader de départ) → suivi live LENT (keeper ±3 dB,
 * parts de loudness vs référence verrouillée, priorité voix, balance auto
 * par quadrants, gate auto, solo v2). Le cerveau vit dans le plan de
 * contrôle (persistence_thread 1 Hz : bmx_tick) ; l'audio ne fait que
 * publier les mean-squares (g_ms_*) et appliquer les gains lissés.
 * ARCHI_V13_BANDMIX.md, ARCHI_V13.5_AUTOMIX_LIVE.md,
 * ARCHI_V13.6_AUTOMIX_COMP_EQ.md.
 *
 * INVARIANTS (gravés, violés par le passé — ne JAMAIS les réintroduire) :
 *   - pas de signal → aucun gain automatique ne bouge (tout est gated
 *     par l'activité act_ticks/al_ref, gel si pre < al_ref − freeze_db) ;
 *   - un reset ou une automation n'écrase JAMAIS un réglage opérateur ;
 *   - toute automation non validée à l'oreille est OFF par défaut
 *     (ex. solo_auto) ;
 *   - tous les tunables sont réglables en live (R&D), jamais en dur.
 *
 * Extraction V14.0 (étape 2, ARCHI_V14_RESTRUCTURATION.md §10.2) depuis
 * mixer-pro.c — code déplacé tel quel. g_bmx reste exposé pour les ops,
 * la persistance et le vfocus/vspat (rôles) ; migration ops : étape 4.
 */
#ifndef MIXER_AUTOMIX_H
#define MIXER_AUTOMIX_H

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#include "mixer-pro.h"    /* N_INPUT_REAL, PERIOD_FRAMES */
#include "dsp_bq.h"       /* struct eqx_bq */
#include "strip_dyn.h"    /* N_EXP_CH */

/* mean square par bloc (brut) + EWMA τ≈3 s, publiés par l'audio
 * (bits float dans un u32). Le contrôle utilise l'EWMA (anti-aliasing —
 * lire 1 bloc de 2 ms par seconde échantillonne 0,2 % du signal). */
extern _Atomic uint32_t g_ms_in[N_EXP_CH];
extern _Atomic uint32_t g_ms_avg[N_EXP_CH];
extern float            g_ms_sm[N_EXP_CH];   /* privé audio_thread */

static inline float bmx_ms(int i)
{
	union { float f; uint32_t u; } v =
		{ .u = atomic_load_explicit(&g_ms_in[i], memory_order_relaxed) };
	return v.f;
}
static inline float bmx_avg(int i)
{
	union { float f; uint32_t u; } v =
		{ .u = atomic_load_explicit(&g_ms_avg[i], memory_order_relaxed) };
	return v.f;
}

/* Rôles par tranche (assignés par l'opérateur, jamais devinés) */
enum { BR_OFF, BR_LEAD, BR_CHOIR, BR_KICK, BR_SNARE, BR_DRUMS,
       BR_BASS, BR_GUITAR, BR_KEYS, BR_LINE, BR_NROLES };
extern const char *const BR_NAMES[BR_NROLES];

/* presets par rôle : cible mix (dB rel. lead), gate, comp — lus aussi par
 * les ops (reset comp par rôle). */
extern const struct bmx_preset {
	float mix_db;
	int   gate_on;  float gate_ratio, gate_hold;
	int   comp_on;  float c_thr, c_ratio, c_atk, c_rel;
} BMX_P[BR_NROLES];

/* V13.6 EQ de placement — état exposé pour les ops (enable/disable + force
 * recalcul role_of=-1) ; redeviendra privé à l'étape 4. */
#define EQX_BQ 3
extern struct eqx_state {
	struct eqx_bq bq[2][N_EXP_CH][EQX_BQ];  /* double-buffer : bascule sans clic */
	_Atomic int   bank[N_EXP_CH];           /* banque active par voie */
	float st[N_EXP_CH][EQX_BQ][2];          /* états (audio, JAMAIS vidés en live) */
	int   role_of[N_EXP_CH];      /* rôle pour lequel les coefs sont calculés */
	_Atomic int on;
} g_eqx;

struct bmx_meas {
	int    done;
	float  rms_avg_db, peak_db, floor_db;
};

/* État complet de l'assistant — champs documentés un à un dans automix.c
 * (déplacé tel quel ; struct nommée bmx_state pour l'extern). */
struct bmx_state {
	int    role[N_EXP_CH];
	struct bmx_meas m[N_EXP_CH];
	/* soundcheck en cours (contrôle + audio) */
	_Atomic int meas_src;         /* -1 = aucune */
	struct timespec meas_t0;
	double acc_ms; uint32_t nblk_s;   /* échantillonné à 1 Hz (ticks) */
	float  peak_max, sm, minsm;
	int    warm;
	/* keeper live */
	int    live;
	int    ref_valid;
	int    autolive;                  /* V13.5 : automix continu */
	float  al_anchor;                 /* V13.5 : ancre loudness (voix), gelée */
	float  al_ref[N_EXP_CH];          /* V13.5 : peak-hold loudness pré-fader */
	float  risk[N_EXP_CH];            /* V13.8 : mémoire du risque (anti-blast reprise) */
	int    act_ticks[N_EXP_CH];       /* V13.9 : ticks consécutifs d'activité */
	float  ref_share[N_EXP_CH];       /* parts de puissance verrouillées */
	float  lt_ms[N_EXP_CH];           /* loudness long terme POST-fader (τ 10 s) */
	float  lt_pre[N_EXP_CH];          /* idem PRÉ-fader (détection silence) */
	float  kdb[N_EXP_CH];             /* correction courante dB */
	int    locking;                    /* capture de référence en cours */
	int    lock_ticks;
	double lock_acc[N_EXP_CH];
	/* V13.9 — tunables automix réglables en live (R&D, jamais en dur) */
	float  freeze_db;
	float  risk_decay;
	float  risk_margin;
	float  gate_db;                   /* GATE AUTO : seuil = al_ref − gate_db */
	/* V13.9 — BALANCE AUTO musique/voix/chœurs (quadrants) */
	int    balance_on;
	float  bal_lufs_tgt;              /* cible LUFS master (−14) */
	float  bal_e_tgt;                 /* cible écart voix lead−musique (+3 dB) */
	float  bal_c_tgt;                 /* cible écart chœurs−musique (+1,5 dB) */
	float  g_voice_db;
	float  g_choir_db;
	float  g_music_db;
	float  prog_peak;                 /* peak-hold loudness programme (gel) */
	int    bal_staged;                /* staging initial jusqu'au 1er lock */
	/* V15.2 — instantané du tick 1 Hz pour le staging 4 Hz (flags
	 * d'activité + puissances de groupe ; le fast tick ne recalcule
	 * jamais les EWMAs, il ne fait que bouger les gains) */
	int    st_hv, st_hm, st_hc, st_loud;
	float  st_Pv, st_Pm, st_Pc;
	/* V13.9 — SOLO manuel + détection auto (v2, base EWMA par voie) */
	int    solo_src;                  /* voie en solo (−1 = aucune) */
	int    solo_auto;
	int    solo_is_auto;
	int    solo_on_cnt, solo_off_cnt;
	float  solo_db;
	float  solo_base[N_EXP_CH];
};
extern struct bmx_state g_bmx;

/* --- API (noms historiques conservés — extraction pure) --- */

/* tick 1 Hz (persistence_thread) : soundcheck + lock + keeper + autolive +
 * balance quadrants + gate auto + solo + makeup LUFS (via g_mk). */
void bmx_tick(void);

/* CALCULER LE MIX : applique staging + gate + comp + faders (control
 * thread, sur soundcheck done ou op recalc). */
void bmx_calc(void);

/* V12-AMX — calcul Dugan par bloc (audio_thread, AVANT mix_block).
 * Énergie post-fader, enveloppe asymétrique 10/200 ms, plancher floor. */
void automix_update(const float in_block[N_INPUT_REAL][PERIOD_FRAMES],
		    uint32_t N);

/* V15.2 : staging balance 4 Hz (persistence loop, 250 ms) — actif
 * uniquement tant que bal_staged=0 ; capteur = LUFS momentané. */
void bmx_balance_fast(void);

/* V13.6 : EQ de placement par rôle (audio_thread, entre gate et comp) */
void eqx_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES]);
/* recalcul des coefs d'une tranche au changement de rôle (control thread) */
void eqx_config(int i, int role);


/* V14.0 étape 4 : ops control du module (dispatcher control.c).
 * Retourne 1 si l'op est traitée, 0 sinon. */
int automix_handle_op(int fd, const char *line);

#endif /* MIXER_AUTOMIX_H */
