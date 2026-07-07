# ARCHI V9.5.12 — NPU Mastering Conv1D delta-matching

**Branche** : `feature/v7.0-multiband-drc-tap`
**Date** : 2026-06-08
**Statut** : V5.12 modèle validé écoute PC + NPU benchmark validé. Phase 2 (intégration board) à attaquer.

## Objectif

Aboutir une chaîne de mastering audio pilotée par un modèle ML embarqué
NPU i.MX8MP. Le modèle prédit en temps réel les 62 paramètres d'une
chaîne LV2 (Para EQ x16 + Calf Exciter + Calf StereoTools + LSP Limiter)
pour reproduire la signature d'un master humain à partir d'un signal raw.

## Vue d'ensemble

```
┌──────────────────────────────────────────────────────────────────┐
│                          Board Debix Model AB                    │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│   Audio raw 2ch (USB stems / mics / NPU TAP IN raw)              │
│                  │                                               │
│                  ├──→ NPU TAP IN raw (carve mem)                 │
│                  │       │                                       │
│                  │       ▼                                       │
│                  │   ml_inference_thread (Phase 2, à venir) :    │
│                  │     - features C/NEON (FFT 1024, hop 512)     │
│                  │     - TFLite INT8 / float32 + VX delegate     │
│                  │     - 1.81 ms / inférence sur NPU (mesuré)    │
│                  │     - update 100 Hz (10 ms cycle)             │
│                  │       │                                       │
│                  │       ▼                                       │
│                  │   62 params LV2 → fx_chain_set_param          │
│                  │       (in-process, smoothing 50 ms tau)       │
│                  │       │                                       │
│                  ▼       ▼                                       │
│   ┌──── INSERT MASTERING (mixer-pro fx_chain) ─────────────┐     │
│   │  Slot 0 : LSP Para EQ x16 stereo                       │     │
│   │  Slot 1 : Calf Exciter                                 │     │
│   │  Slot 2 : Calf StereoTools                             │     │
│   │  Slot 3 : LSP Limiter Stereo                           │     │
│   └────────────────────────────────────────────────────────┘     │
│                  │                                               │
│                  ▼                                               │
│   Output 2ch → DSP TAC5212 → haut-parleurs                       │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

## Architecture modèle (V5.12)

### MasteringConv1D — `training/model.py`

```
Input  : (B, 11, T=19)   features mid-only, 19 frames = 200 ms
   ↓
Conv1D 11  → 64  (kernel 5, padding=2) + ReLU
Conv1D 64  → 128 (kernel 5, padding=2) + ReLU
AdaptiveAvgPool1d(1) → (B, 128)
Linear 128 → 62 + Sigmoid → params normalisés ∈ [0,1]
   ↓
denormalize_params() → dict utilisable par chain
```

- **~52K params** entraînables
- **TFLite f32 = 213 KB**, ~1.7M ops, 0.86M MACs
- **NPU benchmark** : 1.81 ms / inférence (mesuré board 2026-06-08)
- CPU XNNPACK 4 threads : 30.6 ms (trop lent pour 100 Hz)

### Features audio — `training/features.py`

11 features par frame mid-only :
- 5 énergies par bande dB (sub, low-mid, mid, high-mid, air)
- 5 centroïdes spectraux Hz par bande
- 1 niveau global mid dB

Frame FFT 1024 samples (21 ms window), hop 512 samples (10.7 ms → 93.75 Hz feature rate).

### Paramètres LV2 (62-D output) — `training/model.py`

| Slot | Paramètres | Range |
|---|---|---|
| EQ x16 | freq[16] + gain_db[16] + q[16] = 48 | freq 20–20k Hz, gain ±12 dB, Q 0.3–4 |
| Exciter | amount, drive, freq_hz, ceiling = 4 | amount 0–1, drive 1–6, **freq ≥ 5 kHz** (V5.9+), ceiling 0.5–1 |
| StereoTools | balance, mid_gain, side_gain, sm_swap = 4 | balance ±0.2, mid 0.7–1.4, side 0.5–1.7 |
| Limiter | threshold_db, ceiling_lin, attack_ms, release_ms, input_db, output_db = 6 | th -12–0 dB, atk 0.3–10, rel 10–200 ms |

## Loss V5.12 — `training/train_v5_12.py`

**Loss delta-matching** (ce que la chain doit ajouter == ce que le master humain ajoute) :

```
delta_out = bands_dB(output) - bands_dB(raw)    # 20 bandes
delta_tgt = bands_dB(target) - bands_dB(raw)
delta_out_c, delta_tgt_c = clamp(±20 dB)

