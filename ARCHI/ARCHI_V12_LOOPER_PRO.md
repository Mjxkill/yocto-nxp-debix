# ARCHI V12-LOOP-PRO — Loopstation multipiste (refonte)

Date : 2026-07-06 · Remplace le looper mono-buffer V12-LOOP (74718680).
Priorité 2 roadmap V12 — version « pro ».

## Pourquoi une refonte

Le looper V12-LOOP est **mono-buffer à overdub destructif** : les couches
sont sommées dans un seul tampon, seul le DERNIER overdub est annulable
(undo). Il ne peut PAS, par construction, activer/désactiver une couche
arbitraire. Demande utilisateur :
1. Choisir **en live la voie d'entrée** à boucler (parmi les 8 voies).
2. **Activer/désactiver** individuellement les couches (« samples ») de la
   boucle sans les effacer.
3. Le tout sur **une page dédiée**, propre.

→ Modèle **loopstation multipiste** (type Boss RC-505) : N pistes
indépendantes, chacune = 1 couche, chacune avec sa voie source et son
mute. C'est la seule architecture qui satisfait (2).

## Modèle de synchronisation

- Horloge maître partagée : `g_master_len` (frames) + `g_lpos` (position
  globale, avance une fois par bloc, wrap à master_len).
- La **1ʳᵉ piste enregistrée** définit `master_len` : REC libre, figée à
  l'appui PLAY (ou plafond LOOP_MAX).
- Pistes suivantes : REC **aligné** sur l'horloge maître — écriture à
  `(start_pos + k) % master_len`, durée = un tour complet → PLAY auto au
  bord de boucle. `len == master_len` pour toutes → phase garantie.
- Toutes les pistes PLAY non-mutées lisent à `g_lpos` et **s'additionnent
  dans P1/P2** (in_block[16/17], comme le sampleur — sources internes).

## Structures (audio_thread, atomiques)

```c
#define LOOP_TRACKS 6
#define LOOP_MAX_FRAMES (40u * 48000u)   /* 40 s/piste — 6×40s stéréo = 88 MiB */
enum { TR_EMPTY, TR_REC, TR_PLAY };

struct loop_track {
    float          *buf;        /* stéréo LOOP_MAX_FRAMES*2, alloué au démarrage */
    _Atomic uint32_t len;       /* frames (= master_len une fois posée), 0 = vide */
    _Atomic int      state;     /* TR_EMPTY / TR_REC / TR_PLAY */
    _Atomic int      muted;     /* 1 = couche désactivée (conservée) */
    uint32_t         rec_head;  /* écriture pendant REC */
    uint32_t         rec_start; /* g_lpos au début du REC aligné */
    uint32_t         rec_done;  /* frames enregistrées ce tour (pistes alignées) */
    int              src_a, src_b; /* voies d'entrée (-1 = mono, dup) */
    float            gain;
    _Atomic uint32_t peak;      /* crête VU (maj en lecture) */
};
static struct loop_track g_tr[LOOP_TRACKS];
static _Atomic uint32_t  g_master_len;   /* 0 tant qu'aucune piste posée */
static _Atomic uint32_t  g_lpos;         /* position globale */
static _Atomic int       g_loop_run;     /* transport global */
static _Atomic int       g_loop_master;  /* index de la piste maître, -1 sinon */
```

Mémoire : 6 × 40 s × 2 ch × 4 o = 88 MiB (board = 3,6 Go, 3,26 Go libres).
Aucun tampon undo (chaque piste est une couche discrète). Tunable.

## loop_render() — audio_thread, SOUS target_lock, après smp_render

