# TESTS V12-MIDIX E1 — Expandeur MIDI (module de sons)

Date : 2026-07-07 · ARCHI : ARCHI_V12_MIDI_EXPANDER.md (validé critic)
Feature 4 roadmap V12 (clarif. : « expandeur » = module de sons MIDI ;
le gate V12-EXP est conservé).

## Implémentation

- **Gadget USB** : fonction `midi.0` (f_midi, 1 in/1 out, id "A.L.A. MIDI")
  ajoutée au composite dans usb-uac2-setup.sh — le PC voit un port MIDI
  sur le MÊME câble que l'audio 8×8. CONFIG_USB_F_MIDI=y (zéro patch).
- **midi-expander** (nouvelle recette meta-local) : daemon libfluidsynth
  2.3.4 (meta-oe), SoundFont **GeneralUser GS** (32 Mo, GitHub officiel,
  sha256 dans la recette, seedé downloads/). MIDI in via driver fluid
  alsa_raw sur la carte f_midi (autodétection /proc/asound, retry 30 s).
  Rendu 96 frames/2 ms (clock_nanosleep absolu) → ring SHM /ala-midix
  (SPSC, 24576 frames ≈ 0,5 s, magic+widx atomic). Reverb/chorus off,
  poly 64, gain 0.5 (/etc/ala/midi-expander.conf). Service CPUAffinity=0 1
  (jamais les cores RT), Restart=on-failure.
- **mixer-pro** : midix_try_map() dans persistence_thread (1 Hz, mmap
  hors RT, invalidation si magic effacé) ; midix_render() après
  loop_render — pop non-bloquant, zéros+underrun si retard, resync si
  dérive > ring/2, addition dans P1/P2. Ops get_midix/set_midix (gain).
- Image : `midi-expander` dans imx-image-full (fluidsynth en RDEPENDS
  shlibs). soundfont-collection de meta-musicians écartée (recette
  skippée, URLs mortes) → SF2 embarqué dans la recette.

## Validation board (PC → amidi -p hw:1,0,0)

| Test | Résultat |
|---|---|
| Reboot : gadget composite | uac2.0 + midi.0 dans c.1, PC voit "Debix UAC2 8x8 MIDI 1" ✓ |
| Audio USB 8×8 après ajout f_midi | sink PipeWire intact ✓ |
| Boot chain | SF2 chargé 2 s, MIDI in hw:3,0, ring mappé par mixer-pro ✓ |
| Accord C maj (piano GM prog 0) | peak ring 1,3 % FS, P1/P2 = 1,2 % FS, retombe à 0 après note-off+release ✓ |
| Program change (C0 30 → cordes) | peak 6,8 % FS, timbre changé ✓ |
| Jeu soutenu (15 notes) | xrun mixer-pro 4 → 4 = **0**, underruns stables (4, transitoire boot) ✓ |
| systemctl stop midi-expander | present:0 (unmap propre via magic), **0 xrun**, silence ✓ |
| restart | present:1 auto (retry 1 Hz), son revient ✓ |

## Reste

E2 : MIDI DIN physique (UART+opto — matériel), MIDI téléphone, MIDI out/
thru, GUI (choix banque/programme/volume/VU), multi-timbral par canal.
V2 : synthé homemade (modélisation vieux synthé — souhait utilisateur).

## Test utilisateur : EN ATTENTE (clavier/DAW → écoute ; penser à monter
les faders P1/P2 et leur routage — config mixer à refaire post-fix persistance)
