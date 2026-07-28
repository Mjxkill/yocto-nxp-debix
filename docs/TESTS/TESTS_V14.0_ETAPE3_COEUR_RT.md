# TESTS V14.0 — Étape 3 restructuration : cœur RT (uac2_ring + audio_loop)

Date : 2026-07-29. Architecture : `docs/ARCHI/ARCHI_V14_RESTRUCTURATION.md`.
Zone la plus sensible du chantier — extraction pure prouvée par diff.

## Modules extraits

| Commit | Module | Lignes | Contenu |
|---|---|---:|---|
| `034486b5` | `uac2_ring.c/h` | 973+115 | rings SPSC cap/play + threads cap/play/shift + ASRC crossfade K=8 + régulation fill 4 paliers + drift ppm + timing V8.32 + histogrammes V9.1. `smooth_*_middle`/`compute_correction` restent static |
| (3b) | `audio_loop.c/h` | 803+35 | `audio_thread` (chaîne RT complète) + `play_thread` DSP ; `smooth_gains` et `mix_block` INTERNES (static) ; `g_out_gain_cur` exposé (init main) |

`mixer-pro.c` : 4 341 → **2 600** lignes (reste : state/globales, control
socket ~2 000, persistence_thread, main).

## Preuves spécifiques (au-delà de la batterie)

1. **Diff HEAD↔module** (uac2) : zéro différence fonctionnelle — seuls les
   defines/type déplacés vers le .h (valeurs identiques), `static` retirés.
2. **A/B binaire** : fills élevés observés (cap 384/288 vs cible 192) →
   suspicion de régression → rebuild du binaire HEAD (`f9e42e18`, reproduit
   bit-identique après cleansstate) → **même comportement** sur le même
   protocole (cap_fill 288, play 96, drops seuls, cap_full_evt qui grimpe).
   Verdict : état du flux host PipeWire (drift +21…35 ppm selon les runs),
   PAS une régression. Les fills 192/192 de l'étape 0 correspondaient à un
   autre état du stream host.

## Validation board (binaire `9db48a75`)

| Test | Résultat |
|---|---|
| Boot + services | v14.0-modules, frames avancent 48,8 k/s ✓ |
| Morceau : profiling RT | iter avg 1 963 µs, wr/rd avg 1 999 µs, iter_ge50 = 0 ✓ |
| xrun pendant morceau | 6 → 6 (transitoires boot uniquement, delta 0) ✓ |
| xruns UAC2 | cap 0 / play 0 ✓ |
| Chaîne complète en RT (passage actif) | keepers kick −3.3 / drums −9.1 / bass −8.5 / line −22.6 (lead ≈ 0 = ancre, par design), LUFS −16.5, cuts vfocus 1.21/1.23/0.83 ✓ |
| VU entrée/sortie | stems 8-15 vivants, out 0/1 ≈ −24 dBFS ✓ |

## Incidents/leçons (consignés)

- Fausse alerte « chaîne morte » : lecture faite sur un passage calme + je
  n'avais affiché QUE le keeper lead (≈0 par design d'ancre). Discriminé
  par les VU d'entrée puis contre-lecture sur passage actif. Leçon : pour
  juger l'automix, lire TOUS les keepers + un VU, jamais un seul champ.
- `bitbake` sans `source` d'environnement = faux « rien à refaire » +
  binaire stale — d'où l'A/B initialement trompeur (md5 identique).
  Toujours sourcer, toujours vérifier le md5 attendu CHANGE.

## Test utilisateur : EN ATTENTE (écoute)

## Reste

Étape 4 : `control.c` + ops distribuées vers les modules (mixer-pro.c
~2 600 → ~450 ; les externs temporaires redeviennent privés). Puis 5-8 :
effects/, gui-http, web JS/CSS, QML.
