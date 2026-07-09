# RAPPORT MARATHON 2 h « TOUT ACTIVÉ + VRAI SIGNAL » — 2026-07-09 (00:03 → 02:03)

**Branche** : `L6.6.36-2.1.0-debix_model_ab` · **Board** : 192.168.0.198
**Outillage** : `tools/validation/marathon/` (orchestrateur, compo, sondes, dépouillement)
**Demande utilisateur** : 2 h de son réel (USB multicanal + MIDI multi-canaux + compo
+ pads + looper), changements de paramètres continus, vérification des glitchs
pendant les changements, test de rapidité de l'anti-larsen, mastering vu du tap DSP.

## Verdict global

| Métrique | Résultat |
|---|---|
| xrun sur 2 h (mixer) | **0** (compteur 3 → 3, transitoires de boot uniquement) |
| drops ring USB sur 2 h | **0** |
| Changements de paramètres | **~400** (faders, EQ TAC, gates, comps, automix, vfocus, mastering, sends, patchs synthé, mutes, pads) |
| Vrais glitchs numériques dans la capture | **1 seul événement ambigu de 2 ms** (voir §Glitchs) |
| Anti-larsen (après réparation de l'injection) | **notch à 2402 Hz en 8,4 s de rampe 2 dB/s, les 2 canaux** |
| Fuites mémoire (soak 7,5 h parallèle) | **aucune** (RSS mixer constant au Ko près) |
| Restauration de l'état utilisateur | **intégrale, vérifiée** |

## Le contenu joué

- **Compo générée** (`compose.py` → `compo.mid`, ~2 min en boucle) : Am–F–C–G à
  100 BPM — piano arpégé, nappes cordes, basse, batterie GM, mélodie celesta
  (sections A/B), 5 canaux MIDI → synthé M1 (P1/P2), programmes changés en
  pleine lecture par le brasseur.
- **Stems** en rotation continue sur l'USB + **pads** aléatoires +
  **looper** : cycles rec 4 s → ~20 tours de lecture → clear toutes les 10 min.

## Glitchs — méthode et résultats

Capture continue de la sortie USB (8 ch, 32 bits, chunks 5 min), détecteur de
discontinuités (2ᵉ différence vs médiane locale 20 ms, seuil 60×), horodatage
et **corrélation ±1,2 s avec le journal des changements**, puis classification
par la forme d'onde (trou/marche/chute d'énergie = glitch ; montée = attaque
musicale).

- 480 détections brutes → 20 % alignées sur la grille de tempo (batterie),
  99 classées « onset musical », 282 « ambiguës » (transitoires dans des
  passages calmes — ratios 60-130, aspect continu), et **1 seul vrai trou** :
  **00:34:09.290, 96 échantillons figés = exactement 2,0 ms = 1 période**.
  Aucun compteur board n'a bougé à cet instant (xrun 3 constant, drops
  constants, aucun changement à ±7 s) → origine la plus probable : la chaîne
  de capture **côté PC** (pw-record), pas la table. Fichier conservé :
  `cap_1783550033.wav` (+40,73 s).
- Les changements corrélés (eq 14, comp 13, fader 7, gate 5, mastering 3…)
  sont tous dans la gamme des transitoires musicaux (ratio < 130), timing
  compatible avec le contenu (le brasseur agissait pendant la musique) —
  **aucun claquement de paramètre avéré**. À confirmer à l'oreille (checklist).

## Anti-larsen — l'enquête de la nuit

1. Les premiers bursts 2,4 kHz ne déclenchaient **jamais** de notch, alors que
   le daemon posait des notches sur les basses de la musique (82/199/223/293/
   328 Hz — toutes bin-centrées, ce qui a lancé une fausse piste « scalloping »
   jusqu'au critic, ANNULÉE ensuite par la validation empirique).
2. **Cause réelle : l'injection était muette.** PipeWire interprète les WAV
   8 ch en 7.1 et **downmixe aux2-7 vers aux0/1 à −80 dB** : seuls U1/U2
   recevaient du signal. Recette obligatoire (mémorisée) : **raw via stdin +
   `-P '{ stream.dont-remix = true }'` + `--channel-map aux0..7`**.
3. Injection réparée → burst réel : **NOTCH 2402 Hz (raie off-bin), 2 canaux,
   −9 dB, après 8,4 s** de rampe 2 dB/s (déclenchement dès le franchissement
   du seuil −45 dB + persistance ×4 ≈ 1,1 s — comportement nominal, le notch
   tombe bien avant que le larsen ne soit fort). Release 60 s OK.
   **anti-larsen.c est sain — aucun code modifié.**
4. **Vrai sujet restant** : faux positifs sur les basses synthé tenues
   (quasi-sinusoïdales, sans harmoniques → indistinguables d'un larsen par
   l'heuristique). Pendant la musique, le daemon posait des notches −9 dB
   Q30 à 82-328 Hz sur la sortie PA. Options à trancher ensemble :
   `f_min` configurable (ex. 150-200 Hz), critère de croissance renforcé
   sous 400 Hz, ou corrélation avec l'énergie micro.

## Mastering / TAP OUT

Le tap FX (`/dev/imx-audio-tap-out`, ring mmap magic NPAT, sonde `tap_rms.py`)
porte bien le master post-effets ; niveaux suivis toutes les 10 min, cohérents
avec l'état mastering journalisé. NB : le master n'y transite que si les
sources y sont routées (le marathon routait stems+compo vers out0/1 —
restauré ensuite).

## Découvertes/pièges consignés

- **PipeWire downmix silencieux** des canaux >2 vers ce sink (mémoire
  `pipewire-multichannel-gadget`) — invalide les niveaux « USB multicanal »
  de tout test antérieur à cette recette.
- Le trou unique de 2 ms a la taille exacte d'une période mixer/gadget —
  si un jour il se reproduit côté board, chercher du côté du ring USB
  (mais 0 drop compté cette nuit).
- Fréquences détectées par l'AFS pendant la musique : toutes bin-centrées
  (82,4 Hz E2 = bin 14,00 exact, etc.) — coïncidence des notes, pas un bug.

## Restes pour l'utilisateur (oreilles requises)

1. Écouter les 3 moments mastering ON/OFF du brasseur (01:24:18, 01:49:38…)
   dans les captures conservées si doute sur des clics de commutation.
2. Rejouer un cycle looper au casque : la couture de boucle n'a montré aucun
   trou dans la capture, mais l'oreille tranche.
3. Décider la politique anti-larsen sur les basses (options §Anti-larsen.4).
4. Le clic « orphelin » type (00:09:15, ratio 285) est une attaque musicale
   à l'analyse — écoute possible dans `cap_1783548533.wav` à +42 s.

## Artefacts

- Journal complet : `tools/validation/marathon/marathon_log.csv` (dans le
  dépôt) ; captures suspectes conservées : `/home/michael/marathon_capture/`
  (~24 fichiers de 5 min) ; soak 7,5 h : `soak_20260708.csv`.
