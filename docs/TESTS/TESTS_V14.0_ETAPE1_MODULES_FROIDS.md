# TESTS V14.0 — Étape 1 restructuration : modules froids

Date : 2026-07-28. Architecture : `docs/ARCHI/ARCHI_V14_RESTRUCTURATION.md`
(§10 ajouté : architecture détaillée étape 2). Un module = un build + un
test board + un commit (extraction pure, code déplacé tel quel).

## Modules extraits

| Commit | Module | Lignes | Contenu |
|---|---|---:|---|
| `c91fc07e` | `sampler.c/h` | 233+48 | V12-SMP : parseur WAV, scan, rendu P1/P2 |
| `fcc21f24` | `looper.c/h` | 188+66 | V12-LOOP-PRO : 6 pistes, horloge maître, `loop_init()` (ex-main) |
| (1c) | `midix.c/h` | 103+51 | V12-MIDIX : mmap ring SHM + rendu (struct anonyme → nommé pour l'extern, seule adaptation) |

`mixer-pro.c` : 6 760 → **6 255** lignes. Bloc V13.3 lien stéréo (imbriqué
dans la section looper) LAISSÉ en place → `strip_dyn.c` étape 2.

## Validation board (un cycle complet par module)

| Test | Résultat |
|---|---|
| 1a sampler (`0f439db0`) : boot scan | `smp: 1 samples chargés (750 Ko)` ✓ |
| 1a : sampler_trigger slot 0 → pos | playing=1, pos_s 1.0 après 1 s exactement, stop + reload OK ✓ |
| 1b looper (`6b36f6d2`) : rec maître 2 s → play | master_len_s 2.04, run=1, pos_s 0.02→1.06 en 1 s (wrap g_lpos), peak vivant en REC (58) ✓ |
| 1b : clear_all | retour empty, master_len 0 ✓ |
| 1c midix (`0cb08a29`) : boot | `midix: ring mappé (24576 frames)` ✓ |
| 1c : get_midix + set_midix 0.8→1.0 | present=1, gain suit, proxy daemon (status sf2) OK ✓ |
| Chaque build | 0 erreur, 0 warning nouveau ✓ |
| Delta xrun pendant chaque batterie | 0 (compteurs stables post-transitoires boot) ✓ |

## Incident de méthode (corrigé immédiatement)

Le premier retrait du bloc `smp_render` a avalé `loop_render` (commentaire
d'en-tête identique entre les deux rendus, ancrage sur premier match) →
détecté par comptage de lignes (181 vs ~33 attendues), **revert immédiat**
puis ré-extraction ancrée sur le nom de la fonction + garde-fous renforcés
(`count('static void') == 1`). Leçon appliquée aux retraits suivants.

## Test utilisateur : EN ATTENTE (écoute globale + pads/looper au tactile)

## Reste

- `persist.c` : déplacé en FIN d'étape 2 (archi §10.5 — évite le double
  déménagement des types g_bmx/g_vf/g_meq). **À valider par l'utilisateur.**
- Étape 2 : `strip_dyn.c`, `voice.c`, `automix.c`, `master.c` (archi §10).
