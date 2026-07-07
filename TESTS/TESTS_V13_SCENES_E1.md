# TESTS V13-SCENES E1 — panneau SCÈNE : profils + pilotage des automatismes

Date : 2026-07-08 · ARCHI : ARCHI_V13_SCENES.md.
Demandes : toutes les fonctions auto toggleables (anti-larsen inclus),
Dugan retiré du master → panneau AUTO MIX, panneau SCÈNE avec profils à
bascule instantanée + 4 gros boutons actifs, miroir web.

## Implémentation

- **Scènes (mixer-pro)** : 6 slots /var/lib/mixer-pro/scenes/sceneN au
  format mixer_state (save_state_to paramétrée). scene_recall SANS
  coupure : fichier lu en mémoire → fmemopen → application sous
  target_lock (aucune I/O disque sous lock), gains dans les TARGETS
  (glissement smooth_gains), insert chain ré-initiée HORS lock seulement
  si le spec diffère (pattern set_insert). Ops scene_save/recall/list
  (+ nom par slot).
- **Toggles runtime** : anti-larsen — le socket status accepte
  « enable 0|1 » (compat clients existants par timeout 80 ms), disable
  retire les notches posés, daemon reste résident si enable=0 au boot ;
  gui-http POST /api/larsen {enable}. Mastering — g_insert_bypass atomic
  autour du process insert (chaîne chaude) + set/get_insert_bypass.
  Dugan/keeper/vfocus : ops existants, tri-état AUTOMIX exclusif
  (OFF→MUSIQUE→VOIX→OFF).
- **GUI native** : page SCÈNE (nav 10) — 4 gros boutons actifs
  (ANTI-LARSEN : nb notches + dernière fréq ; AUTOMIX : mode + correction
  max dB ; MASTERING : état chaîne + VU enveloppe ML ; VOIX DEVANT :
  Σ creusé + ♪) avec mini-VU, + 6 slots RAPPEL/SAUVER. AUTOMIX+⚙ retirés
  du bandeau master ; PageBandmix reçoit DUGAN ON/OFF + ⚙ (signal vers
  le panneau réglages).
- **Web** : onglet SCÈNE miroir (4 boutons + slots via /api/cmd et
  /api/larsen), bouton automix retiré du bandeau mixer, ⚙ DUGAN sur la
  page SCÈNE.

## Validation board

| Test | Résultat |
|---|---|
| Anti-larsen runtime | POST enable:0 → status enable=0 (notches retirés) ; enable:1 → détection reprend ✓ |
| Mastering bypass | ON→OFF→ON instantané, chaîne conservée (chain:1) ✓ |
| Scène : save (fader M3=0,75) → fader→0,10 → recall | **0,7500 restauré**, xrun 4→4 = **0** ✓ |
| scene_list | slot 0 « Test A » used=1, autres libres ✓ |
| Tri-état exclusif | jamais Dugan+keeper simultanés (ops croisés) ✓ |
| QML | 0 erreur (PageScene + PageBandmix modifiée) ✓ |
| Web | page SCÈNE servie, JS node --check OK ✓ |

## E2 (fait 2026-07-08 — commits 06893450 + e656e1c4)

- **Scènes COMPLÈTES** : /api/scene/save|recall (gui-http orchestre) —
  mixer-pro + alsactl store/restore (TAC/PGA) + blobs DSP + patches et
  canaux synthé (cmd « reload » du midi-expander). Validé board : fader
  + patch synthé modifiés puis rappelés à l'identique, 0 xrun.
- **Confirmation RAPPEL** : 2 taps (CONFIRMER ? 3 s) natif, double-clic
  web — anti-fausse-manip live.
- **Nommage** : clavier tactile natif (AZERTY+chiffres, 20 car.),
  prompt web.
- **Page AUTO MIX web** : parité totale (rôles, mesures, calc/lock/live,
  Dugan, place à la voix avec barres de cut).

## Reste (E3)

Scènes : les samples du PADS (gros fichiers — lien plutôt que copie),
export/import de scènes (USB), photo d'écran du mix dans le slot.

## Test utilisateur : EN ATTENTE (page SCÈNE au doigt : 4 boutons +
sauvegarde/rappel de 2 profils différents en conditions réelles)