L_delta_band  = MSE pondéré sur 20 bandes hybrides
                (×2 sur bandes 500-2K Hz pour éviter "boîte de conserve")
L_delta_corr  = 1 - pearson_corr(delta_out, delta_tgt)
L_rms         = (RMS_dB(output) - RMS_dB(target))²
L_crest       = (crest(output) - crest(target))²
L_temp_coh    = var(params_norm batch).mean()   # cohérence temporelle

total = L_rms/25 + L_delta_band/100 + L_delta_corr/1 + L_crest/100 + L_temp_coh/0.05
```

### 20 bandes hybrides V5.12

- 4 bandes log 20–500 Hz (sub + bass + low-mid)
- 4 bandes log 500–4000 Hz (mid + high-mid)
- 12 bandes linéaires 4000–20000 Hz à 1333 Hz chacune (présence + air détaillé)

## Pipeline training PC

```
Workspace : /home/michael/data/mastering_workspace/ (~/data ≥ 500 GB libre)

cache/    pair_cache_v5_5_200ms.npz   (chunks 200 ms, features pré-calc)
          pair_cache_v2_1s.npz       (chunks 1 s, V5.10 only)
checkpoints/ conv_v5_<x>_full_epoch<NNN>.pt
logs/     train_v5_<x>_full.jsonl + .stdout.log
exports/  mastering_v5_12.tflite (213 KB float32)
outputs/  eval audio wavs raw/target/lv2
```

## Dataset utilisateur

| Champ | Valeur |
|---|---|
| Path | `/home/michael/data/dataset_mastering/` |
| Origine | Cambridge Music Technology (mixs masterisés) |
| Format | 44.1 kHz / 16 bit / stéréo → resample à 48 kHz |
| Nombre paires | 259 (manifest.json) |
| Particularité | **Raw + target sont mono** (L=R parfait dans dataset) |
| Conséquence | Pas d'apprentissage stéréo possible → Haas/MultiSpread inutile pour ce dataset |

## Historique versions V5

| Version | Window | Loss | Bandes | RMS Δ | Notes |
|---|---|---|---|---|---|
| v5.4 | 1 s | norm. (RMS+corr+crest) | 5 Pearson | 1.97 | Baseline 1 s, corr abs |
| v5.5 | **200 ms** | idem | 5 Pearson | 1.60 | Window courte, breakthrough |
| v5.6 | 200 ms | idem | 20 log | (saturé) | corr Pearson saturée à +0.99 |
| v5.7 | 200 ms | MSE per band | 20 log | ÷ (diverge) | bandes basses → silence → log diverge |
| v5.8 | 200 ms | corr Pearson | 20 hybrides (6 log + 14 lin) | 1.46 | Premier hybride |
| **v5.9** | 200 ms | **delta-matching** | 20 hybrides | 1.57 | Loss MSE sur delta vs raw + exciter ≥ 5 kHz |
| v5.10 | 1 s | delta-matching | 20 hybrides | 1.92 | Window 1 s = trop, retour 200 ms |
| v5.11 | 200 ms | delta-matching | 20 hybrides | 1.51 | v5.9 fresh retrain |
| **v5.12** | 200 ms | **delta-match + pondération 500-2K + temp coh** | **bandes fines aigus (4+4+12)** | **1.68** | ← validé écoute, exciter forcé drive=6 + balance=0 |

## Scripts clés

| Script | Rôle |
|---|---|
| `training/train_v5_12.py` | Training principal (loss V5.12) |
| `training/model.py` | Définition MasteringConv1D + PARAM_RANGES |
| `training/features.py` | FFT 1024 + 11 features mid-only |
| `training/surrogate_chain.py` | Chain PyTorch différentiable pour backprop |
| `training/dataset_loader.py` | Iter paires Cambridge, load_audio resample |
| `training/eval_v4.py` | Eval surrogate + chain LV2 réelle, force_drive, force_balance |
| `training/lv2_chain.py` | Wrapper ctypes pour `lv2_chain_host.so` |
| `training/diag_v5_8.py` | Diag stats EQ par décade (gain moyen) |

## Commandes de référence

### Training v5.12 full

```bash
cd /home/michael/yocto-nxp-debix/training
/home/michael/yocto-nxp-debix/.venv-mastering/bin/python train_v5_12.py \
    --n_pairs 30 --epochs 30 --batch 8 --tag v5_12_full
