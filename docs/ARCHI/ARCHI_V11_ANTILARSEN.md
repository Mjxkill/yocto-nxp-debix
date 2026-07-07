# ARCHI V11-AL — Anti-larsen automatique (AFS)

Date : 2026-07-05 · Validé utilisateur : phasage « anti-larsen classique
d'abord, sans NPU » (NPU réservé à une V2 anti-faux-positifs).

## Principe (méthode des processeurs pro — dbx AFS/Sabine)

Boucle de larsen : micro → mix → HP → micro. On détecte les raies de
larsen sur LE SIGNAL QUI PART AUX HP et on pose des notchs très étroits
dans le chemin de sortie physique.

## Placement dans NOTRE chaîne (décisions)

| Fonction | Où | Pourquoi |
|---|---|---|
| Signal analysé | **Tap FX** (`/dev/imx-audio-tap`, play post-effets 8ch S32) | C'est exactement ce qui alimente les HP ; déjà streamé en mmap ; lecteurs multiples OK (cursors indépendants, lecture seule) |
| Détection | **Daemon userspace `anti-larsen`** (C, cores 0-1, nice 10 — même modèle que ml-inference) | RT audio intouché ; FFT à cadence de contrôle |
| Notchs | **Biquads DAC du TAC5212** (12 par TAC, `TAC{0-3} DAC BQ1-12 Coefs`, i2c via amixer) | Dans le chemin de sortie physique, zéro charge DSP/CPU audio, chemin d'écriture éprouvé (drawer BIQUADS) |

Granularité : les BQ DAC sont PAR TAC (paire stéréo de sorties) — un notch
s'applique à la paire. Acceptable (les HP d'une zone sont sur la même paire).

## Arbitrage des 12 biquads

- **BQ 1-6 : utilisateur** (drawer BIQUADS, inchangé)
- **BQ 7-12 : réservés anti-larsen** (6 notchs par paire de sorties —
  standard du domaine : dbx AFS = 6-12 filtres)
- Le drawer affichera les slots 7-12 en lecture seule « AFS » quand le
  daemon est actif (E3, GUI).

## Détection (heuristique classique, PAS de ML)

