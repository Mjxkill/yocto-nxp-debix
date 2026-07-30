# TESTS V14.0 — Étape 5 : effects.c → 6 modules par famille

Date : 2026-07-30. Architecture : `docs/ARCHI/ARCHI_V14_RESTRUCTURATION.md` §4.
`effects.c` (2 389 lignes) supprimé, remplacé par 6 modules + 2 headers
internes. **API `effects.h` inchangée** → aucun appelant modifié.

## Modules

| Fichier | Lignes | Contenu |
|---|---:|---|
| `fx_dynamics.c` | 240 | compressor natif + limiter natif (note anti-fusion strip_dyn reprise) |
| `fx_space.c` | 241 | reverb Schroeder + delay stéréo |
| `fx_eq.c` | 598 | EQ 3 bandes + para_eq_x16 (ML) + enveloppe spectrale FIR + exciter |
| `fx_chain.c` | 213 | passthrough + cascade V9.4 + `fx_free` (branche lv2 → `lv2_free_state`) |
| `lv2_host.c` | 526 | monde lilv, features urid/options, workers, process/params/cleanup |
| `lv2_load.c` | 511 | fx_init_lv2 (instanciation + métadonnées UI V9.5.21) + catalogue |
| `fx_internal.h` | 13 | CLAMP/DB2LIN partagés |
| `fx_lv2.h` | 190 | interface PRIVÉE host↔load↔chain (structs lv2, globals, protos) |

Adaptations (seules) : symboles partagés host↔load dé-statiqués et déclarés
dans `fx_lv2.h` ; branche lv2 de `fx_free` devenue `lv2_free_state()`
(corps identique).

## Validation board (binaire `1c9fa56d`)

| Test | Résultat |
|---|---|
| Build | 0 erreur, 0 warning nouveau ✓ |
| 4 moteurs natifs bus : get_fx 0-3 | compressor/reverb/delay/eq états complets ✓ |
| set_fx_param round-trip (reverb wet 0.45 → relu 0.450 → restore 0.5) | ✓ (mes 2 premiers essais « bad args » = mauvais nom de champ dans MON test — le champ est `param`, contrat inchangé) |
| Insert mastering au boot | chaîne 3 étages restaurée, active:true (fx_chain + spectral_env + lv2_load) ✓ |
| Catalogue LV2 | 345 plugins listés (monde lilv OK) ✓ |
| Morceau avec insert actif | **LUFS −14.17 tenu**, xrun 0, iter_ge50 = 0, xruns UAC2 0/0 ✓ |

## Incident de méthode (consigné)

La coupe host/load ancrée sur « dernier commentaire avant fx_init_lv2 » est
tombée dans un commentaire INTERNE de lv2_get_state (pas de bannière avant
la fonction) → 2 fichiers invalides, détectés au gcc host, régénérés avec
coupe sur la ligne de définition + assert d'équilibre des accolades.

## Test utilisateur : EN ATTENTE (écoute effets bus + mastering)

## Reste

Étape 6 : mixer-gui-http.c (1 654) → 5 modules. Étape 7 : beta.html →
HTML+CSS+JS. Étape 8 : StripFxDrawer.qml. Puis passe 2 : audit de
re-privatisation des externs devenus inutiles (fx_ops → domaine effets).
