# TESTS V13-EQ — Égaliseur paramétrique TAC (web + LCD) + FFT de voie

**Date** : 2026-07-08
**Branche** : `L6.6.36-2.1.0-debix_model_ab`
**Commits** : `8d6f003b` (panneau web), `9bd2935c` (panneau natif LCD),
`38e3dd6a` (FFT lissée), + fix whitelist `/api/alsa/set` (ce commit)

## Objet

Remplacement des champs biquad bruts par un égaliseur paramétrique pro dans
le tiroir de tranche (onglet TAC BIQUADS), web et LCD :
courbe |H(f)| interactive (drag poignées, log 20 Hz–20 kHz, ±18 dB),
champs/knobs F-G-Q par bande, FFT temps réel de la voie en fond
(tap 2 analyzer, INPUT pour strips IN / OUTPUT pour strips OUT).

## Binaires déployés

| Fichier | md5 |
|---|---|
| `/usr/bin/mixer-console` | `84241d918bbd520da7cba88b4b527580` |
| `/usr/bin/mixer-gui-http` | `d3e4d2907c07be20404391074fde18ad` |
| `/var/www/mixer-gui/beta.html` | `c93fbc153132055807fa5ad7b25a70e7` |

Firmware SOF / topology : **inchangés** (feature 100 % TAC + GUI).

## Règles matérielles établies (empirique, board)

1. **CH1 = BQ1/5/9, CH2 = BQ2/6/10** (modulo 4, BQ3/4/7/8/11/12 morts) —
   déjà connu côté DAC (V11), désormais appliqué côté ADC.
2. **'3 Biquads/Ch' OBLIGATOIRE** : à '2 Biquads/Ch' (état d'usine du boot),
   un blob écrit dans ADC BQ1 est bien dans le silicium (vérifié par
   relecture I2C page 8) mais **n'agit pas** sur le canal. Le passage de
   DSP_CFG0 à 3/Ch (bits 3:2 = 11) rend le filtre actif. Posé sur les
   4 TACs + `alsactl store` le 2026-07-08.
3. **Q1.31 : aucun coefficient > 1** → pas de boost « pur » représentable
   au-delà de l'unité ; les types shelf/peak gain > 0 sont écrêtés par
   `rbjBlob` (héritage V11, N1/D1 stockés ÷2).
4. Côté **DAC (sorties)** : BQ5/9 et BQ6/10 appartiennent à l'anti-larsen →
   une seule bande utilisateur (BQ1/BQ2).

## Bugs trouvés/corrigés pendant la validation

- **`amixer_value_safe()` refusait `/`** → « 3 Biquads/Ch » rejeté en 503
  silencieux, le forçage ensure3() des GUIs n'atteignait jamais le TAC.
  Fix : `/` ajouté à la whitelist (fork+exec sans shell, sans risque).
- **FFT saccadée** : valeurs brutes posées à 5 Hz → ballistique par frame
  (attaque τ30 ms / retombée τ120 ms), recette SpectrumView (`38e3dd6a`).
- **Clavier de nommage scènes** : `modelData` du Repeater interne masquait
  la rangée → touches cassées + 37 TypeError au boot (fix dans `9bd2935c`).
- **Hôte build** : tar Ubuntu 0.2 casse pseudo → tar-native + symlink
  hosttools (voir mémoire `host_tar_pseudo_fix`).

## Résultats

- Écriture blob GUI → ALSA → driver → **silicium vérifiée** (relecture I2C
  page 8 = blob exact).
- Courbe/poignées/champs synchronisés, web (capture CDP) et LCD.
- FFT de voie fluide après lissage.
- **Atténuation large bande sur BQ1 de M1 audible et visible sur la FFT**
  après passage à 3 Biquads/Ch.

## Test utilisateur : **OUI** — « la ça marche ! » (2026-07-08)

## Restes / suites possibles

- EQ > 3 bandes par voie = hors TAC (option mixer-pro C natif, ou SOF
  `eq_iir`) — décision utilisateur en attente.
- Captures du panneau EQ à ajouter au MANUEL_UTILISATEUR.
