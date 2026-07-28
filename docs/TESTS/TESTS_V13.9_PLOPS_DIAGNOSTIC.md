# TESTS V13.9 — Diagnostic « plops » : cause racine = écritures TAC de l'anti-larsen

Date : 2026-07-27/28. Symptôme : 4-7 plops audibles par morceau (dataset USB →
automix → casque), produit non vendable en l'état.

## Versions

Commit `19b46b1b` (mixer-pro md5 `6785fe27…`), config nominale AUTOMIX LIVE.
Outil de mesure : `tools/tapdump.c` (dump du tap FX kernel
`/dev/imx-audio-tap-out`, 8 ch S32 post-effets, copie par spans — 0 perte).

## Méthode (élimination, une variable par run, verdicts oreille + compteurs)

| Run | Config | Compteurs (op get_drift) | Plops (oreille) |
|---|---|---|---|
| 1 | nominale | cap_empty +4, xrun_cap +1 | 4-7 |
| 2 | nominale | **0 famine** | 2 gros |
| 3 | gates OFF (−30) | — | 1 petit |
| 4 | gates OFF | cap_empty +1, iter≥50µs 8 | 3 gros **après l'arrêt de la voix** + 2 petits |
| 5 | gates OFF + vfocus OFF | — | 2 gros |
| 6 | **anti-larsen STOPPÉ**, reste nominal | — | **0** |
| 7 | idem (contre-vérif) | — | **0** |
| 8 | idem + enveloppe NPU vérifiée **62/64 vivante en lecture** | — | **0** |

Éliminés par la mesure : famines ring USB (0 pendant des runs avec plops),
corrections de drift (~100/morceau, décorrélées du compte), gates, vfocus,
NPU. Retour USB hôte : silencieux (rien routé) — impasse d'enregistrement.

**Flagrant délit** : surveillance du socket anti-larsen pendant la lecture —
notch **545 Hz posé sur les 2 canaux master en pleine musique** à 22:30:23
(faux positif sur note tenue ; déjà observé : 246 Hz, âge 60 s, sur un autre
morceau). Chaque pose = écriture de blob de coefficients biquad DAC dans le
TAC5212 **en cours de stream** + apparition instantanée d'un notch Q30 −9 dB
sur une note qui sonne = plop.

## Conclusion (validée utilisateur)

- **Cause racine : l'anti-larsen écrivait les biquads TAC en live.**
- **Règle d'architecture** : les effets TAC sont **statiques** (soundcheck) —
  aucune écriture pendant le show. Tout traitement dynamique doit vivre en
  software (mixer-pro : double-banque + crossfade sans clic, technique
  validée eqx/meq) ou dans le DSP.
- Anti-larsen **stoppé + disabled au boot** (2026-07-27). Ne pas réactiver
  tel quel. Refonte v2 logicielle à concevoir : notchs en biquads mixer-pro,
  réjection des notes tenues, actif seulement si micros actifs.

## Test utilisateur : OUI — « plus de plop ! »

## Restes

- 2 « petits plops sans lien » ponctuels probablement liés aux famines rares
  (cap_empty ~1/morceau) — sous le seuil de gêne, à traiter au sprint
  transport (ASRC/PLC, cf. REVUE_2026-07-18 §fiabilisation).
- Test sinus anti-larsen (morsure TAC) devenu sans objet dans cette
  architecture — sera re-testé sur la v2 logicielle.
