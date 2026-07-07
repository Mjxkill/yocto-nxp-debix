# ARCHI V12-SYNTH — moteur « M1-like » (AI Synthesis) dans l'expandeur

Date : 2026-07-07 · Souhait utilisateur : synthé homemade type Korg M1,
avec sa page de configuration. Nuit Fable — E1.

## Architecture retenue : 2e moteur DANS midi-expander

Le port f_midi (rawmidi) est EXCLUSIF : un seul lecteur. Donc pas de 2e
daemon — le moteur M1 (`ala-synth`) vit dans midi-expander, à côté de
fluidsynth. **Choix du moteur PAR CANAL MIDI** : chan_engine[16] ∈
{GM=fluidsynth, M1=ala-synth}. Le callback MIDI dispatche ; le rendu
additionne fluid + M1 dans le même ring SHM → P1/P2. La page EXPANDEUR
devient : par canal, GM (progr. GM) OU M1 (patch), + éditeur de patch.

## Source des échantillons : la SF2 embarquée (parseur minimal)

Le moteur lit les MULTISAMPLES de GeneralUserGS.sf2 (déjà sur la carte) :
parseur RIFF borné (~200 lignes) : LIST pdta → inst/ibag/igen (zones :
keyRange gen 43, sampleID gen 53, overridingRootKey 58, sampleModes 54)
+ shdr (start/end/loop/samplerate/originalPitch) ; LIST sdta → smpl
(PCM s16, mmap du fichier, lecture in-place, conversion float au rendu).
→ tous les instruments de la banque deviennent des sources PCM pour les
oscillateurs. Les vrais samples M1 viendront plus tard (auto-sampleur).

## Voix M1-like (16 voix, vol de la plus ancienne)

```
OSC1 (multisample, pitch = note−rootKey, interp linéaire, loop)
OSC2 (idem, detune ±50 cents, balance osc1/osc2)      [mode single: osc2 off]
  → VDF : LP 2×1-pôle SANS résonance (authentique M1),
          cutoff 0-99 (100 Hz–12 kHz log) + EG_int × EG_vdf + kbd track léger
  → VDA : gain × EG_vda × vélocité^sens
EG (VDF et VDA) : forme M1 ADBSSR — Attack t, Decay t → Break level,
  Slope t → Sustain level, Release t (params 0-99 comme le M1)
LFO triangle : rate/depth → pitch (vibrato) + delay
```

Coût : 16 voix × 2 osc × 96 frames / 2 ms sur core 0-1 — très large marge
(le M1 le faisait en 1988 sur ASIC).

## Patches

- Struct : {name, osc1_inst, osc2_inst (-1=single), detune, balance,
  vdf:{cutoff, eg_int, a,d,bl,s,sl,r}, vda:{a,d,bl,s,sl,r}, lfo:{rate,
  depth, delay}, vel_sens, level} — params 0-99 (esprit M1).
- Banque : 16 slots /var/lib/ala/synth-patches.conf (texte, un patch par
  bloc), 4 patches usine par défaut (Piano, Pad, Bass, Bells — sources
  GeneralUser). Éditables/sauvables depuis la GUI.

## Ops socket (extension du protocole texte existant)

- `engine <chan> <gm|m1> [patch_idx]` — moteur par canal
- `inst_list` → instruments SF2 (idx + nom) pour le choix des OSC
- `patch_list` / `patch_get <idx>` / `patch_set <idx> <param> <val>`
  (édition live : cutoff/level immédiat, enveloppes à la prochaine note)
- `patch_save <idx> <name>` — persiste la banque
- `status` étendu : engines[16] + patch[16]

## GUI — page EXPANDEUR enrichie (pas de 9e page)

- Ligne de canal : interrupteur **GM/M1** ; en M1 le sélecteur ‹ › navigue
  les 16 patches (nom affiché) ; bouton **ÉDIT** → panneau éditeur.
- **Panneau éditeur de patch** (overlay plein écran, style StripFxDrawer) :
  choix OSC1/OSC2 (‹ › dans inst_list), detune/balance, sliders 0-99
  VDF (cutoff, eg_int, ADBSSR), VDA (ADBSSR), LFO (rate/depth/delay),
  vel_sens, level, champ nom, SAUVER. Édition live (patch_set) pendant
  qu'on joue.

## Tests E1

1. `engine 0 m1 0` + notes PC → son du patch usine, 0 xrun.
2. Édition cutoff live pendant tenue → le timbre bouge.
3. GM sur CH1 + M1 sur CH2 simultanés (dispatch).
4. patch_save + restart daemon → banque restaurée.
5. 16 voix (accords tenus + arpèges) → pas de clic au vol de voix
   (release rapide 3 ms sur la voix volée).

## Invariants

- Rendu M1 additionné dans le buf AVANT l'écriture ring (même cadence
  2 ms, jamais de blocage : si patch/inst invalide → silence).
- Le thread ctl ne touche jamais les voix actives (params copiés à la
  note-on ; cutoff/level lus par le rendu en relaxed — races bénignes).
- fluidsynth inchangé pour les canaux GM ; parseur SF2 en lecture seule.
