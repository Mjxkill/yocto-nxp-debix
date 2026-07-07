# TESTS V12-LOOP E1 — Looper (pédale de boucle native)

Date : 2026-07-06 · ARCHI : ARCHI_V12_LOOPER.md (validé critic)
Priorité 2 roadmap V12.

## Implémentation

- mixer-pro : moteur `loop_render()` dans l'audio_thread (sous target_lock,
  après le convert S32→float, après `smp_render`). Machine d'état
  atomique IDLE/REC/PLAY/OVERDUB/STOP. Deux buffers RAM 60 s stéréo float
  (boucle + undo, 2 × 23 Mo) alloués AU DÉMARRAGE — zéro alloc RT.
  - REC : `buf[len++] = in_block[src] × input_gain × automix_gain`
    (POST-fader), plafond 60 s → PLAY auto.
  - PLAY/OVERDUB : restitution additionnée dans les tranches P1/P2
    (in_block[16/17], mortes en skip_phone) avec gain looper propre.
  - OVERDUB : snapshot `undo[pos]` à la 1ʳᵉ visite de chaque position
    (`dub_count < len`) → UNDO restaure l'état pré-session même sur
    plusieurs passes.
  - UNDO : restauration incrémentale par l'audio (96 frames/bloc, flag
    `undo_pending`) — jamais de memcpy 23 Mo en RT ni dans le control thread.
  - Ops : looper_ctl (rec/play/overdub/stop/undo/clear), looper_cfg
    (src_a/src_b/gain_db), looper_status.
- Console native : bandeau LOOPER sous la grille PADS — boutons
  ● REC / ▶ PLAY / ⊕ DUB / ■ STOP / UNDO / CLEAR, temps pos/len, barre de
  progression, source affichée. Poll looper_status 2 Hz (page visible).

## Validation board (tone USB injecté depuis le PC de dev)

Source enregistrée = tranches USB 8/9 (fader forcé 0 dB pour le test),
tone 440 Hz −12 dBFS stéréo. Rétabli ensuite (8/9 → 0, source → M1/M2).

| Test | Résultat |
|---|---|
| REC 2,2 s → PLAY | longueur figée 2,63 s, state play, pos qui avance ✓ |
| Boucle sur P1/P2, source live coupée | in[16]/in[17] = 25,1 % FS (= niveau source), in[8]/in[9] = 0 ✓ |
| Vérif POST-fader | fader 8/9 à 0 → boucle silencieuse ; fader à 0 dB → boucle enregistrée ✓ |
| OVERDUB 1 passe (couche 660 Hz) | layers 1→2, pic P1/P2 44–59 % FS (≈ ×2, battement 440/660) ✓ |
| UNDO | layers 2→1, pic revenu EXACTEMENT à 25,1 % FS (état pré-session) ✓ |
| STOP → CLEAR | state stop puis idle, len=0, layers=0 ✓ |
| Delta xruns (REC/PLAY/DUB/UNDO/STOP/CLEAR) | 4 → 4 = **0 nouveau** ✓ |

## Reste (E2)

Web : bandeau looper miroir dans beta.html/index.html. Sélecteur de source
graphique (‹ ›). Sync tempo / quantize. Footswitch (GPIO) plus tard.

## Test utilisateur : EN ATTENTE (page PADS + écoute REC/PLAY/OVERDUB/UNDO)
