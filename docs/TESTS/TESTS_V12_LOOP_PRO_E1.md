# TESTS V12-LOOP-PRO E1 — Loopstation multipiste

Date : 2026-07-06 · ARCHI : ARCHI_V12_LOOPER_PRO.md (validé critic, approved)
Priorité 2 roadmap V12 — **remplace** le looper mono-buffer V12-LOOP
(74718680) qui ne pouvait pas activer/désactiver une couche.

## Demande utilisateur

« il faut que je puisse configurer les voies d'entrées, j'ai les 8 voies
et je sélectionne la voie que je veux looper en live… il faut pouvoir
activer ou désactiver des samples de la loop… fais le looper sur une
autre page et prend ton temps pour faire les choses en version pro. »

## Implémentation

- mixer-pro : **6 pistes indépendantes** (`g_tr[LOOP_TRACKS]`), chacune =
  1 couche discrète, buffer stéréo 40 s propre (6 × 14,6 Mo = 88 Mo,
  alloués au démarrage). Horloge maître partagée (`g_master_len` +
  `g_lpos`) posée par la 1ʳᵉ piste ; pistes suivantes enregistrées
  **alignées** (un tour complet → PLAY auto) → phase garantie, toutes à
  master_len. Enregistrement POST-fader d'une **voie sélectionnable**
  (src_a, mono→dup). Restitution additionnée dans P1/P2 (in_block[16/17]).
  - Sécurité RT : `g_lpos` copié en local par bloc ; memset de la piste
    au **rec-arm d'une piste VIDE** (control thread, non lue par l'audio)
    → aucun glitch, pas de undo massif. Un seul REC simultané.
  - master_len découplé : conservé tant qu'une piste a du contenu, remis
    à 0 quand la dernière piste est effacée / clear_all.
  - Ops : `looper_track_ctl {track,action:rec|play|mute|unmute|clear}`,
    `looper_track_cfg {track,src_a,src_b,gain_db}`,
    `looper_ctl {action:play_all|stop_all|clear_all}`, `looper_status`
    (global + tableau par piste avec peak VU).
- Console native : **nouvelle page dédiée LOOPER** (nav 7 pages —
  MIXER/EFFETS/MASTERING/PADS/**LOOPER**/ROUTING/SYSTÈME). En-tête
  transport (PLAY/STOP/CLEAR ALL + barre de position maître) + 6 lignes
  piste : badge, **sélecteur de voie ‹ M1..M8 / USB1..USB8 ›**, VU crête,
  durée, boutons ● REC / ▶ PLAY / **ON·MUTE** / ✕ CLEAR. Poll 4 Hz gaté
  page visible. Bandeau looper retiré de PagePads.

## Validation board (tones USB injectés, 2 pistes même source strip 8)

Baseline xrun = 3. Track 0 = 440 Hz (maître), Track 1 = 660 Hz (aligné).

| Test | Résultat |
|---|---|
| Track 0 REC 2,5 s → PLAY | master_len figé 2,88 s, run=1, state play ✓ |
| Track 1 REC aligné (un tour) → PLAY auto | len 2,88 s = master, 2ᵉ couche ✓ |
| 2 couches (440+660) sur P1/P2 | pic in[16] = **50,2 % FS** (≈ ×2) ✓ |
| MUTE track 1 | in[16] → **25,1 %** (440 seul) ✓ |
| UNMUTE track 1 | in[16] → **50,0 %** ✓ |
| MUTE track 0 | in[16] → **25,1 %** (660 seul) ✓ |
| UNMUTE track 0 | in[16] → **50,0 %** ✓ |
| CLEAR track 1 | 440 seul (25,1 %), **track 0 continue**, master préservé ✓ |
| CLEAR track 0 (dernière) | tout vide, in[16]=0, **master_len→0, run→0** ✓ |
| Sélecteur de voie (looper_track_cfg) | src_a reflété dans looper_status ✓ |
| **Delta xruns** (REC/PLAY/MUTE/UNMUTE/CLEAR ×N) | 3 → 3 = **0 nouveau** ✓ |
| GUI PageLooper | rendu sans erreur QML (0 QColor/undefined) ✓ |

## Reste (E2)

Web : page LOOPER miroir (beta.html/index.html). Sélecteur source stéréo.
Sync tempo / quantize. Overdub intra-piste. Footswitch GPIO. Persistance
config src/gain par piste.

## Test utilisateur : EN ATTENTE (page LOOPER + écoute REC/mute/clear par piste)
