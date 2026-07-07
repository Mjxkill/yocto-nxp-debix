# ARCHI V13-SCENES — panneau SCÈNE : profils + pilotage des automatismes

Date : 2026-07-08 · Demandes utilisateur :
1. TOUTES les fonctions auto activables/désactivables (l'anti-larsen est
   aujourd'hui toujours actif → toggle runtime).
2. Retirer le bouton AUTOMIX (Dugan) du bandeau master → l'intégrer au
   panneau AUTO MIX.
3. Panneau SCÈNE : sauvegarde de TOUS les paramètres de la table,
   plusieurs profils, bascule instantanée.
4. Sur SCÈNE : 4 GROS boutons actifs avec infos/VU — ANTI-LARSEN ON/OFF,
   AUTOMIX OFF/MUSIQUE/VOIX(Dugan), MASTERING ON/OFF, VOIX DEVANT ON/OFF.
5. Web = miroir du LCD.

## 1. Moteur de scènes (mixer-pro)

- 6 slots, /var/lib/mixer-pro/scenes/scene<N> (+ scene<N>.name).
- **Sauver** = écrire l'état complet au format mixer_state EXISTANT
  (réutilise save_mixer_state paramétrée par chemin) — faders, mutes,
  routing, sends, automix (cfg+membres+poids), expander, comp, bandmix
  (rôles+réf+live), vfocus, insert spec.
- **Rappeler (instantané, sans coupure audio)** : parse du fichier dans
  des TABLEAUX DE STAGING (control thread, AUCUN lock pendant l'I/O),
  puis application sous target_lock bref (memcpy des targets ~2 Ko +
  exp/cmp/vf_configure) — les gains glissent via smooth_gains (pas de
  clic). Insert chain : ré-init type set_insert si le spec diffère.
- Ops : scene_save {slot,name}, scene_recall {slot}, scene_list.
- Le mixer_state courant reste la persistance boot (inchangé) — une
  scène rappelée devient l'état courant (donc re-persistée).

## 2. Toggles runtime des automatismes

| Fonction | Mécanisme |
|---|---|
| Anti-larsen | daemon : le socket status accepte désormais une commande optionnelle « enable 0|1 » avant de répondre (compat : lecture vide = status). gui-http : POST /api/larsen {enable}. Les notches posés restent, la détection s'arrête ; « enable 0 » retire aussi les notches actifs (retour neutre). |
| Automix Dugan (parole) | existant set_automix_cfg on |
| Auto-mix musique (keeper) | existant bandmix_live on |
| Mastering (insert out 0/1) | NOUVEAU g_insert_bypass atomic testé dans l'audio autour du process insert + op set_insert_bypass — la chaîne reste instanciée, bascule instantanée |
| Voix devant (vfocus) | existant set_vfocus on |

Bouton AUTOMIX tri-état (exclusif, simple pour l'utilisateur) :
OFF → MUSIQUE (bandmix_live=1, dugan=0) → VOIX (dugan=1, live=0) → OFF.

## 3. GUI native

- **PageScene** (10e page, onglet « SCÈNE ») :
  - 4 gros boutons actifs (~grille 2×2) avec état + mini-infos live :
    ANTI-LARSEN (nb notches posés + dernière fréq), AUTOMIX
    (OFF/MUSIQUE/VOIX + correction max courante en dB), MASTERING
    (ON/OFF + mini-VU enveloppe ML), VOIX DEVANT (ON/OFF + barre de
    creusement total + ♪).
  - 6 slots de scène : nom + RAPPEL + SAUVER (le SAUVER écrase le slot
    avec l'état courant ; nommage E2, défaut « Scène N »).
  - Polls gatés page visible : bandmix_status/get_vfocus/get_midix déjà
    là ; larsen via XHR /api/larsen (pattern StripFxDrawer) ; scene_list.
- **MIXER master** : retrait du bouton AUTOMIX + ⚙ (demande explicite).
- **PageBandmix** : bandeau « DUGAN (PAROLE) » — toggle + ⚙ (ouvre le
  panneau réglages existant).

## 4. Web (beta.html)

Page SCÈNE miroir (4 gros boutons + slots via /api/cmd et /api/larsen
POST) ; retrait du bouton amx-global du bandeau mixer, déplacé dans la
page AUTO MIX web (avec le ⚙ déjà présent).

## Tests E1

1. Toggle anti-larsen OFF → daemon inerte (status enable=0, notches
   retirés) ; ON → détection reprend. Persistance conf.
2. set_insert_bypass : mastering coupé/rétabli sans clic ni xrun.
3. Tri-état AUTOMIX : exclusivité vérifiée (jamais Dugan+keeper ensemble).
4. scene_save slot 1 (config A) / modifier faders / scene_recall →
   retour exact, AUCUN xrun, glissement doux ; scene_list noms ok.
5. Reboot → scènes conservées ; état courant = dernier état (inchangé).
6. Web : parité visuelle + fonctionnelle.

## Invariants

- Rappel de scène : JAMAIS d'I/O fichier sous target_lock (staging).
- Bypass mastering : la chaîne LV2 reste chaude (pas de ré-init).
- Anti-larsen OFF : notches retirés proprement (biquads neutralisés).
