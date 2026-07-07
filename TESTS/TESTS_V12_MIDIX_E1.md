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

## E2-GUI (fait 2026-07-07) — page EXPANDEUR + multi-timbral

- **midi-expander** : socket de contrôle /run/midi-expander.sock (thread
  dédié, protocole texte status/prog/gain/panic → réponse JSON),
  persistance des programmes par canal /var/lib/ala/midix-chans.conf
  (débounce 2 s), restaurée au boot du daemon.
- **mixer-pro** : op proxy `midix_ctl` (control thread, timeout 500 ms,
  échec immédiat si daemon absent).
- **Console** : nouvelle page **EXPANDEUR** (nav 8 pages) — en-tête
  (statut/SF2, VU, volume synthé, PANIC) + **16 lignes canal MIDI** dans
  un Flickable : sélecteur de programme ‹ › avec noms GM, CH 10 =
  batterie. Poll status 1 Hz (proxy) + VU 4 Hz (get_midix direct).
- **Fix page LOOPER** : les 6 pistes scrollent (Flickable) — avant,
  seules 4-5 étaient visibles.

| Test E2 | Résultat |
|---|---|
| status/prog/gain/panic via proxy | tous ok, status reflète l'état réel fluid ✓ |
| Multi-timbral CH1 piano + CH2 cordes (0x90/0x91) | 2 timbres simultanés, peak 8,2 % FS ✓ |
| Persistance (restart daemon) | gain 0.8 + CH2=48 restaurés à l'identique ✓ |
| QML | 0 erreur (PageExpander + PageLooper scroll) ✓ |

## Reste

E3 : MIDI DIN physique (UART+opto — matériel), MIDI téléphone, MIDI out/
thru, choix de banque SF2 multiple, kits batterie nommés.
V2 : synthé homemade (piste discutée : architecture type Korg M1 —
AI Synthesis PCM+VDF+VDA, très faisable sur A53 ; contrainte = les
échantillons (ROM Korg copyright) → auto-sampleur via MIDI out + entrées
de la console, ou multisamples libres).

## Test utilisateur : EN ATTENTE (clavier/DAW → écoute ; penser à monter
les faders P1/P2 et leur routage — config mixer à refaire post-fix persistance)
