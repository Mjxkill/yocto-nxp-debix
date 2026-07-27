# TESTS V13.9 — AUTOMIX LIVE complet : balance 3 groupes + gate auto + solo + fixes régressions

Date : 2026-07-27 (sprint 12-18/07, validation finale après reboot)

## Versions

| Artefact | Référence |
|---|---|
| Commit git | `6da9847e` (branche L6.6.36-2.1.0-debix_model_ab) |
| mixer-pro | md5 `6785fe27106bcc549ad4d4e98637052b` |
| mixer-console (LCD) | md5 `25269782e1da8819e0b3043178bc9a10` |
| mixer-ml-inference | md5 `ff5a72d797fa7ae46ba0a9ee974a70bd` |
| beta.html (web) | md5 `d34d20fa3ee37068f6faaddb91627db8` |
| Firmware SOF / topology | inchangés (baseline V3.2.2+, DMA 2 ms) |
| Dataset | automix_8track régénéré ×2 (48 WAV : rooms/ruff exclus, voix→LEADVOX, dynamiques préférés) |

## Fonctionnalités sous test (V13.7 → V13.9)

- Balance auto 3 groupes (quadrants) : LUFS −14 + voix +3 dB + chœurs +1,5 dB,
  gel universel « pas de signal → aucun gain ne bouge », gel par groupe (act[]),
  staging 8 dB/s au 1er lock, gains de groupe conservés au reset.
- Gate auto adaptative (al_ref − gate_db) armée en autolive, désarmée au
  changement de rôle (fix gate fantôme).
- Mémoire du risque = anti-blast de reprise seulement (≤ 5 s après silence).
- Solo manuel (bouton S) + détection auto v2 (élévation vs propre base),
  auto OFF par défaut.
- EQ placement : HPF voix 250 Hz / chœurs 180 Hz.
- Master : EQ smile crossfadé + makeup LUFS + limiteur −1 dBFS.
- Mastering NPU réparé (Wants=mixer-ml-inference + unit /etc purgée) —
  démarre seul au boot, enveloppe vivante 62/64 bandes.
- Reset ne force plus le vfocus (réglage opérateur respecté).
- GUI web + LCD : page AUTO MIX 2 colonnes, tous les réglages exposés.

## Protocole

Reboot carte complet → vérif 4 services actifs (mixer-pro, mixer-console,
mixer-ml-inference, anti-larsen) sans intervention → checklist par morceau
(manifest → rôles → reset → un seul pw-play) → écoute au casque, 3 morceaux
propres du dataset :

| Morceau | Rôle AUTRES | Verdict utilisateur |
|---|---|---|
| KatWright_ByMySide | line (cuivres) | « c'est bon ! pas parfait mais bon ! » |
| WestEndBlend_MustBeVoodoo | line (cuivres) | « c'est bon !! … stable et agréable à écouter » (volume lent au 1er départ post-reboot = gains à réapprendre, attendu) |
| RememberJones_DontPutMeOnHold | line (cuivres+BV) | validé — « étape importante » |

## Résultats

- ✅ Pompage : disparu (solo auto OFF par défaut ; verdict « stable »).
- ✅ Balance voix/musique/chœurs stable et agréable sur 3 morceaux.
- ✅ Démarrage : plein volume dès le 2e morceau (gains conservés) ; le tout
  1er après reboot réapprend (~15 s), comportement attendu.
- ✅ Daemon NPU : démarre seul au boot, survit aux stop/start (prouvé).
- ✅ Reset préserve les réglages opérateur (vfocus prouvé ON et OFF).

## Test utilisateur : OUI (3 morceaux, casque, 2026-07-27)

## Restes connus (hors périmètre de cette fiche)

- Anti-larsen : test sinus d'écoute (morsure TAC + off-by-one kernel BQ6/12)
  et fix faux-positifs sur notes tenues (notch 246 Hz posé sur musique, prouvé).
- Spatializer voix : jamais validé à l'oreille.
- Solo auto v2 : validation dédiée (anti-battement) avant réactivation défaut.
- « Pas parfait » : affinage fin (places par rôle réglables, persistance des
  tunables) — voir docs/REVUE_2026-07-18.md.