```

→ ~20 min sur PC. Best ckpt = epoch ~29.

### Eval audio LV2 réel

```bash
/home/michael/yocto-nxp-debix/.venv-mastering/bin/python eval_v4.py \
    --ckpt v5_12_full_epoch029 --n_pairs 3 --seconds -1 \
    --cap_eq_db 12 --cap_limiter_in_db 12 \
    --force_drive 6.0 --force_balance 0.0
```

→ Wavs écrits dans `/home/michael/data/mastering_workspace/outputs/`.

### Export TFLite (PyTorch → TFLite via litert-torch)

```python
import torch, litert_torch
from model import MasteringConv1D
ckpt = torch.load('conv_v5_12_full_epoch029.pt')
model = MasteringConv1D(n_input=11); model.load_state_dict(ckpt['model']); model.eval()
edge = litert_torch.convert(model, (torch.randn(1, 11, 19),))
edge.export('mastering_v5_12.tflite')
```

### Benchmark NPU sur board

```bash
scp /tmp/mastering_v5_12.tflite root@192.168.0.9:/tmp/
ssh root@192.168.0.9 'python3 /tmp/bench_tflite.py'
# CPU 30.6 ms vs NPU 1.81 ms (VX delegate)
```

## Validations utilisateur écoute V5.12 (2026-06-08)

| Track | Drive=1 (prédit) | Drive=6 (forcé) | Verdict utilisateur |
|---|---|---|---|
| HeyDelilah | "moins pâteux, manque air" | "très bon" | drive=6 retenu |
| Contraband | "boîte de conserve, vibrato trompette" | "trop d'aigus 1k-3k" | acceptable |
| MorningSickness | OK | OK | |

**Issues identifiées non corrigées V5.12** :
- Aigus charley/snare pas assez fins
- Boîte de conserve 500-2K diminuée mais pas éliminée
- Vibrato trompette (dû à update params chaque chunk 200 ms)
- Manque de stéréo perçue (dataset Cambridge **mono** → impossible à apprendre)

## Phase 2 — Intégration board (à venir)

| # | Brique | Cost |
|---|---|---|
| 1 | Export TFLite quantizé INT8 (calibration features réelles) | 1 j |
| 2 | Module C `ml_features.c` : FFT 1024 NEON + bandes + centroïdes | 2 j |
| 3 | Thread `ml_inference_thread.c` dans mixer-pro : NPU TAP IN → features → TFLite → fx_chain_set_param | 2 j |
| 4 | NPU TAP DT update : ajouter carve "IN raw" en plus de "OUT post-FX" | 1 j |
| 5 | UI dropdown "Mixer Assistant" (passthrough / mastering / future) | 2 j |
| 6 | Dashboard live params (62 ctrls) dans mixer-gui-http | 1-2 j |

**Total Phase 2 estimé : ~1.5 semaine.**

## Limitations connues V9.5.12

1. **Dataset Cambridge mono** : pas de signal stéréo dans raw ni target → impossible apprendre largeur stéréo
2. **Surrogate vs LV2 réel** : surrogate Python pour gradient backprop != chain LV2 réelle. Eval LV2 dévie ~2 dB du target loudness sans `--force_drive`.
3. **Smoothing 50 ms** : compromis vibrato/réactivité. Pour transients rapides (drums), peut sembler "molasse".
4. **Pas de loss perceptuelle** : MSE+corr en dB. Pas LUFS, pas A-weighted. Suffisant pour POC mais pas niveau production.
5. **Update 100 Hz simulé** : actuellement params statiques en eval. Phase 2 = vraie inférence rolling window.

## Références mémoire

- [[project_v10_npu_mastering_chain]] : roadmap globale 7 étapes
- [[dataset_mastering_location]] : Cambridge 259 paires
- [[v7_archi_master_reference]] : ARCHI_V7.0.pdf — parent ref
- [[project_npu_non_negotiable]] : NPU = objectif central
- [[feedback_test_fiches]] : règle TESTS_V<x>_E<y>.md à chaque step
- [[feedback_architecture_doc_per_phase]] : règle ARCHI_V<x>.md avant code

## Fichiers obsolètes

- `ARCHI/ARCHI_V10.md` (2026-06-05) : décrit MasteringMLP 17 features, obsolète depuis V5.4 (Conv1D 11 features). À déprécier ou marquer ARCHIVED.
