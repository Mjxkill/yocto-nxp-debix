# TESTS V13.9 — Refactor DRY lot 5b (parseurs, biquads RBJ, compresseurs)

Date : 2026-07-28. Exigence utilisateur : « des choses qui peuvent être
factorisées tu les dupliques… je veux que tout soit clean » — factorisation
maximale, merge avec subtilité, une extraction à la fois, vérifiée à chaque pas.

## Ce qui a été fait

| Duplication | Action | Nature |
|---|---|---|
| Parseurs jumeaux boot/scène (77 lignes identiques ×2, déjà modifiées 2× en parallèle le même jour) | Extraction `parse_state_lines(FILE*)` unique, appelée par `load_mixer_state` et `scene_apply` (verrouillage inchangé, chez les appelants) | Extraction pure (−160/+90 lignes) |
| `struct vf_bq` ≡ `struct eqx_bq` | Fusionnées : UN type de biquad RBJ dans le moteur | Renommage mécanique |
| Peaking RBJ dupliqué (`eqx_peak` vs `vf_peak_coefs`) | Noyau commun `rbj_peak_core(q, cw, alpha, A)` — sert le chemin config (cos/sin calculés) ET le chemin RT vfocus (précalculés) | Extraction pure (expressions identiques déplacées) |
| `meq` vs `eqx` | AUCUNE duplication réelle : meq réutilisait déjà `struct eqx_bq` + `eqx_peak` ; `meq_shelf`/`eqx_hpf` = types de filtres uniques | Constat (rien à merger) |
| `cmp_render` vs `effects.c comp_process_block` | PAS des doublons : algorithmes différents à dessein (crête/bloc + loi dB + rampe anti-zipper vs enveloppe/échantillon + loi linéaire). Fusion = changement du son validé des deux | Documentation croisée aux deux endroits |

## Validation board (mixer-pro md5 `85fe26996d4e…`)

| Test | Chemin factorisé prouvé | Résultat |
|---|---|---|
| T1 : gate_db 19 → restart → relu 19 | `load_mixer_state` → `parse_state_lines` | ✓ |
| T2 : scene_save(19) → set 25 → scene_recall → relu 19 | `scene_apply` → `parse_state_lines` | ✓ |
| T3 : master_eq mid −2.5 appliqué | `meq_compute` → `eqx_peak` → `rbj_peak_core` | ✓ |
| T4 : cuts vfocus 0.4 → 8.45 dB pendant chant, active=1 | `vf_peak_coefs` → `rbj_peak_core` (RT par bloc) | ✓ |
| Build | 0 erreur, 0 warning nouveau | ✓ |

## Test utilisateur : EN ATTENTE (écoute de confirmation — les extractions sont
mathématiquement identiques mais l'oreille reste le juge final du projet)

## Reste

- Écoute de confirmation utilisateur sur un morceau complet.
- La règle est gravée dans le skill methode-dev §5 : factorisation maximale,
  jamais « différée pour gain faible » — ce n'est pas ma décision.
