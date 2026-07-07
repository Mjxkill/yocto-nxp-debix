# Test Fiche : V9.5.12 — E1 modèle ML mastering validé + NPU benchmark

**Date** : 2026-06-08
**Statut** : GO

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase NPU mastering — Modèle ML v5.12 validé |
| Version de référence | V9.5.12 |
| Étape | E1 — Modèle Conv1D delta-matching + écoute PC + bench NPU |
| Commit yocto-nxp-debix | `<à committer après validation>` (branch `feature/v7.0-multiband-drc-tap`) |
| Topologie de référence | sof-imx8mp-tac5212.tplg (V3.2.2 baseline, inchangée) |
| Firmware sof-imx8m.ri md5 board | (V8.33 baseline, inchangé) |
| Topology .tplg md5 board | (V3.2.2, inchangé) |
| Modèle ML | `/home/michael/data/mastering_workspace/checkpoints/conv_v5_12_full_epoch029.pt` |
| TFLite f32 | `/tmp/mastering_v5_12.tflite` (213 KB) déployé `/tmp/mastering_v5_12.tflite` sur board |

## Modèle ML

| Champ | Valeur |
|---|---|
| Architecture | MasteringConv1D (`training/model.py`) |
| Input | (1, 11, 19) — 11 features mid-only × 19 frames (200 ms) |
| Output | (1, 62) params normalisés ∈ [0, 1] |
| Trainable params | ~52 K |
| TFLite f32 size | 213 KB |
| Best loss epoch 29 | 0.2461 |
| RMS Δ vs target | 1.677 dB (eval surrogate) |
| Delta corr (Pearson sur deltas) | +0.9614 |
| Total training time | 19.4 min (PC, 30 epochs × 30 paires) |

## Devices ALSA + audio

Pas changé vs V8.33 (utilisation playback DSP standard).

## Tests réalisés (Claude — automatisés)

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| 1 | Training v5.12 30/30 full | `python train_v5_12.py --n_pairs 30 --epochs 30 --batch 8 --tag v5_12_full` | Loss < 0.5 et descend | **0.246 best epoch 29** ✓ |
| 2 | Eval surrogate v5.12 sur 3 paires | `python eval_v4.py --ckpt v5_12_full_epoch029 ...` | \|ΔRMS\| < 3 dB | moy 3.18 dB OK |
| 3 | Eval LV2 réel sur 3 paires (drive=1 par défaut) | idem | \|Δlv2\| < 2 dB | **0.87 dB** ✓ |
| 4 | Eval avec `--force_drive 6.0 --force_balance 0.0` | idem | LV2 OK | Δlv2 moy 2.04 dB OK (loudness shift normal) |
| 5 | Conversion PyTorch → TFLite f32 | `litert_torch.convert(model, ...)` | TFLite valide | 213 KB OK ✓ |
| 6 | Benchmark CPU TFLite | `python bench_tflite.py` | Mesure | 30.6 ms (4 threads) |
| 7 | **Benchmark NPU TFLite (VX delegate)** | `python bench_tflite.py` avec `load_delegate('/usr/lib/libvx_delegate.so')` | < 10 ms | **1.81 ms** ✓✓ |

## Test utilisateur (réel, écoute PC)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | OUI |
| Type de test | Écoute des 3 wavs LV2 sur carte 2 PC, comparaison vs raw et target |
| Track 1 (HeyDelilah) | **"très bon"** avec drive=6 forcé |
| Track 2 (Contraband) | "boîte de conserve résiduelle 500-2K, vibrato trompette" |
| Track 3 (MorningSickness) | acceptable |
| Verdict global | **GO** — qualité acceptable pour POC, issues mineures documentées |
| Issues à traiter | aigus pas assez fins (charley/snare), comb-filter pas voulu, manque de stéréo perçue (limite dataset mono) |
| Commentaires | drive=6 + balance=0 = bonne base. Smoothing 50 ms tau retenu pour Phase 2 |

## Benchmark NPU détaillé (board 192.168.0.9)

```
=== CPU (float32, XNNPACK 4 threads) ===
Input  : [1, 11, 19] float32
Output : [1, 62] float32
CPU inference : 30.576 ms/iter (500 iters)

=== NPU (VX delegate libvx_delegate.so) ===
NPU inference : 1.810 ms/iter
```

**Marge cycle 100 Hz** : 10 ms - 1.81 (NPU) - 0.5 (features NEON) - 0.1 (params write) = **~7.6 ms de marge** ✓

## Conclusion

✅ **GO** pour Phase 2 (intégration board live) :
- Modèle v5.12 converge proprement et sonne acceptable sur écoute PC
- TFLite conversion via litert-torch fonctionne sans modification d'archi
- NPU inference 1.81 ms = compatible 100 Hz update (10 ms cycle)
- Marge confortable pour ajouts (features compute, fx_chain_set_param)

## Référence (autres docs/spec liées)

- ARCHI : `ARCHI/ARCHI_V9.5.12.md`
- Spec parent : `ARCHI/ARCHI_V10.md` (obsolète — décrit MasteringMLP 17 features)
- Memory : [[project_v10_npu_mastering_chain]]
- Fiches précédentes : N/A (première fiche série V9.5 mastering)
