# ARCHI V10 — NPU mastering autonome

> ⚠ **OBSOLÈTE depuis 2026-06-08** — Ce document décrit l'architecture
> initiale V9.5.3 (MasteringMLP 17 features). Le modèle a évolué vers
> **MasteringConv1D 11 features avec window 200 ms + loss delta-matching**.
>
> **Document à jour** : `ARCHI/ARCHI_V9.5.12.md`
>
> Conservé ici comme historique de la direction initiale et choix
> d'architecture parents (insert mixer-pro, smoothing externe, etc.).

**Branche** : `feature/v7.0-multiband-drc-tap` (commits V9.4 → V9.6)
**Date d'init** : 2026-06-05

## Objectif

Aboutissement du projet Debix Model AB : console de mixage audio
embarquée pilotée par un **ingé son ML** sur le NPU i.MX8MP. Le NPU
écoute le signal en temps réel et règle automatiquement une chaîne de
mastering (LV2 + DSP HiFi4 + TAC5212 codec) pour reproduire la qualité
d'un master humain.

## Pipeline complet

```
┌──────────────────────────────────────────────────────────────┐
│                     Board Debix Model AB                     │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  Inputs ─→ Mix (mixer-pro 26×8 sends → 4 bus FX → master)    │
│                                                              │
│       ↓ out_0, out_1 (master DSP stéréo)                     │
│                                                              │
│  ┌──── INSERT MASTERING ────────────────────────────────┐    │
│  │  LV2 chain (V9.4 — mixer-pro post-master) :          │    │
│  │    Para EQ x16 → Exciter → StereoTools → Limiter     │    │
│  │  (params NPU-pilotés, smoothing externe V9.4.1)      │    │
│  └──────────────────────────────────────────────────────┘    │
│                                                              │
│       ↓                                                      │
│                                                              │
│  ┌──── DSP HiFi4 (SOF) ────────────────────────────────┐     │
│  │  MULTIBAND_DRC2.0  (BYTES blob V9.4.3)              │     │
│  │  PGA2.0 Out volumes                                  │     │
│  │  (params NPU via /api/cmd set_alsa)                  │     │
│  └──────────────────────────────────────────────────────┘    │
│                                                              │
│       ↓                                                      │
│                                                              │
│  ┌──── TAC5212 codec ──────────────────────────────────┐     │
│  │  DRC + limiter hardware                             │     │
│  │  Biquads BQ1-12 par TAC                             │     │
│  │  (params NPU via /api/cmd set_tac_reg, V9.4.2)      │     │
│  └──────────────────────────────────────────────────────┘    │
│                                                              │
│       ↓                                                      │
│                                                              │
│  Speakers + UAC2 stems (out 2..17 dry, master non-affecté)   │
│                                                              │
│  ┌──── NPU (V9.6) ─────────────────────────────────────┐     │
│  │  TFLite VX delegate (libvx_delegate.so)             │     │
│  │  Boucle 5 Hz :                                       │     │
│  │   1. Lit features audio depuis tap DMA              │     │
│  │   2. MLP inference (86K params INT8) → 62 floats    │     │
│  │   3. Écrit params via :                              │     │
│  │       - set_insert_param (LV2)                       │     │
│  │       - set_alsa (DSP HiFi4)                        │     │
│  │       - set_tac_reg (TAC5212)                       │     │
│  └──────────────────────────────────────────────────────┘    │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

## API mixer-pro pour NPU

| Couche | Endpoint | Format |
|---|---|---|
| **LV2 bus FX** | `set_fx_engine`, `set_fx_param` | engine+uri / name+value |
| **LV2 insert chain** | `set_insert`, `set_insert_param`, `get_insert`, `insert_bypass` | plugins[] / slot+name+value |
| **DSP HiFi4 INTEGER** | `set_alsa`, `get_alsa` | numid/name + value |
| **DSP HiFi4 BYTES** | `set_alsa bytes` | numid + hex string (max 8192) |
| **TAC5212 codec** | `set_tac_reg`, `get_tac_reg` | tac (0..3) + reg + value |

Tous les params LV2 ont un **smoothing externe** (V9.4.1) : transition
`ctrl_target → ctrl_values` avec α=0.039 par cycle, 95% en ~150 ms.

## Pipeline training PC (V9.5)

```
┌─ Workspace : /home/michael/data/mastering_workspace/ ─────────┐
│                                                               │
│  Dataset utilisateur (~/data/dataset_mastering/) :            │
│    259 paires Pop (raw → mastered)                            │
│    Format 44.1 kHz / 16 bit / stéréo                          │
│    Resample → 48 kHz pour cohérence board                     │
│                                                               │
│  Pipeline :                                                   │
│    1. dataset_loader.py    Iter pairs, fix symlinks           │
│    2. features.py          17 floats / frame (port MATLAB)    │
│    3. surrogate_chain.py   PyTorch différentiable :           │
│       - surrogate_eq      (16 biquads peak)                   │
│       - surrogate_exciter (HPF + tanh)                        │
│       - surrogate_stereo  (M/S)                               │
│       - surrogate_limiter (envelope IIR + soft clip)          │
│       Total : 62 params dynamiques                            │
│    4. model.py             MLP 256×2 (86 334 params)          │
│       Input  : 17 features                                    │
│       Output : 62 params normalisés [0,1] (sigmoid)           │
│    5. train.py             Adam, loss combinée :              │
│       1·MSE_wav + 0.5·MSE_spec + 0.05·ΔRMS² + 0.05·Δcrest²    │
│    6. eval.py              Surrogate vs LV2 réel              │
│    7. export_tflite.py     ONNX → TFLite INT8                 │
│       Calibration : 200 samples features dataset              │
│    8. deploy_npu.py        scp + configure + loop inference   │
│                                                               │
│  Artefacts :                                                  │
│    cache/      pair_cache_5s.npz (RAW + tgt + features)       │
│    checkpoints/ mlp_<tag>_epoch<N>.pt                          │
│    logs/       train_<tag>.jsonl + .stdout.log                 │
│    exports/    mlp_<tag>.{onnx,tflite,int8.tflite}            │
│    outputs/    eval audio wavs                                │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

