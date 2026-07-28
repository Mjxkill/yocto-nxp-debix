// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * persist — sérialisation d'état + scènes (V13-SCENES).
 *
 * Un seul format de lignes d'état (« comp », « bandmix », « balance »,
 * « vspatial », « sends », « links »… — voir MIXER_PRO_REFERENCE.md), écrit
 * par save_state_to et relu par le parseur COMMUN parse_state_lines
 * (revue 2026-07-28, lot 5b) sur les DEUX chemins : boot (load_mixer_state)
 * et rappel de scène sans coupure audio (scene_apply). Les fichiers dédiés
 * (presets bus FX, mic map, out gains, master EQ) ont leurs save/load
 * propres. Déclencheur : g_presets_dirty (state.h), consommé par
 * persistence_thread (mixer-pro.c, 1 Hz).
 *
 * Extraction V14.0 (étape 2e, ARCHI_V14_RESTRUCTURATION.md §10.5) depuis
 * mixer-pro.c — code déplacé tel quel, en DERNIER de l'étape 2 : le
 * parseur touche g_bmx/g_vf/g_vspat/g_meq_p, extraits avant lui.
 */
#ifndef MIXER_PERSIST_H
#define MIXER_PERSIST_H

/* presets bus FX (fichier JSON dédié, 1 Hz si dirty) */
void save_presets(void);

/* fichiers dédiés : remap mics TDM, trims de sortie, EQ master */
void save_mic_map(void);
void load_mic_map(void);
void save_out_gain(void);
void load_out_gain(void);
void save_master_eq(void);
void load_master_eq(void);

/* état complet (matrices, strips, assistant, effets, insert…) */
void save_state_to(const char *path);   /* scènes : chemin arbitraire */
void save_mixer_state(void);            /* → MIXER_STATE_PATH */
void load_mixer_state(void);            /* boot (avant les threads) */

/* V13-SCENES : rappel de profil sans coupure audio (control thread) */
int scene_apply(const char *path);

#endif /* MIXER_PERSIST_H */
