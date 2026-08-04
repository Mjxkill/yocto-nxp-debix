# ARCHI V15 — ANTI-LARSEN v2 logiciel (notchs mixer-pro + preuve par la boucle)

Date : 2026-08-04. Statut : **PROPOSITION — en attente de validation Michael.**

## 1. Pourquoi une v2

Le v1 (daemon `anti-larsen.c`) détectait correctement mais ACTUAIT en
écrivant les biquads TAC pendant le live → plops (cause racine prouvée
2026-07-28, règle gravée : effets TAC statiques, dynamique en logiciel).
La v2 garde la détection, déplace les notchs dans mixer-pro où le
changement de coefficients sans clic est un pattern prouvé (meq crossfade
50 ms, eqx double-banque états conservés).

## 2. Les trois idées directrices (design Michael, 2026-08-04)

1. **Le larsen ne naît que dans des micros.** L'opérateur marque les voies
   « source larsen possible » (flag par tranche, comme les rôles — jamais
   deviné). La détection et les notchs ne concernent QUE ces voies.
   Typiquement : micros chant/instruments ouverts ; jamais les stems USB,
   la ligne, le sampleur.
2. **Preuve par la boucle (le cœur de la v2).** Quand une fréquence f
   suspecte apparaît : on pose le notch (= on coupe f dans la boucle) puis
   on regarde l'ENTRÉE micro (spectre pré-notch) :
   - f **disparaît** de l'entrée (après la décroissance de salle) →
     c'était du larsen (le micro n'entendait que la sono) → notch CONFIRMÉ ;
   - f **persiste** à l'entrée (une vraie source acoustique dans la salle :
     note tenue, cuivre, sifflet…) → PAS un larsen → **retrait immédiat**
     du notch (crossfade). Coût : un creux étroit de quelques centaines de
     ms sur cette fréquence — inaudible comparé à un larsen.
   L'heuristique spectrale (pureté, croissance) ne sert plus de VERDICT,
   seulement de DÉCLENCHEUR de la sonde. Le verdict est empirique.
3. **Un notch non confirmé se retire immédiatement ; un notch confirmé se
   libère lentement** (décroissance, retrait si pas de récidive) — la
   salle change, les notchs ne s'accumulent pas.

## 3. Découpage (règle V14 : un module par fonction)

### 3.1 Détection — daemon `anti-larsen` conservé (v1 éprouvé)

- Lit le tap NPU (/dev/imx-audio-tap-*) comme aujourd'hui : spectre de la
  SORTIE (candidates à la sonde) + spectres d'ENTRÉE des voies flaguées
  (verdict de la sonde). NB : le tap RAW donne l'entrée brute, le tap FX
  la sortie post-effets — les deux existent déjà (V7.0).
- N'écrit PLUS JAMAIS l'amixer. Actuation = ops socket mixer-pro.
- Machine d'états par fréquence : SUSPECT → SONDE (notch posé, fenêtre de
  verdict) → CONFIRMÉ (notch tenu + libération lente) ou INNOCENT (retrait
  immédiat + f blacklistée quelques secondes pour éviter le ré-essai en
  boucle sur une note tenue).

### 3.2 Actuation — module moteur `antilarsen.c/h` (mixer-pro)

- Banque de N notchs RBJ par voie FLAGUÉE, appliquée sur `in_block` entre
  gate et eqx (le larsen est tué à la source ; stems/ligne jamais touchés).
- Anti-plop : double-banque + crossfade (pattern meq), états jamais vidés.
- `dsp_bq` : ajout du design notch RBJ (rbj_notch, cookbook — même noyau).
- Ops (le module possède ses ops, pattern V14) :
  `larsen_flag {src,on}`, `larsen_notch {src,freq,depth}`,
  `larsen_release {src,freq}`, `larsen_status`, `larsen_cfg {…}`.
- OFF par défaut (invariant : automation non validée = opt-in) ; panneau
  GUI : voies flaguées, notchs actifs (freq/profondeur/état), sensibilité,
  fenêtre de verdict, profondeur max, libération. TOUT réglable (R&D).

## 4. Séquence type

1. Daemon : pic f à croissance rapide sur la sortie → candidates.
2. Daemon → mixer-pro : `larsen_notch` sur LES voies flaguées ACTIVES
   (act[] automix) — notch simultané = la boucle casse vite ; si le mix
   par-voie doit être raffiné, ré-ouverture une à une en tâche de fond
   pour ne garder que la voie coupable (option, pas dans la v2 minimale).
3. Fenêtre de verdict (~200–500 ms, réglable — dépend du RT60 de salle) :
   le daemon compare l'énergie à f dans les ENTRÉES flaguées avant/après.
4. Verdict : disparu → confirmé (log + GUI) ; persiste → retrait immédiat
   + blacklist temporaire de f.
5. Confirmé : libération lente (ex. profondeur −1 dB / 10 s, retrait
   complet si pas de récidive) — paramètres réglables.

## 5. Invariants

- Aucune écriture TAC, jamais (règle 2026-07-28).
- Voies non flaguées : chemin audio STRICTEMENT identique (off = zéro coût,
  pattern eqx/exp).
- Pas de signal → rien ne bouge (sonde uniquement si act[] et autolive).
- Un notch de sonde a une durée de vie BORNÉE : confirmé ou retiré, jamais
  « oublié posé ».
- Opérateur roi : flags par voie posés à la main, panneau de contrôle, OFF
  par défaut, kill-switch global.

## 6. Ce que ça coûte en RT

8 voies × 4 notchs × biquad = moins que l'eqx actuel (16×3). Le design de
coefficients à la pose = control thread (pattern eqx_config). Rien de
nouveau dans l'audio_thread hors une cascade conditionnelle déjà éprouvée.

## 7. Plan de validation (le larsen ne se simule pas en dataset)

1. Bench à vide : ops + notchs posés/retirés sans clic sur morceau dataset
   (on vérifie l'ABSENCE de dégradation, fiche + écoute).
2. Provocation réelle : micro + enceinte en re-bouclage physique, gain
   monté jusqu'à l'accrochage — mesurer : temps de kill, verdict correct
   sur note tenue (chanter/tenir une note dans le micro flagué), non-
   accumulation des notchs. Nécessite la présence de Michael (mesures +
   oreille).

## 8. Décisions restantes (Michael)

1. **N notchs max par voie** (v1 : 4) et profondeur max (dB) ?
2. **Fenêtre de verdict** par défaut (~300 ms ?) — réglable de toute façon.
3. La v2 minimale sonde TOUTES les voies flaguées actives en même temps ;
   le raffinement « trouver LA voie coupable » (ré-ouverture une à une) :
   dans la v2 ou plus tard ?
