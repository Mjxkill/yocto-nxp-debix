# A.L.A. — Liste complète des fonctionnalités (état 2026-07-08)

Console audio live A.L.A. (Audio Live Assistant) by Electrosens R&D.
Debix Model AB (i.MX 8M Plus) · toutes les fonctions validées board.

---

## 1. Cœur audio (mixer-pro)

- **Table 26 entrées × 18 sorties** : 8 micros (TAC5212 ×4 via SAI7/DSP),
  8 stems USB (gadget), 2 téléphone, 8 returns FX → 8 sorties analogiques,
  8 sorties USB, 2 téléphone. Blocs 2 ms, latence one-way qq ms, RT
  SCHED_FIFO sur cores isolés, mix NEON.
- **Matrice de routage complète** (26×18) + départs FX (26×8), faders
  lissés anti-zipper, mutes, trims de sortie par strip, remap des slots
  micros.
- **4 bus d'effets stéréo** : moteurs natifs ou **plugins LV2**
  (catalogue ~345 plugins, hosting complet worker/atom).
- **Insert mastering** sur le master (chaîne jusqu'à 8 plugins) avec
  **bypass instantané** (chaîne conservée chaude) et assistant de source
  (hw / passthrough / mastering).
- **Asservissement de dérive USB↔DSP** (shift ppm par feedback xrun).
- Meters complets (26 in / 18 out / 8 FX), analyseur FFT temps réel à
  points de mesure sélectionnables, profils de temps d'exécution.

## 2. Traitement par tranche (16 voies réelles)

- **Gate / expandeur** : threshold/ratio/attack/release/range/hold,
  enveloppe crête 2 ms, GR temps réel — l'outil « toms » et « micros
  de conférence ».
- **Compresseur** : threshold/ratio/attack/release/makeup — y compris
  sur les tranches USB (aucun DSP sur ce chemin).
- **Effets TAC5212 par voie** (chemin analogique) : biquads
  programmables (forme RBJ, éditeur intégré), volumes/fine gain,
  AGC/HPF côté ADC — via le drawer d'effets par tranche.
- **Effets DSP par canal** (chemin micros) : DRC et multiband DRC
  per-channel (firmware SOF patché), blobs édités depuis la GUI.

## 3. Automatismes

- **Automix Dugan (parole)** : partage de gain NOM-constant entre les
  micros membres (bouton « A » par tranche), response/floor/poids ±20 dB
  par tranche (priorité animateur), barre de gain auto sous chaque fader.
- **Auto-mix musique (assistant groupe)** : rôles par tranche (voix
  lead, chœurs, grosse caisse, caisse claire, batterie, basse, guitare,
  clavier, ligne) → **soundcheck guidé** 12 s/tranche → **calcul
  automatique** (gains d'entrée, gates sur bruit mesuré, compresseurs
  par rôle, mix de départ voix devant) → **verrouillage de l'équilibre**
  → **suivi live** (keeper ±3 dB, priorité voix, zéro pompage).
- **« Voix devant »** (unmasking spectral) : la musique est creusée
  uniquement dans les bandes où la voix chante, à l'instant où elle
  chante — 5 bandes dynamiques sidechainées, barres de cut visibles.
- **Anti-larsen** : détection FFT temps réel (seuil/PNR/persistance/
  non-harmonicité), notches RBJ posés automatiquement dans les biquads
  TAC, liste des notches en GUI, **activable/désactivable en direct**.
- **Mastering NPU** : chaîne d'inférence TFLite sur le tap post-effets
  (daemon dédié), enveloppes affichées.

## 4. Sources internes de performance

- **Sampleur (page PADS)** : 16 pads (WAV 48 kHz), déclenchement
  tactile, re-trigger, sortie sur les tranches P1/P2.
- **Loopstation (page LOOPER)** : 6 pistes indépendantes type RC-505 —
  la 1ʳᵉ piste fixe la boucle, les suivantes s'alignent ; source
  sélectionnable par piste (M1..USB8), **mute/clear par couche**, VU par
  piste (aussi pendant l'enregistrement) + master, transport global.
- **Expandeur MIDI (page EXPANDEUR)** : module de sons multi-timbral
  16 canaux sur le port **MIDI USB** (même câble que l'audio) —
  par canal : banque **GM complète** (GeneralUser GS, 128 programmes +
  kits) OU **synthé M1 maison** (2 oscillateurs PCM sur 324 multisamples,
  filtre VDF sans résonance, enveloppes ADBSSR 0-99, LFO, 16 voix,
  16 patches éditables **en direct** dans un éditeur plein écran,
  banque persistée). Vumètres d'activité MIDI par canal, volume, PANIC.

## 5. Scènes et pilotage (page SCÈNE)

- **6 profils complets nommés** (clavier tactile) : mixer + TAC/PGA +
  blobs DSP + patches synthé — **rappel instantané sans coupure audio**,
  confirmation 2 taps anti-fausse-manip.
- **4 gros boutons de pilotage** avec état, infos live et **vumètres
  signal réels** : ANTI-LARSEN (notches + micros), AUTOMIX (tri-état
  OFF/MUSIQUE/VOIX + correction), MASTERING (sorties L/R), VOIX DEVANT
  (creusement + voix). **Master L/R permanent** en tête de page.

## 6. Interfaces

- **Console native LCD** (Qt6/eglfs, écran DSI tactile, boot kiosque
  avec logo A.L.A. du kernel à l'app) : 10 pages — MIXER (bancs de
  tranches), EFFETS, MASTERING (+ spectre), PADS, LOOPER, EXPANDEUR,
  AUTO MIX, SCÈNE, ROUTING, SYSTÈME — + drawer d'effets par tranche
  (TAC + DSP + GATE) et panneau réglages Dugan.
- **Interface web** (`http://<console>:8080/beta`) : **parité totale**
  avec le LCD (11 onglets), pilotable depuis PC/tablette/téléphone.
- **API REST** (gui-http) + **protocole socket JSON** (67 ops) pour
  l'intégration.

## 7. Connectique et plateforme

- **USB gadget composite** vers le PC : carte son **8×8** S32 48 kHz +
  **port MIDI** in/out sur le même câble.
- Sorties analogiques via 4× TAC5212 (chaîne d'effets codec incluse),
  entrées micro avec contrôle complet du codec.
- **Tap NPU** post-effets (SHM) pour l'inférence embarquée ; firmware
  **SOF custom** (mode ASYNC, DMA 2 ms, pipelines 8 canaux, matrix
  16×8, taps).
- Yocto Scarthgap, kernel 6.6.36-rt (PREEMPT), cores 2-3 isolés pour
  l'audio, image bootable complète (`imx-image-full`).

## 8. Robustesse / exploitation

- Persistance complète de l'état (reboot-proof), reset usine, fiches de
  test par version (`docs/TESTS/`), documents d'architecture par phase
  (`docs/ARCHI/`), 0 xrun exigé à chaque validation.
- Services systemd supervisés, redémarrages isolés (un synthé qui meurt
  ne coupe jamais la console), journalisation diagnostique.

---

## En chantier / prochaines étapes notées

- Auto-sampleur (la console échantillonne un vieux synthé via MIDI out +
  ses entrées) → vrais multisamples pour le moteur M1.
- Classification NPU des rôles (plus besoin de les déclarer).
- MIDI DIN physique + MIDI téléphone, MIDI out/thru.
- Scènes : samples PADS liés, export/import USB.
- SAI RX priming (shift de slots au boot — workaround : restart mixer-pro).
- Sprint latence (PREEMPT_RT, MMAP, insert offload core 3).
