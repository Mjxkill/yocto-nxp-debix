# TESTS V15.1 — Démarrage de la balance auto (fix « trop fort puis −21 »)

Date : 2026-08-05. Constat Michael (écoute KatWright, balance validée par
ailleurs) : début trop fort ~5 s, puis plongée à −21 LUFS, puis la voix
entre et tout est bon. Exigence : corriger le début SANS toucher au régime
établi. `MIXER_VERSION` = v15.1-balance-startup.

## Cause racine (MESURÉE avant correction, trajectoire 1 Hz)

1. Au réarmement, les gains de groupe héritaient de la FIN du morceau
   précédent (+36 dB d'outro calme) ;
2. la boucle balance exige voix ET musique actives (hv&&hm) → gelée
   pendant toute l'intro instrumentale (10 s à −10 LUFS, trop fort) ;
3. la voix entre → staging 8 dB/s constant avec un mètre LUFS short-term
   qui traîne ~3 s → sur-correction mesurée à −21 LUFS.

## Correctif (3 points, validés Michael — uniquement réarmement + staging)

1. Réarmement autolive : gains de groupe → 0 dB (remplace la décision
   V13.9 « gains conservés », documenté dans le code) ;
2. Intro instrumentale : DESCENTE SEULE vers la cible LUFS (jamais de
   montée → règles silence/gel intactes) ;
3. Staging : pas proportionnel à l'erreur (8→1 dB/s en approche) —
   atterrissage doux ; vitesses post-lock 3/1 dB/s INCHANGÉES.

## Validation (binaire `e0dfc347`, les DEUX cas exigés)

**Cas A — intro instrumentale (KatWright, le cas du constat)** :

| avant (mesuré) | après (mesuré) |
|---|---|
| t1-10 : musique +36 figée, LUFS monte à **−10** (trop fort) | t1-10 : groupes 0 dB, LUFS −33 (niveau naturel des stems, jamais fort) |
| t11-14 : chute 36→12 à 8 dB/s | montée douce dès la voix, pas 8→4.3→2 (proportionnel) |
| t15-22 : plongée à **−21 LUFS** puis remontée | AUCUN creux — approche monotone, stabilisation −15/−16 |

**Cas B — intro chantée (LeftoverSalmon_RiversRisin, contre-test)** :
LUFS sain dès t=2 (−17), convergence douce, régime établi PARFAIT :
LUFS final −13/−14 (cible −14), écart voix−musique final **+3.0 dB
exactement** (= bal_e_tgt) — preuve que les boucles post-lock sont
intactes. xrun 0 sur les deux runs.

Note : SunshineGarciaBand écarté du contre-test (absent du manifest —
règle : pas de morceau sans rôles vérifiés).

## Test utilisateur : EN ATTENTE (écoute du début des deux morceaux)
