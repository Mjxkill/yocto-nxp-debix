# TESTS V13-BANDMIX E1 — auto-mix musique (assistant groupe) + suivi live

Date : 2026-07-07 · ARCHI : ARCHI_V13_BANDMIX.md (validé critic, approved).
Demande : « je suis nul en son et je dois sonoriser mon groupe » — sur les
16 tranches (DSP IN + USB IN, stems WAV de test par l'USB).

## Implémentation

- **V13-COMP** : compresseur natif par tranche (prérequis USB — pas de
  DRC DSP sur ce chemin) : cmp_render() après exp_render (gate→comp),
  threshold/ratio/attack/release/makeup, même patron RT que le gate,
  ops set/get_comp partiels, persistance, off = zéro coût.
- **Mesure** : puissance par bloc + **EWMA τ≈3 s calculée par l'audio à
  500 Hz** (publiées en atomics). Le plan de contrôle lit la moyenne
  vraie — jamais un bloc instantané.
- **Rôles** (10 : lead/choir/kick/snare/drums/bass/guitar/keys/line/off)
  avec presets : cible de mix relative (voix = 0 dB), gate par rôle
  (dur batterie, doux voix), comp par rôle.
- **Soundcheck** : bandmix_measure 12 s par tranche → rms_avg/peak/floor.
- **CALCUL** : gain staging (rms → −20 dBFS, garde-crête −6), gate
  (floor + 8 dB, borné), comp du rôle, fader = staging + cible de rôle.
- **VERROUILLER** : capture 30 s des parts de loudness POST-fader
  (retouches utilisateur incluses) = la référence artistique.
- **LIVE (keeper)** : correcteur 1 Hz en boucle FERMÉE (la mesure inclut
  keeper_gain), parts courantes vs référence, zone morte ±1 dB, pas
  0,5 dB/tick, **butées ±3 dB**, slew audio τ 2 s, priorité voix lead,
  tranches en pause ignorées (silence pré-fader vs floor, repli rms−15
  pour les sources continues). keeper_gain[] multiplié dans la chaîne
  automix (jamais les faders), OFF → retour doux à 0 dB.
- **GUI native** : page AUTO MIX (nav 9 pages) — rôle ‹ › par tranche,
  MESURER avec progression, CALCULER / VERROUILLER / LIVE, barre keeper
  ±3 dB centrée par tranche. Persistance rôles + référence + live
  (parseur fgets ligne à ligne).

## 3 bugs d'asservissement trouvés/corrigés pendant la validation

1. **Référentiel silence** : loudness post-fader comparé au floor mesuré
   pré-fader → toutes les tranches « silencieuses », keeper inerte.
   Fix : lt_pre pré-fader + repli rms−15 pour sources continues.
2. **Boucle ouverte** : la mesure n'incluait pas keeper_gain → le
   correcteur ne voyait pas ses corrections → intégrateur aux butées et
   oscillations. Fix : boucle fermée.
3. **Aliasing 1 Hz** : lire 1 bloc de 2 ms/s = 0,2 % du signal → sources
   modulées (tremolo/battements) mesurées au hasard → dérive à
   l'équilibre. Fix : EWMA 3 s côté audio (500 Hz).
   (+ piège fscanf préfixe « bandmix » vs « bandmix_live » → parseur
   fgets ; et pièges PipeWire : WAV 7.1 upmixé, jouer en RAW avec
   --channel-map=aux0..aux7.)

## Validation board (groupe synthétique : basse 80 Hz batt., voix 440
tremolo, batterie bruit×tremolo 3 Hz sur U1/U2/U3)

| Test | Résultat |
|---|---|
| Mesures (12 s/stem) | basse −16,3/floor −17,5 · voix −15,6 · batterie −19,1 ✓ |
| CALCUL | faders −12,7/−10,4/−12,9 dB (maths cibles de rôle exactes), comps 4:1/3:1/2:1 posés ✓ |
| VERROUILLER (30 s) | ref_valid=1, parts capturées ✓ |
| Équilibre (mix normal, LIVE ON) | keeper **0.00 / 0.00 / 0.00** stable ✓ |
| Batterie **+6 dB** | keeper batterie → **−3,00 dB** (butée, bon sens), basse/voix **0.00**, stable 58 s, zéro pompage ✓ |
| Persistance (restart) | rôles + référence + live restaurés ✓ |
| RT | xrun 0 nouveau, prof_mix 391 µs ✓ |

## Reste

E2 : page web AUTO MIX (miroir), onglet DYNAMIQUE (comp) dans le drawer,
HPF auto par rôle (biquads TAC, chemin micros). V2 : classification NPU
des rôles (plus besoin de les déclarer), cibles par genre musical.

## Test utilisateur : EN ATTENTE (stems WAV réels — lire en RAW
--channel-map=aux0..aux7, sinon PipeWire remappe les canaux du WAV)