FFT réelle 8192 @ 48 kHz (5,86 Hz/bin, fftwf déjà dans l'image) par paire
de sorties, décimée à ~10 Hz. Un bin est candidat larsen si TOUTES :
1. Magnitude > seuil absolu (−45 dBFS défaut)
2. **PNR** (peak-to-neighbour ratio, ±10 bins hors ±1) > 25 dB — raie pure
3. **Persistance/croissance** : présent sur ≥ 4 analyses consécutives avec
   magnitude non décroissante (~400 ms) — signature d'une boucle qui monte
4. **Non-harmonicité** : pas de partiel à 2f/3f de magnitude comparable
   (discrimine flûte/sifflet/voix tenue)

Action : notch RBJ Q=30 profondeur −9 dB sur le slot libre le plus ancien ;
si la raie persiste → approfondir par pas de −3 dB jusqu'à −18 dB ; si tous
les slots sont pris → réutiliser le plus ancien notch « dynamique ».
Libération : un notch dynamique inactif (raie disparue) depuis > 60 s est
retiré ; les N premiers notchs posés peuvent être promus « fixes »
(larsen structurels de la salle).

**Persistance (correction critic)** : les BQ TAC sont des contrôles BYTES
(vérifié board : `type=BYTES,values=20`) → alsactl ne les couvre PAS (même
trou que les blobs DSP, et c'est le reste connu « persistance RBJ
biquads »). Le daemon persiste donc SES notchs fixes lui-même
(`/var/lib/anti-larsen/state.json`) et les ré-applique à son démarrage —
service `After=ala-fx-restore.service` (donc après le tac-reset qui vide
les registres). Les notchs dynamiques sont volontairement transitoires.

## Codec RBJ → blob TAC (C)

Port du `rbjBlob` de stripfx.js (RBJ notch → coefs Q1.31 big-endian format
TI, ordre b0,b1,b2,a1,a2 précédé du header TAC) — implémentation C dans le
daemon, validée par comparaison octet à octet avec le JS (mires générées).

## Contrôle / observabilité (E1 minimal)

- Config `/etc/mixer-pro/anti-larsen.conf` : enable par paire de sorties,
  seuils, nb de notchs fixes/dynamiques.
- Statut JSON par socket UNIX `/run/anti-larsen.sock` (liste des notchs :
  freq, profondeur, âge, slot) — la GUI (E3) et le web liront ça.
- Journal systemd : chaque pose/approfondissement/libération loggé.
- La carte ALSA est résolue PAR NOM (l'index bouge entre boots — constaté
  3→4 ce soir).

## Coordination des écritures BQ (correction critic)

Les écritures ALSA cset sont atomiques par contrôle et sérialisées par le
kernel (i2c) — pas de race technique entre daemon et GUI. Le conflit est
SÉMANTIQUE : partition stricte des slots (1-6 utilisateur / 7-12 AFS),
documentée en E1, matérialisée en E3 (drawer : slots 7-12 affichés
« AFS », verrouillés quand le daemon est actif).

## Affinité CPU (arbitrage critic)

Le critic suggérait le core 3 : REFUSÉ — les cores 2-3 sont isolés pour
mixer-pro RT (invariant projet : jamais de non-RT dessus). Le daemon va
sur le core 0 épinglé, nice 10, exactement comme ml-inference. La charge
est bornée par construction (FFT décimée 10 Hz) et MESURÉE en E1
(avant/après, delta xruns + charge core 0).

## Étapes

- **E0 (préalables empiriques — E1 ne démarre pas si E0 échoue)** :
  (a) valider H1 : signal joué sur une sortie → raies visibles dans la
  FFT du tap FX (script /root/tests/al_tap_fft.py) ;
  (b) valider H2 : écrire un notch BQ TAC pendant lecture musicale →
  aucun plop/artefact audible (soft-step TAC) ;
  (c) valider le FORMAT des coefficients : en Q1.31 strict, D1 d'un notch
  grave (≈1,98) sature — beaucoup de codecs TI stockent b1/a1 divisés
  par 2. Test : notch 1 kHz posé (variante pleine + variante /2, script
  /root/tests/al_notch.py), tone 1 kHz joué → l'atténuation entendue
  tranche le format. Datasheet à consulter en parallèle.
  Fallback si E0 échoue : le daemon restera désactivé (enable=0 défaut
  image) et les slots 7-12 rendus flat — aucun risque résiduel.
- **E1** : daemon détection + notchs, config statique, statut socket.
  Test : larsen provoqué volontairement (micro devant HP) → suppression
  < 1 s, musique non dégradée (écoute), CPU daemon < 5 % d'un core,
  0 xrun supplémentaire.
- **E2** : promotion fixes/dynamiques + persistance + réglages fins.
- **E3** : GUI (page ou section SYSTÈME) : on/off, liste des notchs
  en temps réel, bouton « geler », slots AFS visibles dans le drawer.

## Invariants

- AUCUNE modification mixer-pro/SOF/kernel — uniquement un daemon + amixer.
- Cores audio 2-3 jamais touchés ; daemon épinglé core 0, nice 10.
- FFT décimée (leçon capture-stall : la charge CPU core 0 doit rester
  bornée — mesure obligatoire avant/après).
- Un utilisateur peut tout désactiver (enable=0) → les slots 7-12 sont
  rendus (flat) immédiatement.

## Risques / limites connus

- Notch par PAIRE de sorties (pas par canal seul) — limitation TAC.
- Si l'utilisateur occupe les BQ 7-12 manuellement aujourd'hui, ils seront
  écrasés quand l'AFS est actif (documenté, GUI E3 le montrera).
- Latence de détection ~400 ms par conception (anti-faux-positifs) ;
  un larsen déjà fort est notché en < 1 s.
