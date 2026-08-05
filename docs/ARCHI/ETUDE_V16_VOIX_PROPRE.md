# ÉTUDE V16 — « Voix propre » : bouton 3 positions sur la piste voix

Date : 2026-08-05/06. Demande Michael : un bouton sur la tranche voix avec
3 possibilités (brut / nettoyé A / nettoyé B), et vérifier sérieusement si
le modèle peut tourner sur NPU (préserver le CPU). **Étude AVANT codage**
(exigence explicite). Aucune ligne de code produit ici.

## 1. Ce qui est MESURÉ (bench 2026-08-05, board + host)

### Qualité (pré-entraînés parole, U7 LeftoverSalmon, sans adaptation)
| Modèle | Repisse pure | Chant | Écoute Michael |
|---|---|---|---|
| DTLN 1 M | −15,7 dB | −5,0 dB | en cours |
| GTCRN 48 k | −25,1 dB | −12,8 dB (abîmé) | en cours |

### Exécution board (i.MX8MP)
| | CPU A53 (XNNPACK) | NPU (VX delegate) |
|---|---|---|
| DTLN (2 étages/hop 8 ms) | **2,24 ms → 28 % d'UN cœur** | 3,57 ms — PLUS LENT (LSTM non mappés) |
| GTCRN | pas encore exécutable (cf. §2) | idem |

### Conversion GTCRN → TFLite (pour NPU) : tentée, ÉCHEC en l'état
- onnx2tf backend `flatbuffer_direct` : graphe invalide (Pad, TRANSPOSE
  cassé au chargement) ;
- backend `tf_converter` : échec MatMul_26 (réarrangement NCHW/NHWC).
- **Voie propre identifiée** : export TFLite depuis le PyTorch source via
  `ai-edge-torch` (le repo GTCRN fournit code + checkpoint) — tâche à
  budgéter (~½ à 1 journée), pas un quick-win. Alternative : INT8 par
  quantization calibrée sur nos WAV (le VIP8000 excelle en int8).

## 2. Réponse à « le NPU pourra faire mastering + modèle voix ? »

- **Capacité** : oui, large (mastering = 1,81 ms @100 Hz ≈ 8-18 %).
- **DTLN** : le NPU ne l'accélère PAS (mesuré) — s'il est retenu, il vit
  sur UN cœur A53 (28 %), hors core 0 (piège IRQ capture connu).
- **GTCRN** : 33 MMACs/s = ~20× plus léger que DTLN. Si l'export
  ai-edge-torch aboutit, il coûtera ~2-5 % d'UN cœur en CPU — le débat
  NPU devient presque sans objet ; et sa nature convolutionnelle en fait
  LE candidat NPU int8 si on veut viser 0 % CPU.
- **Le vrai levier** : si un fine-tuning chant s'impose de toute façon
  (probable, cf. GTCRN qui abîme le chant), on RE-ENTRAÎNE — et alors on
  choisit dès le départ : architecture conv-friendly, 48 kHz natif, export
  TFLite int8 propre. Le pipeline d'entraînement existe (mastering ML :
  PC → TFLite → NPU, validé). Dataset : voix studio propres du corpus +
  repisse synthétique (mélanges contrôlés).

## 3. Architecture du bouton 3 positions (étude, à valider)

### 3.1 UI / contrôle
- Tranche voix (rôles LEAD/CHŒURS) : bouton cyclique 3 états
  `BRUT / NET-A / NET-B` (web + LCD, parité).
- Op moteur : `set_voice_clean {src, mode}` (module, pattern V14),
  persisté comme réglage opérateur.

### 3.2 Insertion audio — LE point dur : la latence
- Modèles 16 kHz sur étagère : fenêtre 32 ms ⇒ **~32-40 ms de latence
  algorithmique** sur le canal traité + resampling 48↔16 k. INCOMPATIBLE
  avec la règle façade < 10 ms.
- Choix possibles (décision Michael) :
  a) mode réservé à la MESURE (débiaiser balance/gate — aucun impact
     latence, le bouton choisit l'estimateur) ;
  b) accepter une latence dédiée sur LE canal voix traité (à chiffrer à
     l'oreille : la voix 30 ms derrière le groupe, décalage perceptible) ;
  c) modèle custom 48 kHz à petite fenêtre (hop 2-6 ms, multiple de 96) —
     latence < 10 ms tenable, mais c'est le sprint re-entraînement.
- Intégration process : JAMAIS de TFLite dans mixer-pro (règle V9.5.12,
  freeze VX/galcore + RT99). Pattern éprouvé : daemon jumeau de
  mixer-ml-inference + ring SHM aller-retour (pattern midix inversé),
  coût IPC ≈ 1-2 périodes (2-4 ms) à inclure dans le budget.

### 3.3 Échelle
1 canal voix = 28 % d'un cœur (DTLN) — plusieurs micros chant = à
surveiller ; GTCRN-like custom = ~2-5 %/canal, extensible aux chœurs.

## 4. Feuille de route proposée (chaque porte = GO Michael)

| Porte | Contenu | Livrable |
|---|---|---|
| P1 | Écoute des 3 fichiers bench (fait sur U7) | verdict oreille : DTLN utilisable en mesure ? |
| P2 | Export GTCRN via ai-edge-torch + bench VX/int8 board | GTCRN exécutable ? coût réel CPU/NPU |
| P3 | Sprint A mesure (soustraction spectrale de puissance, sans ML) + branchement estimateur au choix (bouton côté MESURE) | balance/gate débiaisés sur micros ouverts |
| P4 | Décision latence (3.2 a/b/c) → si (c) : sprint entraînement custom 48 k (dataset synthétique, infra mastering) | modèle voix A.L.A. natif |
| P5 | Bouton 3 positions AUDIO (daemon + ring SHM + UI web/LCD) | fonctionnalité complète |
