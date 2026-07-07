/* SPDX-License-Identifier: MIT
 * V12-SYNTH — moteur « M1-like » (AI Synthesis) de l'expandeur A.L.A.
 * 2 OSC PCM (multisamples SF2) → VDF LP sans résonance → VDA,
 * enveloppes ADBSSR, LFO, 16 voix. Voir ARCHI_V12_SYNTH_M1.md. */
#ifndef ALA_SYNTH_H
#define ALA_SYNTH_H

#include <stdio.h>
#include <stddef.h>

/* Init : parse la SF2 (multisamples) + charge la banque de patches.
 * Retour 0 = OK, <0 = moteur indisponible (GM seul). */
int  sy_init(const char *sf2_path);

/* Événement MIDI (thread driver MIDI) — file SPSC, jamais de blocage.
 * type = status & 0xF0 (0x80 note-off, 0x90 note-on, 0xB0 CC). */
void sy_midi(int type, int chan, int d1, int d2);

/* Le canal est-il assigné au moteur M1 ? (thread MIDI + ctl) */
int  sy_chan_is_m1(int chan);

/* Rendu : draine la file, avance les voix, ADDITIONNE dans buf
 * (stéréo entrelacé, frames × 2 floats). Thread de rendu uniquement. */
void sy_render_add(float *buf, int frames);

/* Commandes du socket de contrôle (thread ctl). Reconnaît :
 * engine / inst_list / patch_list / patch_get / patch_set / patch_save.
 * Retour 1 = commande traitée (réponse écrite dans out), 0 = inconnue. */
int  sy_ctl(const char *req, char *out, size_t outsz);

/* Fragment JSON pour status : ,"engines":[...],"patch":[...] */
int  sy_status_json(char *out, size_t outsz);

/* Persistance des assignations de canaux (fichier midix-chans.conf) */
void sy_save_chans(FILE *f);
int  sy_load_chan_line(const char *line);

#endif
