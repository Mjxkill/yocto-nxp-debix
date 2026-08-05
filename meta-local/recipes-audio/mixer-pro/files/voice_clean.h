// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * voice_clean — V16 : nettoyage de la voix par le daemon voice-clean
 * (CPU3), bouton BRUT / DTLN / GTCRN / SPECSUB par voie voix.
 *
 * mixer-pro pousse [voix, réf musique] dans le ring SHM, le daemon
 * transforme, mixer-pro REMPLACE la voie par le retour traité (latence
 * fixe du pipeline, ~48 ms — assumée et jugée à l'oreille, R&D).
 * BRUT = bypass strict, zéro accès ring. Famine daemon = SILENCE sur la
 * voie + compteur (jamais du brut non traité d'un coup, jamais masqué).
 * Mode NON persisté : repart en BRUT au boot (invariant opt-in).
 * ARCHI_V16_VOICE_CLEAN.md.
 */
#ifndef MIXER_VOICE_CLEAN_H
#define MIXER_VOICE_CLEAN_H

#include <stdatomic.h>
#include <stdint.h>

#include "mixer-pro.h"    /* N_INPUT_REAL, PERIOD_FRAMES */

extern struct vc_state {
	_Atomic int src;               /* voie traitée (−1 = aucune) */
	_Atomic int mode;              /* VC_* (voice_clean_shm.h) */
	_Atomic uint32_t famines;      /* pops RX à vide (diag) */
	int ramp;                      /* fondu d'engagement (audio) */
	uint32_t rx_rd;                /* curseur RX privé (audio) */
	int primed;                    /* amorçage : attendre VC_PRIME périodes
	                                * de RX avant de consommer (marge de
	                                * jitter daemon = la latence FIXE de
	                                * l'ARCHI §2 — oubliée en v1, 86
	                                * famines mesurées 2026-08-05) */
	struct vc_shm *shm;            /* NULL si init KO */
} g_vc;

/* main, avant les threads : crée + mappe le segment SHM (owner) */
void vc_init(void);

/* audio_thread, SOUS target_lock, après exp_render : push voix+réf,
 * remplace la voie par le retour traité. mode BRUT = retour immédiat. */
void vc_process(float in_block[N_INPUT_REAL][PERIOD_FRAMES]);

/* ops control du module (dispatcher control.c) */
int voice_clean_handle_op(int fd, const char *line);

#endif /* MIXER_VOICE_CLEAN_H */
