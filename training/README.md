# V9.5+ — Pipeline training NPU mastering

Reproduit la chaîne LV2 du mixer-pro (board Debix Model AB) sur PC pour le
training ML du modèle NPU qui pilotera les params.

## Architecture cible

```
Board (mixer-pro v9.4.3+) :
  inputs → mix → out_0/out_1 → [INSERT chain LV2] → DSP → TAC5212 → speakers
                               Para EQ x16 + Exciter
                               + StereoTools + Limiter

PC training (Ubuntu 24.04, .venv-mastering) :
  wav input → lilv host → mêmes 4 plugins LV2 → wav output
                          mêmes URIs, mêmes ranges
                          (params variables = ce que le NPU apprend)
```

## Prérequis PC

```bash
sudo apt install lsp-plugins-lv2 calf-plugins liblilv-dev lilv-utils \
                 python3-pip python3-venv

python3 -m venv .venv-mastering
.venv-mastering/bin/pip install numpy scipy soundfile librosa torch torchaudio
```

## Vérification plugins

```bash
lv2ls | grep -iE 'para_equalizer_x16_stereo|Exciter|StereoTools|limiter_stereo'
```

Doit retourner les 4 URIs cibles :
- `http://lsp-plug.in/plugins/lv2/para_equalizer_x16_stereo`
- `http://calf.sourceforge.net/plugins/Exciter`
- `http://calf.sourceforge.net/plugins/StereoTools`
- `http://lsp-plug.in/plugins/lv2/limiter_stereo`

## Scripts

- `lv2_host.py` — wrapper Python ctypes autour de liblilv-0
- `chain_process.py` — POC : applique la chaîne sur un wav, écrit wav sortie
- `validate_equivalence.py` — V9.5.2 : compare board ↔ PC sur sample identique
- `dataset_loader.py` — V9.5.3 : load paires (raw, mastered) du dataset utilisateur
- `train.py` — V9.5.3 : PyTorch training loop (modèle CNN 1D dilaté style WaveNet)

## État

- V9.5.1 : POC reproduction LV2 PC ← EN COURS
- V9.5.2 : Validation équivalence board ↔ PC
- V9.5.3 : Pipeline training PyTorch
