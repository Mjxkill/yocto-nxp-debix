# ARCHI V12-SMP — Sampleur (page PADS)

Date : 2026-07-06 · Priorité 1 roadmap V12 (après automix).

## Principe

Lecteur d'échantillons natif dans mixer-pro : WAVs préchargés en RAM,
déclenchés depuis une page PADS tactile, lus dans les tranches **P1/P2
(src 16/17)** — mortes aujourd'hui (`skip_phone=1`, in_block[16/17]
memset 0) mais déjà présentes dans les banques GUI, routables par la
matrice et dotées d'un fader. Zéro changement de topologie/DSP/kernel.

## Moteur (mixer-pro)

- **Banque** : 16 slots. Répertoire `/var/lib/ala/samples/` scanné au
  démarrage (tri alphabétique → slot 0..15) + op `sampler_reload`.
- **Loader (control thread)** : parseur WAV minimal (RIFF fmt/data,
  PCM 16/24/32 et float32, mono→dup ou stéréo, 48 kHz EXIGÉ sinon rejet
  loggé — resampling = V2). Buffers float stéréo malloc control-thread.
- **Publication** : pointeurs de slot échangés sous target_lock (le hook
  audio lit dans la section verrouillée existante du cycle). Anciens
  buffers → liste de libération différée, purgée au reload suivant
  (jamais de free d'un buffer potentiellement en lecture).
- **Rendu (audio_thread)** : après la conversion S32→float, pour chaque
  slot `playing` : add buf[pos..pos+96] × gain dans in_block[16]/[17],
  avance pos, fin de sample → stop (one-shot ; loop = V2 avec le looper).
  Retrigger = repart à 0. Budget : quelques voix × 96 frames — trivial.
- **Ops** : `sampler_list` (slots : nom, durée, playing, pos),
  `sampler_trigger {slot, gain_db?}`, `sampler_stop {slot|-1=tous}`,
  `sampler_reload`. Pas de persistance d'état à part le répertoire
  lui-même (les WAVs SONT l'état).

## GUI (native d'abord, web E2)

- **Page PADS** (6ᵉ page, navbar) : grille 4×4, nom du fichier, pad ambré
  quand en lecture (poll sampler_list 2 Hz, page visible seulement —
  leçon N8), tap = trigger (TapHandler ReleaseWithinBounds), bouton
  STOP ALL. Le niveau/routage se règlent sur les tranches P1/P2 du
  mixer (sémantique console : le sampleur est une source comme une autre).
- E2 web : miroir + upload de WAVs (gui-http endpoint multipart) +
  renommage/couleurs.

## Tests E1

1. WAV 48 k généré scp → reload → list = slot visible.
2. trigger → signal visible sur meters src16/17 + audible via routage
   P1→OUT ; retrigger et stop OK ; fin de sample = arrêt propre.
3. Delta xruns 0 pendant lecture + trigger en rafale.
4. Fichier invalide (44.1 k) → rejet loggé, pas de crash.

## Invariants

- P1/P2 restent des tranches normales (fader/mute/routing/automix).
- Si le phone aloop est réactivé un jour (skip_phone=0), le sampleur
  S'ADDITIONNE au signal phone (documenté ; config exclusive = V2).
- Aucune allocation/IO dans audio_thread ; slots atomiques.
