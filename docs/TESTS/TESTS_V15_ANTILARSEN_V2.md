# TESTS V15 — Anti-larsen v2 : notchs logiciels + preuve par la boucle

Date : 2026-08-05. Architecture : `docs/ARCHI/ARCHI_V15_ANTILARSEN_V2.md`
(validée : 4 notchs/voie, sonde −12 dB / plafond −24 / Q 8, verdict 300 ms
réglable, fonctionnalité COMPLÈTE y compris identification du coupable).
`MIXER_VERSION` = v15.0-antilarsen.

## Livré (les 3 étages, complets)

1. **Moteur — `antilarsen.c/h`** (mixer-pro, 268+62 l.) : banque 4 notchs
   RBJ/voie flaguée (eqx_peak gain négatif Q8), double-banque + bascule
   atomique états conservés (pattern eqx), rendu entre gate et EQ placement,
   off = zéro coût. Ops du module : larsen_enable/flag/notch/release/cfg/
   status. Flags + réglages persistés (ligne « larsen »), notchs jamais.
2. **Daemon — `anti-larsen.c` v2** (réécrit, 662 l.) : double tap NPU
   (sortie = candidates via heuristique v1 conservée en DÉCLENCHEUR ;
   entrées micros = VERDICTS). Machine d'états : SONDE (notch broadcast
   voies flaguées) → verdict 300 ms (f morte à l'entrée = larsen confirmé ;
   f persiste = note tenue → retrait immédiat + blacklist 10 s) → REFINE
   (ré-ouverture voie par voie, celle qui fait repartir f garde son notch)
   → récidive = approfondissement −3 dB → libération après 60 s calmes.
   PLUS AUCUN accès TAC/ALSA (dépendance alsa-lib retirée de la recette).
3. **GUI web (page SYSTÈME)** : chips M1–M8 (flags opérateur →
   larsen_flag), notchs moteur en direct, état machine du daemon
   (SONDE xxx Hz), toggle enable existant conservé (carte SCÈNE).

## Validation board (mixer-pro + daemon + GUI déployés)

| Test | Résultat |
|---|---|
| Ops moteur : enable, flag ×2, notch broadcast | posed=2 sur les 2 flaguées ✓ |
| Notch sur voie NON flaguée | refused=1 (jamais silencieux) ✓ |
| Release ciblé (voie 0, 1200 Hz) puis global | removed=1 puis 1 ✓ |
| larsen_cfg q 10→8 round-trip | ✓ |
| Persistance : restart → enable + flags relus par le moteur | ✓ |
| **5 cycles pose/retrait notch −18 dB pendant morceau** | **delta xrun = 0** (4→4) ✓ |
| Daemon V15 : boot, sync enable moteur, status étendu (state/probe_hz) | ✓ |
| Conf embarquée : enable=0 (opt-in), service V15, deps nettoyées | ✓ |
| GUI : panneau rendu, clic M3 → flag moteur =1 vérifié par op, 0 erreur console | ✓ |

## Incident de campagne (résolu, consigné)

xruns continus (+20/10 s) pendant les tests, TOUS scénarios (avec/sans
notch, console arrêtée, daemon arrêté) → PAS la V15 : état de carte
dégradé après 14 jours d'uptime + churn de déploiements (load 4.9).
**Reboot → delta xrun 0 avec la pile V15 complète active**, re-prouvé par
les 5 cycles notch. Leçon : sur delta xrun anormal, vérifier l'uptime/load
AVANT d'accuser le code du jour.

## Test utilisateur : REQUIS (le larsen ne se simule pas)

Protocole (ARCHI §7, présence Michael) :
1. Flaguer les micros ouverts (chips M1.. page SYSTÈME), activer
   (carte SCÈNE ou chip enable), morceau dataset : vérifier zéro fausse
   sonde et zéro dégradation à l'oreille.
2. Micro + enceinte en re-bouclage : monter le gain jusqu'à l'accrochage —
   mesurer temps de kill, notch confirmé sur la bonne voie (page SYSTÈME).
3. Chanter/tenir une note dans un micro flagué : vérifier verdict INNOCENT
   (retrait immédiat, blacklist) — l'oreille juge le creux transitoire.
4. Récidive : re-provoquer à la même fréquence → approfondissement ;
   attendre 60 s calmes → libération.
