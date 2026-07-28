// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * looper — V12-LOOP-PRO : loopstation multipiste (RC-505).
 *
 * LOOP_TRACKS pistes indépendantes, chacune = 1 couche discrète : voie
 * source sélectionnable, mute/clear individuels. Horloge maître partagée
 * (g_master_len + g_lpos), posée par la 1re piste enregistrée ; les pistes
 * suivantes s'enregistrent alignées (un tour complet) → phase garantie.
 * Restitution additionnée dans P1/P2 (in_block[16/17], comme le sampleur).
 * Buffers alloués au démarrage, jamais en RT. memset au rec-arm d'une piste
 * VIDE (control thread, non lue par l'audio) → aucun glitch, pas de undo.
 * ARCHI_V12_LOOPER_PRO.md.
 *
 * Extraction V14.0 (étape 1, ARCHI_V14_RESTRUCTURATION.md) depuis
 * mixer-pro.c — code déplacé tel quel. g_tr et l'horloge restent exposés
 * tant que les ops (rec/play/mute/clear/status) vivent dans le control
 * socket ; elles migreront ici à l'étape 4.
 */
#ifndef MIXER_LOOPER_H
#define MIXER_LOOPER_H

#include <stdatomic.h>
#include <stdint.h>

#include "mixer-pro.h"   /* N_INPUT_REAL, PERIOD_FRAMES */

#define LOOP_TRACKS      6
#define LOOP_MAX_FRAMES  (40u * 48000u)   /* 40 s/piste — 6×40s stéréo = 88 MiB */

/* V13.2 : TR_ARMED = REC quantifié — la piste attend le prochain début de
 * boucle maître pour passer en REC (un tour exact puis PLAY, couture ≤ 1
 * période). Demande utilisateur 2026-07-10. */
enum { TR_EMPTY, TR_REC, TR_PLAY, TR_ARMED };
extern const char *const TR_NAMES[];

struct loop_track {
	float           *buf;        /* stéréo entrelacé LR, LOOP_MAX_FRAMES*2 */
	_Atomic uint32_t len;        /* frames (= master_len une fois posée), 0=vide */
	_Atomic int      state;      /* TR_EMPTY / TR_REC / TR_PLAY */
	_Atomic int      muted;      /* 1 = couche désactivée (conservée) */
	uint32_t         rec_head;   /* écriture piste maître (REC libre) */
	_Atomic uint32_t rec_start;  /* g_lpos capturé par l'audio au 1er bloc REC aligné */
	uint32_t         rec_done;   /* frames enregistrées ce tour (piste alignée) */
	int              src_a, src_b; /* voies source (-1 : mono → dup) */
	float            gain;
	_Atomic uint32_t peak;       /* crête VU (maj en lecture) */
};

extern struct loop_track g_tr[LOOP_TRACKS];
extern _Atomic uint32_t  g_master_len;  /* 0 tant qu'aucune piste posée */
extern _Atomic uint32_t  g_lpos;        /* position globale (frames) */
extern _Atomic int       g_loop_run;    /* transport global (0=stop, 1=play) */
extern _Atomic uint32_t  g_loop_mpeak;  /* crête master (somme des pistes) */
#define REC_START_NONE 0xFFFFFFFFu

/* Alloue les buffers pistes (main, AVANT les threads — jamais en RT).
 * Échec d'alloc → piste dégradée, loggé bruyamment. */
void loop_init(void);

/* Rendu (audio_thread, SOUS target_lock, après le convert S32→float) */
void loop_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES]);

#endif /* MIXER_LOOPER_H */
