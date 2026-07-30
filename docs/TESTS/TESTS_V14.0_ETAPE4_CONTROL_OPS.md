# TESTS V14.0 — Étape 4 : control socket éclaté, chaque module possède ses ops

Date : 2026-07-30. Architecture : `docs/ARCHI/ARCHI_V14_RESTRUCTURATION.md` §3.

## Ce qui a été fait (extraction pure : corps d'ops déplacés tels quels)

**4a (`733dfbc2`)** — 37 ops migrées de handle_cmd vers LEUR module :
sampler(4), looper(4), midix(3), strip_dyn(6), automix(13 → `automix_ops.c`,
fichier séparé pour la limite 1000 — même domaine, même .h), master(1),
voice(3), persist(3). Chaque module expose `int <mod>_handle_op(fd, line)`
(1 = traitée, 0 = pas à moi) ; dispatcher en tête de handle_cmd, noms
disjoints → ordre sans effet. Transformations mécaniques : `return;` →
`return 1;`, buffer statique local → `g_ctl_reply` partagé (control.h,
un seul thread control, inchangé depuis V9.3.3).

**4b (ce commit)** — `control.c` extrait (json helpers + handle_cmd cœur +
control_thread). Limite 1000 dépassée deux fois → deux nouvelles familles
extraites dans la foulée : `fx_ops.c` (14 ops fx bus/insert/assistant —
domaine effets, migrera avec l'étape 5) et `tac_ops.c` (4 ops amixer/i2c
TAC — règle « TAC statique » rappelée en tête).

Bilan : `mixer-pro.c` = **398 lignes** (globales transversales +
persistence_thread + signals + main). Cible ~450 du plan : atteinte.
**Plus aucun fichier du moteur > 1000 lignes** (effects.c 2389 = étape 5).

## Validation board (binaires `1da57907` 4a, `bb52bb10` 4b)

| Test | Résultat |
|---|---|
| Batterie 4a : 18 ops (1+ par module + cœur) | 18/18 ok ✓ |
| Batterie 4b : 22 ops (modules + fx + tac + cœur) | 21/22 ok + get_alsa « bad args » sans argument = contrat normal ✓ |
| get_alsa avec vrai contrôle (numid 3, ADC1 Fine Gain) | value 8 ✓ (« read failed » sur numid 314 = contrôle bytes/TLV, hors périmètre op) |
| Op inconnue | `unknown op` ✓ |
| GUI web /api/state via gui-http | 200 ✓ |
| Morceau : keepers actifs (kick +3.5, drums +5.0, line +13.0 en convergence), VU stems + out vivants, live/ref_valid/autolive 1/1/1 | ✓ |
| xrun pendant morceaux | 0 ✓ |
| Builds 4a et 4b | 0 erreur, 0 warning nouveau ✓ |

Note de lecture (2e occurrence du piège « passage calme ») : un
`bandmix_status` à t=20 s d'un morceau frais montre des keepers ≈ 0 —
c'est la CONVERGENCE depuis l'état persisté, pas une panne. Toujours
croiser avec les VU avant de conclure.

## Test utilisateur : EN ATTENTE (GUI web + LCD complets, écoute)

## Reste

Étape 5 : effects.c → effects/ (fx_ops.c y migrera). Étape 6 : gui-http.
Étape 7 : beta.html → HTML+CSS+JS. Étape 8 : QML. Passe 2 (optionnelle) :
re-privatiser les externs devenus inutiles après l'étape 4 (audit).
