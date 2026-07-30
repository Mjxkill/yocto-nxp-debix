// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * control — socket de contrôle /run/mixer-pro.sock (JSON ligne par ligne).
 *
 * V14.0 étape 4 : chaque module POSSÈDE ses ops — il expose
 * `int <mod>_handle_op(int fd, const char *line)` qui traite l'op et
 * retourne 1, ou 0 si l'op ne lui appartient pas. Le dispatcher
 * (handle_cmd) essaie les modules puis les ops « cœur » (matrices, fx bus,
 * insert, meters, diag système). Les noms d'ops sont disjoints → l'ordre
 * des essais est sans effet. Protocole documenté : MIXER_PRO_REFERENCE.md.
 *
 * Un SEUL thread control (connexions séquentielles) → le buffer de réponse
 * est partagé et statique, comme depuis V9.3.3.
 */
#ifndef MIXER_CONTROL_H
#define MIXER_CONTROL_H

/* buffer de réponse partagé (un seul thread control). 48 Ko : get_fx avec
 * params + ranges (NPU, V9.3.3). Taille EXPLICITE : les handlers font
 * sizeof(g_ctl_reply). */
extern char g_ctl_reply[49152];

/* Helpers JSON minimalistes (pas un parser complet — `"key":<val>`).
 * Utilisés par tous les handlers d'ops. */
int json_get_str(const char *s, const char *key, char *out, int max);
int json_get_int(const char *s, const char *key, int *out);
int json_get_float(const char *s, const char *key, float *out);
int json_has_op(const char *s, const char *op);

#endif /* MIXER_CONTROL_H */
