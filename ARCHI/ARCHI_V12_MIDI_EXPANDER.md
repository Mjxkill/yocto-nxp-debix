# ARCHI V12-MIDIX — Expandeur MIDI (module de sons)

Date : 2026-07-06 · Feature 4 roadmap V12 (clarification utilisateur :
« expandeur » = module de sons MIDI, pas seulement le gate de dynamique —
le gate V12-EXP est conservé).

## Vision utilisateur

- 3 accès MIDI in/out à terme : **gadget USB** (PC/DAW), **téléphone**,
  **DIN physique** sur la table.
- Sons : **SoundFont pour l'instant** (« ça fera l'affaire »), puis
  synthétiseur homemade (modélisation d'un vieux synthé) en V2.

## Périmètre E1

MIDI **in gadget USB uniquement** → moteur SoundFont → tranches P1/P2.
DIN physique (UART) et téléphone = E2+ (matériel/protocole à définir).
MIDI out = E2 (routing/thru).

## Architecture — daemon séparé + ring SHM (pattern mixer-ml-inference)

```
PC/DAW ── câble USB actuel ──> f_midi (gadget) ──> /dev/snd/midiC?D0
                                                        │
                              midi-expander (daemon, cores 0-1, Nice)
                              libfluidsynth 2.3.4 + GeneralUser GS.sf2
                                                        │ rend 96 frames stéréo float
                              SHM ring /dev/shm/ala-midix (lock-free SPSC)
                                                        │
                              mixer-pro audio_thread : pop 96 frames → P1/P2 +=
```

### Pourquoi un daemon séparé (et pas libfluidsynth dans mixer-pro)

- fluidsynth tire **glib** + ses propres threads → rien de tout ça dans le
  process RT (mixer-pro est pinné cores 2-3 isolés ; les threads fluid
  hériteraient de l'affinité RT — inacceptable).
- Isolation crash : synthé planté ≠ console muette.
- Même pattern éprouvé que mixer-ml-inference (SHM producer → RT consumer).

### Gadget USB — ajout f_midi au composite

`usb-uac2-setup.sh` : `functions/midi.0` (id "A.L.A. MIDI", 1 in/1 out)
linké dans c.1 AVANT le bind UDC. CONFIG_USB_F_MIDI=y (built-in, zéro
patch kernel). ⚠ Changement de descripteurs → le PC ré-énumère au reboot ;
la fonction uac2.0 n'est PAS touchée (mêmes paramètres). Risque géré :
test immédiat du flux audio USB après reboot, revert simple (retrait du
symlink) si régression.

### Ring SHM (SPSC, même discipline que les rings UAC2)

Header : magic, write_idx (frames, atomic), ring_size. Data : float
stéréo entrelacé, 256 périodes (≈ 0,5 s). Producer = midi-expander
(self-paced sur horloge CLOCK_MONOTONIC 2 ms, rend PERIOD_FRAMES à
l'avance) ; consumer = audio_thread mixer-pro (pop non-bloquant : si
retard producer → zéros, compteur underrun, JAMAIS d'attente RT).
Cursor privé consumer (pattern NPU tap inversé).

### midi-expander daemon

- fluid_settings : synth.sample-rate 48000, synth.polyphony 64,
  synth.reverb/chorus off en E1 (CPU + la console a ses FX), gain 0.5.
- MIDI in : driver fluid `alsa_raw` sur le port f_midi (autodétection de
  la carte "f_midi" via /proc/asound). Program change/CC gérés par fluid.
- Rendu : fluid_synth_write_float par blocs de 96 dans le ring, thread
  cadencé 2 ms (pas de callback audio — on EST la clock).
- Config : /etc/ala/midi-expander.conf (sf2_path, gain, polyphony).
  SF2 par défaut : GeneralUser GS (soundfont-collection meta-musicians).
- Service systemd : CPUAffinity=0 1, Nice=5, After=usb-uac2-gadget.

### mixer-pro (modification minimale)

`midix_render(in_block)` après loop_render : pop 96 frames du ring SHM
(mmap au démarrage, re-tentative périodique si absent), in_block[16/17]
+= × gain. Op `get_midix` (présence, underruns, peak) pour la GUI. Aucun
changement de structure — P1/P2 additionnent déjà sampleur + looper.

## Image / recettes

- `fluidsynth` (meta-openembedded 2.3.4) + `soundfont-collection`
  (meta-musicians, GeneralUser GS) dans imx-image-full.
- Nouvelle recette `midi-expander_1.0.bb` (meta-local/recipes-audio).
- `usb-uac2-gadget` : bump script (midi.0).

## Tests E1

1. Reboot → PC voit le port MIDI ("A.L.A. MIDI") ET l'audio UAC2 8×8
   intact (cap+play, xruns = comportement V8.33).
2. `aplaymidi`/DAW depuis le PC : notes → son sur P1/P2 (meters + écoute),
   program change change le son.
3. Latence touche→son < 15 ms (mesure tap).
4. Kill midi-expander → console silencieuse sur P1/P2 mais AUCUN xrun
   mixer-pro (zéros). Restart → son revient.
5. Delta xruns mixer-pro = 0 pendant jeu soutenu (accords, sustain).

## V2+ (hors E1)

MIDI DIN physique (UART + optocoupleur — matériel), MIDI téléphone,
MIDI out/thru/routing, GUI page EXPANDEUR (banque/programme/volume/VU),
synthé homemade (modélisation vieux synthé), multi-timbral par canal MIDI.
