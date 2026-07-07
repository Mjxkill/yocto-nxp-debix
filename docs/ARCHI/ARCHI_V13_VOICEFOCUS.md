# ARCHI V13-VFOCUS — « Place à la voix » (unmasking spectral sidechainé)

Date : 2026-07-07 · Demande : « que la musique soit creusée aux fréquences
de la voix pour que la voix ressorte ».

## Principe (dynamic EQ sidechainé, façon TrackSpacer — natif ici)

Quand la voix chante, on **creuse la musique de quelques dB uniquement
dans les bandes où la voix a de l'énergie à cet instant**. Voix muette →
la musique retrouve son spectre entier. Ni compression globale (pompage),
ni EQ statique (musique éteinte en permanence) : un creusement chirurgical
et mobile.

## Architecture (mixer-pro, audio_thread)

### Bandes fixes (présence vocale)
5 bandes peaking RBJ : 250 Hz, 500 Hz, 1 kHz, 2 kHz, 4 kHz (Q 1,4).

### Analyse (sidechain = tranches rôle VOIX)
- Source : somme des tranches BANDMIX rôle **lead** (+ choir optionnel),
  pondérée par leurs faders (post-fader = ce qui est audible).
- 5 filtres passe-bande (mêmes fréquences) sur ce signal → enveloppe par
  bande (attack 5 ms, release 180 ms, coefs précalculés).
- Activité voix : enveloppe large bande > seuil (−45 dBFS post-fader,
  ou floor bandmix + 10 si mesuré) — sinon cut = 0 partout.

### Creusement (appliqué aux tranches "musique")
- Cibles : tranches dont le rôle bandmix ∈ {kick, snare, drums, bass,
  guitar, keys, line} (les rôles pilotent tout — cohérent assistant).
- Par bande : cut_db = amount × part de l'énergie vocale dans la bande,
  borné à max_cut (défaut 4,5 dB, réglable 0–12). Lissage du cut
  (attack 10 ms / release 200 ms) — anti-chatter.
- LES MÊMES 5 gains pour toutes les tranches musique → les coefficients
  peaking sont calculés UNE fois par bloc (5 × RBJ), puis 5 biquads
  passés sur chaque tranche cible (état par tranche×bande).
- Position chaîne : après gate/comp, avant automix/mix — in-place sur
  in_block (le cut multiplicatif pré-fader ≡ post-fader au rendu).

### Coût RT
Analyse : 5 biquads sur 1 signal. Creusement : 5 biquads × ≤14 tranches
× 96 frames ≈ 34 k MAC / 2 ms — négligeable (le mix 34×18 en fait plus).
Recalcul coefs : 5 RBJ/bloc (quelques cos/sin) — négligeable. Zéro alloc.

## Ops / GUI

- `set_vfocus {on, amount(0-100), max_cut_db}` / `get_vfocus` →
  {on, amount, max_cut_db, active(voix détectée), cuts_db[5]}.
- GUI : page AUTO MIX, bandeau « PLACE À LA VOIX » : interrupteur,
  slider AMOUNT, 5 mini-barres de cut temps réel (on VOIT la musique
  s'écarter quand la voix chante). Persistance mixer_state (fgets).

## Tests E1 (stems USB, groupe synthétique + vraie voix si dispo)

1. Voix seule → cuts suivent le spectre voix ; musique seule → cuts 0.
2. Mix complet : pendant la voix, énergie de la musique dans les bandes
   500-2k baisse de ~amount ; hors voix, spectre musique intact (mesure
   analyzer FFT + écoute).
3. Pas de chatter sur voix hachée (release 200 ms).
4. Delta xruns 0, prof_mix mesuré avant/après.

## Écarté

- FFT/spectral gate : latence + coût injustifiés vs 5 biquads.
- EQ statique « carve » : éteint la musique même sans voix.
- Duck large bande sidechain : pompage, on ne creuse pas QUE les bandes utiles.