## Choix d'architecture validés (2026-06-05)

| Question | Décision | Justification |
|---|---|---|
| Cible insert | out_0 + out_1 DSP uniquement | Autres outputs (UAC2 stems, phone) restent dry |
| Composition chaîne | 4 plugins LV2 + DSP DRC + TAC limiter | Hybride : qualité hardware + flexibilité ML |
| Modélisation TAC pour PC | Surrogate Python | Reproductibilité PC ↔ board |
| Lock mix_block | Hold target_lock pendant tout l'insert | Évite race u-a-f sur swap chain |
| Buffers temp | Static BSS globaux | RT-safe, ~3 KB |
| MAX_CHAIN | 8 plugins | 5 cibles + marge |
| Modèle ML | MLP 256×2 | Cohérent MATLAB existant, quantizable INT8 |
| Approche training | Surrogate différentiable PyTorch | Backprop direct (vs finite diff lent) |
| Smoothing | Externe mixer-pro (V9.4.1, α=0.039) | NPU prédit target, plugin smooth |
| Plateforme training | PC host (Ubuntu 24.04) puis ASUS Ascent GX10 | Iteration rapide puis prod |
| Workspace storage | ~/data/mastering_workspace/ | Disque / à 94 %, ~/data 523 GB libre |

## Limitations connues V9.5.3

1. **Limiter surrogate symétrique** : attack/release fusionnés via IIR
   one-pole. Au deploy, le LSP Limiter réel applique le vrai release.
2. **Side band features énormes** : raw Cambridge mono → master stéréo
   crée des deltas side gigantesques (artifact dataset). À filtrer.
3. **Loss audio MSE** : non perceptuelle. Pas d'A-weighting ni de
   loudness LUFS. Acceptable POC.
4. **NPU tap = dummy POC** : features lues via API, pas vraie DMA
   tap reserved-memory. À compléter V9.6.x.
5. **Params LV2 mapping limité** : `apply_params_to_real_lv2` (eval.py)
   mappe les noms approximativement. Mismatch sur certains slots.

## Roadmap restante après V9.6

- **V9.7** : NPU tap DMA reserved-memory (kernel module imx-audio-tap
  active + DT update 2 carves)
- **V9.8** : systemd service npu-inference.service (start auto au boot)
- **V9.9** : Loss perceptuelle (LUFS, MFCC, A-weighting)
- **V10.0** : Validation listening test utilisateur + iteration

## Références mémoire

- [[project_v10_npu_mastering_chain]] : roadmap 8 étapes
- [[dataset_mastering_location]] : dataset utilisateur
- [[v7_archi_master_reference]] : ARCHI_V7.0.pdf de référence parent
- [[feedback_rt_optimization_mandatory]] : règle code RT block-based
- [[project_npu_non_negotiable]] : NPU = objectif central projet
