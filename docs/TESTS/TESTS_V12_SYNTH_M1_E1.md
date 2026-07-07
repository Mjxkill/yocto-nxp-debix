# TESTS V12-SYNTH E1 — moteur « M1-like » (AI Synthesis) + éditeur de patch

Date : 2026-07-07 · ARCHI : ARCHI_V12_SYNTH_M1.md (validé critic, approved).
Souhait utilisateur : synthé homemade type Korg M1 avec sa page de config.

## Implémentation

- **ala-synth.c** (nouveau, ~650 lignes) dans midi-expander : le port
  f_midi étant exclusif, le moteur M1 vit À CÔTÉ de fluidsynth, choix
  **par canal MIDI** (chan_engine[16] ∈ {GM, M1}).
  - Parseur SF2 minimal lecture seule (RIFF inst/ibag/igen/shdr + smpl
    mmappé) : **324 instruments** de GeneralUserGS deviennent des
    multisamples sources. Héritage des générateurs de la **zone globale**
    (sampleModes/rootkey) — GeneralUser y met le mode boucle.
  - Voix (16, vol de la plus ancienne) : OSC1+OSC2 PCM (detune/balance,
    interp linéaire, boucle), **VDF 2×1-pôle sans résonance** (authentique
    M1) avec EG_int, **VDA**, EG **ADBSSR** (params 0-99 façon M1), LFO
    triangle pitch avec delay, vélocité quadratique.
  - File SPSC MIDI (thread driver → rendu), patches 16 slots persistés
    (/var/lib/ala/synth-patches.conf), 4 usine : M1 Piano (Stereo Grand),
    Warm Pad (Synth Strings), Deep Bass, Bells (Celeste).
  - Ops : engine / inst_list / patch_list / patch_get / patch_set (live) /
    patch_save. Proxy mixer-pro midix_ctl étendu (passthrough "line",
    réponses 16 Ko en boucle de lecture).
- **GUI PageExpander enrichie** : bascule GM/M1 par canal (violet), le
  sélecteur ‹ › navigue programmes GM ou patches M1, bouton **ÉDIT** →
  **éditeur plein écran** : OSC1/OSC2 (‹ › dans les 324 instruments),
  detune/balance, VDF (cutoff, EG int, ADBSSR), VDA (ADBSSR), LFO
  (rate/depth/delay), vel sens, level — sliders 0-99, édition LIVE
  pendant le jeu, SAUVER persiste la banque.

## Validation board (PC → amidi hw:1,0,0)

| Test | Résultat |
|---|---|
| Parse SF2 au boot | « moteur M1 prêt », 324 instruments, 4 patches usine ✓ |
| M1 Piano (accord) | peak 33 % FS, decay naturel de piano ✓ |
| Warm Pad tenue 5 s | sustain infini (boucle zone globale), LFO visible sur le peak, release propre ✓ |
| Édition LIVE cutoff 55→10→55 | timbre fermé puis rouvert pendant la tenue ✓ |
| GM (CH2 cordes) + M1 (CH1) simultanés | deux timbres, un seul ring ✓ |
| Persistance (patch_save + restart) | lfo_depth 25 + engines[0]=1 restaurés ✓ |
| PANIC | tue les voix (y compris bloquées) ✓ |
| xruns mixer-pro | 0 nouveau sur toute la session ✓ |
| GUI éditeur | 0 erreur QML ✓ |

## Pièges rencontrés (à retenir)

1. **Builds silencieusement skippés** : `source oe-init && bitbake` avec un
   cwd hérité → source échoue, `grep -c ERROR` affiche 0, on déploie un
   binaire PÉRIMÉ. Toujours vérifier `md5sum build vs board` + mtime
   binaire > mtime source après deploy.
2. **Perte du 1er message MIDI après idle** (amidi open-send-close) :
   session sacrificielle nécessaire en test ; un DAW/clavier réel garde
   le port ouvert — non-problème en usage. Un décrochage complet de la
   réception f_midi est apparu après de nombreux rebinds gadget →
   récupéré par reboot ; À SURVEILLER en usage DAW réel.
3. Voix bloquée si note-off perdu (restart daemon entre on et off) →
   PANIC la tue ; comportement normal d'un module MIDI.

## Reste

V2 : vrais multisamples M1 via l'auto-sampleur (MIDI out + entrées
console), pitch-bend/modwheel, résonance optionnelle (mode « pas M1 »),
effets (la console les a déjà), import SysEx M1.

## Test utilisateur : EN ATTENTE (page EXPANDEUR → canal M1 → ÉDIT + jeu)
