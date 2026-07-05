# TESTS V10-FX — Effets DSP 100 % par canal (multiband_drc)

Date : 2026-07-05 · ARCHI : ARCHI_V10_FX_PERCHANNEL.md (validé critic)
Demande : bypass multiband réellement par canal + bit-transparent, et tous
les paramètres GUI (crossover compris) par canal.

## Versions

| Artefact | Référence |
|---|---|
| Firmware SOF | sof/ commit `356b490cd`, zephyr.ri md5 `f8413ea74d7ee6e98274f96787d7d918`, version dmesg `2:10:0-248e3` |
| Topology | sof-imx8mp-tac5212-V7.0.m4 (CONTROLBYTES_MAX 6144), déployée |
| Codec GUIs | stripfx.js + beta.html : blob V3, normalisation 8 canaux, selfTest idempotence 2ᵉ tour |
| UI | XOVER par canal (natif + web), ACTIF/OFF = canal courant sur toutes les bandes |

## Changements firmware

1. `enabled` PAR CANAL (multiband_drc_generic s16/s32) — `state->processed`
   devient un bitmask par canal ; un canal OFF garde son délai (alignement)
   mais aucun gain calculé/appliqué. Avant : band-global lu sur ch0.
2. Bypass BIT-TRANSPARENT (multiband_drc_process) : tous canaux × toutes
   bandes OFF → default_pass, plus d'emphasis/crossover. Coût ≤ 32 lectures
   int / période 2 ms.
3. Blob V3 : section crossover PAR CANAL (8 × 6 biquads = 1344 o) après les
   drc_coef ; détection par arithmétique de taille, V1/V2 compat intacte ;
   MAX_BLOB_SIZE 6144.

## Tests board (empiriques, 2026-07-05 soir)

| Test | Résultat |
|---|---|
| Boot nouveau firmware | version `248e3` chargée, audio OK, 0 xrun |
| Blob V3 forgé (python, xover distincts par canal 150-500/4000-5750 Hz) écrit À CHAUD | `ok:true`, relecture intacte (size 3780, layout V3 détecté), **0 xrun**, audio continu |
| Blob V2 d'origine restauré | `ok:true` (2436 o) — état utilisateur préservé |

## Tests d'écoute à faire (utilisateur)

1. **OFF par canal** : effet DSP multiband ON avec compression audible sur
   voies 1 et 2 → OFF sur la voie 1 seule → la voie 1 doit redevenir brute,
   la voie 2 rester compressée. (Avant : OFF voie 1 coupait tout.)
2. **Bypass transparent** : tout OFF sur toutes les voies → bouger les
   knobs XOVER ne doit RIEN changer au son (avant : coloration audible).
3. **Crossover par canal** : XOVER low très différent entre voie 1 et
   voie 2 (ex 100 Hz vs 2 kHz) avec compression forte de la bande basse →
   effet clairement différent entre les deux voies.
4. Vérifier « APPLIQUER AU DSP » → ✓ + persistance après reboot
   (rémanence dsp-blobs, taille V3 ≤ 6144 OK).

## Test utilisateur : EN ATTENTE

## Notes

- Normalisation codec : tout blob multiband édité repasse en layout
  per-channel 8 entrées à l'application (voulu — « tout par canal »).
- selfTest : l'invariant devient l'idempotence du 2ᵉ tour pack∘parse
  (le 1ᵉʳ pack peut légitimement normaliser/grossir le blob).
- DRC1.0 standalone : per-channel params déjà OK (D3, chemin generic
  confirmé compilé — zephyr-sdk gcc, pas de __XCC__).
