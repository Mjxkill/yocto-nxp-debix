# ARCHI V12-LOOP — Looper (pédale de boucle native)

Date : 2026-07-06 · Priorité 2 roadmap V12.

## Sémantique (pédale RC classique)

REC (enregistre, définit la longueur au tap suivant) → PLAY (boucle) →
OVERDUB (couches) → PLAY → … ; STOP ; UNDO (retire la DERNIÈRE session
d'overdub, multi-passes incluses) ; CLEAR. Boucles libres (sync tempo V2).

## Intégration mixer-pro

| Élément | Choix |
|---|---|
| Source enregistrée | paire de tranches configurable (src_a/src_b, mono si b=-1), POST-fader/automix — défaut M1/M2 |
| Restitution | additionnée dans les tranches **P1/P2** (comme le sampleur : « sources internes ») avec gain looper propre — fader/routage P1/P2 gouvernent l'ensemble |
| Buffers | boucle + undo : 2 × 60 s stéréo float (2 × 23 Mo) alloués AU DÉMARRAGE (control thread) — zéro alloc RT |
| Longueur max | 60 s (op de config, replafonnable) |

## Machine d'état (audio_thread, atomique)

IDLE → REC : copie source → buf[len++] (plafond → PLAY auto)
REC → PLAY : longueur figée, pos=0
PLAY : P1/P2 += buf[pos] × gain ; pos wrap
PLAY → OVERDUB : session ouverte ; par frame : si la position n'a pas
  encore été visitée CETTE session (compteur frames_dubbed < length),
  undo[pos] = buf[pos] AVANT l'ajout — l'UNDO restaure alors l'état
  pré-session même après plusieurs passes de dub (les passes 2+
  s'additionnent sur des positions dont l'original est déjà sauvé).
OVERDUB → PLAY : session fermée, layers++
UNDO (control) : memcpy undo→buf hors RT ? NON — 23 Mo. L'undo est fait
  PAR L'AUDIO en douceur : flag undo_pending, l'audio restaure 96 frames
  par bloc en parallèle de la lecture (restauration complète en
  length/96 blocs ≈ imperceptible, la couche disparaît progressivement
  sur un tour de boucle au pire).
STOP : lecture figée (pos conservée) ; PLAY reprend.
CLEAR : len=0, state IDLE (buffers conservés, pas de free).

## Ops

- `looper_ctl {action:"rec"|"play"|"overdub"|"stop"|"undo"|"clear"}`
- `looper_cfg {src_a, src_b, gain_db}`
- `looper_status` → {state, len_s, pos_s, layers, src_a, src_b}

## GUI (page PADS → « PADS & LOOPER »)

Bandeau looper sous la grille : gros boutons ● REC / ▶ PLAY / ⊕ DUB /
■ STOP / UNDO / CLEAR, affichage temps pos/len + barres de progression,
sélecteur de source (‹ › sur les tranches). Poll looper_status 2 Hz
(page visible). Web = E2.

## Tests E1

1. REC 2 s (tone USB) → PLAY : boucle audible/mesurable sur P1/P2, wrap propre.
2. OVERDUB 2 passes → couche ajoutée ; UNDO → retour exact pré-session
   (meters + écoute).
3. STOP/PLAY, CLEAR, re-REC.
4. Delta xruns 0 pendant toutes les transitions.

## Invariants

- Zéro allocation/IO/memcpy massif dans audio_thread (undo incrémental).
- Sampleur et looper coexistent sur P1/P2 (addition) — gains propres.
- Transitions par atomics ; le control thread ne touche jamais les
  buffers pendant REC/OVERDUB (états vérifiés avant config).
