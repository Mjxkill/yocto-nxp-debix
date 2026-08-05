# BENCH — extraction de voix d'un micro à repisse (modèles sur étagère)

Date : 2026-08-05. Question Michael : « extraire la voix de cette bande son
en live — il n'existe pas des petits modèles déjà existants ? » Contexte :
repisse mesurée sur LeftoverSalmon U7 (plancher −20 dB sous la crête voix,
corrélation d'enveloppe 0,59) ; la soustraction de forme d'onde est
impossible (cohérence ~0, établi 2026-07).

## Protocole

Segment U7 30–120 s (chant + instrumental), 16 kHz (fs natif des modèles).
Classement par seconde : « chant » = U7 top 30 % ; « repisse pure » =
musique forte ET U7 sous sa médiane (28 s / 16 s). Modèles pré-entraînés
SANS adaptation : DTLN (~1 M, TFLite officiel), GTCRN (48 k, ONNX
streaming). Sorties d'écoute 48 k : `/home/michael/data/bench_vocal/out/`.

## Qualité (pré-entraînés parole, AUCUN fine-tuning chant/musique)

| Modèle | Repisse pure | Secondes de chant | Lecture |
|---|---|---|---|
| DTLN | **−15,7 dB** | −5,0 dB | bon contraste, voix modérément touchée |
| GTCRN | **−25,1 dB** | −12,8 dB | contraste énorme mais le CHANT est abîmé |

Nuance : les « secondes de chant » contiennent aussi de la repisse — une
partie de la baisse y est légitime. **L'oreille tranche** (fichiers 48 k
prêts : original / dtln / gtcrn).

## Board (i.MX8MP, benchmark_model TFLite 2.16, 200 runs)

| DTLN (les 2 étages / hop 8 ms) | étage 1 | étage 2 | total | verdict |
|---|---|---|---|---|
| CPU A53 (XNNPACK) | 0,82 ms | 1,42 ms | **2,24 ms** | RTF 0,28 sur UN cœur — temps réel confortable |
| NPU (VX delegate) | 1,73 ms | 1,84 ms | 3,57 ms | PLUS LENT que le CPU (LSTM mal mappés) + compilations initiales 0,25–1,15 s |

**Réponse à « le NPU pourra faire le mastering ET DTLN ? »** : oui en
capacité, mais c'est encore mieux — DTLN n'a pas BESOIN du NPU (mesuré :
le VX n'accélère pas ses LSTM). Il tournerait sur un cœur A53 (~28 %,
placement à choisir hors core 0/IRQ capture), le NPU reste dédié au
mastering (1,81 ms @100 Hz). GTCRN (convolutionnel) serait le candidat si
on voulait du NPU — conversion ONNX→TFLite à faire.

## Conclusions

1. Sur étagère, DTLN fait déjà un travail utilisable pour la MESURE
   (débiaiser Pv/act/gate de l'automix) ; pour l'AUDIO façade, aucun des
   deux n'est présentable sans fine-tuning chant (dataset dispo : voix
   studio propres + repisse synthétique).
2. Le sprint A (soustraction spectrale de PUISSANCE, sans ML) reste
   pertinent en premier étage de mesure — zéro dépendance, zéro modèle.
3. Écoute Michael requise avant d'engager : fichiers dans bench_vocal/out/.