```
1. Enregistrement — pour chaque piste en TR_REC :
   ig = input_gain[src]*automix_gain[src]  (POST-fader)
   - piste maître (g_master_len==0) :
       buf[rec_head++] = in_block[src]*ig ; plafond LOOP_MAX → fige
       master_len=rec_head, g_lpos=0, run=1, state PLAY.
   - piste alignée (master_len>0) :
       idx=(rec_start+rec_done)%master_len ; buf[idx]=in_block[src]*ig ;
       rec_done++ ; à rec_done==master_len → len=master_len, state PLAY.
2. Lecture — pour chaque piste TR_PLAY, len>0, !muted :
       in_block[16][f] += buf[g_lpos*2]  *gain ;
       in_block[17][f] += buf[g_lpos*2+1]*gain ; maj peak.
3. Avance g_lpos = (g_lpos+PERIOD_FRAMES)%master_len si run && master_len.
```

Note : g_lpos avancé UNE fois/bloc (pas par piste). Lecture indexée par
frame f à partir de g_lpos local (copie), master_len constant → toutes
pistes en phase.

## Sécurité RT / concurrence

- Buffers alloués au démarrage (control thread), **jamais** en RT.
- CLEAR d'une piste : state←EMPTY (atomique, l'audio cesse de la lire au
  bloc suivant), PUIS memset(buf) dans le **control thread** (non-RT) —
  garantit le silence des positions non ré-enregistrées au prochain REC.
- Config source/gain refusée pendant le REC de CETTE piste.
- REC : refusé si une autre piste est déjà en REC (un seul REC à la fois).
- Transitions par atomics ; le control thread ne touche jamais buf d'une
  piste en TR_REC/TR_PLAY (sauf mute, qui est atomique et lu en lecture).

## Ops socket

- `looper_track_ctl {track, action:"rec"|"play"|"mute"|"unmute"|"clear"}`
- `looper_track_cfg {track, src_a, src_b:-1, gain_db}`  (refusé si REC)
- `looper_ctl {action:"play_all"|"stop_all"|"clear_all"}`  (transport global)
- `looper_status` →
  `{master_len_s, pos_s, run, tracks:[{track,state,len_s,muted,src_a,src_b,gain_db,peak}...]}`

Rétro-compat : les anciens ops mono (`looper_cfg`/`looper_ctl rec/play/
overdub/undo`) sont RETIRÉS — le bandeau PADS aussi. Aucun consommateur
board hors GUI (mono jamais testé en prod hors E1).

## GUI — nouvelle page LOOPER (nav 7 pages)

Nav : MIXER(0)/EFFETS(1)/MASTERING(2)/PADS(3)/**LOOPER(4)**/ROUTING(5)/
SYSTÈME(6). PageLooper.qml, gating par Timer QML visible (poll
looper_status ~4 Hz). Retrait du bandeau looper de PagePads.qml.

Contenu :
- En-tête : titre, longueur maître + barre de position, transport
  PLAY ALL / STOP ALL / CLEAR ALL.
- 6 lignes piste : n° + couleur, **sélecteur de voie source** (‹ M1..M8 /
  USB1..USB8 ›), boutons **● REC / ▶ PLAY** (finalise), **MUTE** (=
  activer/désactiver la couche), **CLEAR**, VU crête, durée enregistrée.

## Tests E1

1. Piste 1 REC 2 s (tone USB voie choisie) → PLAY : master_len figé,
   boucle sur P1/P2, wrap propre.
2. Piste 2 source = autre voie, REC → PLAY : 2ᵉ couche alignée, additionnée.
3. MUTE piste 2 → disparaît de P1/P2 ; UNMUTE → revient. MUTE piste 1
   idem. (= activer/désactiver samples.)
4. CLEAR piste 2 → effacée, piste 1 continue. CLEAR ALL → silence, EMPTY.
5. Sélecteur de voie : changer la source d'une piste vide, vérifier REC
   sur la bonne voie.
6. Delta xruns 0 sur toutes les transitions (REC/PLAY/MUTE/CLEAR).

## Invariants

- 1 piste = 1 couche indépendante, mute/clear individuels.
- Zéro alloc/IO/memset massif dans audio_thread (memset au CLEAR = control).
- Toutes pistes = master_len → phase garantie, lecture à g_lpos partagé.
- Restitution additionnée P1/P2 ; coexiste avec le sampleur (addition).
- Un seul REC simultané ; master = 1ʳᵉ piste posée.
