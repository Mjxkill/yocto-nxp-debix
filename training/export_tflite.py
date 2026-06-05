#!/usr/bin/env python3
"""
V9.5.3 phase 8 — Export TFLite INT8 quantizé pour deploy NPU i.MX8MP.

Pipeline d'export :
    1. Load checkpoint PyTorch MasteringMLP
    2. Export ONNX (opset 13, input dynamic batch)
    3. ONNX → TFLite float32 (référence)
    4. ONNX → TFLite INT8 quantizé avec representative dataset
       (échantillons features du dataset utilisateur)

Compatibilité i.MX8MP NPU :
    - VX delegate via libvx_delegate.so (déjà dans image-full BSP NXP)
    - INT8 obligatoire pour acceleration NPU (sinon CPU fallback)
    - Opérations supportées : Linear, ReLU, Sigmoid (notre MLP simple)

Dépendances : onnx, onnxruntime, onnx_tf OU tensorflow (pour tflite convert)

Usage :
    python3 export_tflite.py --ckpt v1_epoch049 --calibration_samples 200
"""

import argparse
import os
from pathlib import Path

import numpy as np
import torch

from model import MasteringMLP, N_FEATURES_IN
from features import compute_features


WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT_DIR  = WORKSPACE / 'checkpoints'
EXPORT_DIR = WORKSPACE / 'exports'

SR = 48000


def export_onnx(ckpt_path: Path, onnx_path: Path):
    """Step 1 : PyTorch checkpoint → ONNX."""
    print(f"[1/3] Loading ckpt {ckpt_path.name}...")
    ckpt = torch.load(str(ckpt_path), map_location='cpu', weights_only=True)
    model = MasteringMLP()
    model.load_state_dict(ckpt['model'])
    model.eval()

    print(f"[2/3] Exporting ONNX → {onnx_path.name}...")
    dummy = torch.randn(1, N_FEATURES_IN)
    torch.onnx.export(
        model, dummy, str(onnx_path),
        input_names=['features'],
        output_names=['params_norm'],
        dynamic_axes={'features': {0: 'batch'},
                      'params_norm': {0: 'batch'}},
        opset_version=13,
        do_constant_folding=True,
    )
    print(f"    ONNX size: {onnx_path.stat().st_size // 1024} KB")


def build_calibration_set(n_samples: int = 200) -> np.ndarray:
    """Step 2 : extrait features de N samples du dataset pour calibration INT8.

    Format attendu par TFLite quantizer : tableau (n_samples, N_FEATURES_IN)
    float32.
    """
    from dataset_loader import iter_pairs, load_audio

    print(f"[calibration] Building set of {n_samples} feature vectors...")
    all_feats = []
    for i, pair in enumerate(iter_pairs()):
        if i >= n_samples:
            break
        try:
            raw, _ = load_audio(pair.raw_path)
        except Exception:
            continue
        # Truncate à 10s pour rapidité
        n_lim = 10 * SR
        raw = raw[:n_lim].astype(np.float32)
        feats = compute_features(raw, sr=SR)
        # Échantillonne quelques frames de chaque paire
        idx = np.random.choice(len(feats), size=min(5, len(feats)), replace=False)
        all_feats.append(feats[idx])
    calib = np.concatenate(all_feats, axis=0).astype(np.float32)
    print(f"    calibration set shape: {calib.shape}")
    return calib


def onnx_to_tflite_float32(onnx_path: Path, tflite_path: Path):
    """Step 3 : ONNX → TFLite float32 via onnx_tf.

    Note : si onnx_tf n'est pas dispo, fallback sur conversion TF SavedModel
    manuelle ou utiliser tflite-tools / ai-edge-litert.
    """
    try:
        from onnx_tf.backend import prepare
        import onnx
    except ImportError:
        print("  ⚠ onnx_tf non installé. pip install onnx_tf tensorflow")
        print("    fallback : export TF SavedModel via TF directement")
        return _onnx_to_tflite_via_tf2(onnx_path, tflite_path)

    print(f"[3/3a] ONNX → SavedModel...")
    onnx_model = onnx.load(str(onnx_path))
    tf_rep = prepare(onnx_model)
    sm_dir = tflite_path.parent / (tflite_path.stem + '_saved_model')
    tf_rep.export_graph(str(sm_dir))
    print(f"    SavedModel → {sm_dir}")

    print(f"[3/3b] SavedModel → TFLite float32...")
    import tensorflow as tf
    converter = tf.lite.TFLiteConverter.from_saved_model(str(sm_dir))
    tflite_model = converter.convert()
    tflite_path.write_bytes(tflite_model)
    print(f"    TFLite float32 size: {tflite_path.stat().st_size // 1024} KB")
    return sm_dir


def _onnx_to_tflite_via_tf2(onnx_path: Path, tflite_path: Path):
    """Alt path si onnx_tf manque. Voir docs ONNX/TFLite."""
    raise NotImplementedError(
        "Installer onnx_tf : pip install onnx_tf\n"
        "Ou utiliser ai-edge-litert (Google) pour conversion ONNX → TFLite."
    )


def tflite_quantize_int8(saved_model_dir: Path, calib: np.ndarray,
                         out_path: Path):
    """Step 4 : Quantize INT8 avec representative dataset.

    Le calib doit refléter la distribution réelle des features (au runtime
    NPU). On utilise un échantillon du dataset utilisateur.
    """
    print(f"[4/4] Quantizing INT8 with {len(calib)} calibration samples...")
    import tensorflow as tf

    def repr_dataset():
        for i in range(len(calib)):
            yield [calib[i:i+1].astype(np.float32)]

    converter = tf.lite.TFLiteConverter.from_saved_model(str(saved_model_dir))
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = repr_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type  = tf.int8
    converter.inference_output_type = tf.int8
    tflite_int8 = converter.convert()
    out_path.write_bytes(tflite_int8)
    print(f"    TFLite INT8 size: {out_path.stat().st_size // 1024} KB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ckpt', type=str, required=True)
    ap.add_argument('--calibration_samples', type=int, default=200)
    args = ap.parse_args()

    EXPORT_DIR.mkdir(parents=True, exist_ok=True)
    ckpt_path = CKPT_DIR / f"mlp_{args.ckpt}.pt"
    if not ckpt_path.exists():
        cands = list(CKPT_DIR.glob(f"mlp_*{args.ckpt}*.pt"))
        if not cands:
            raise FileNotFoundError(args.ckpt)
        ckpt_path = cands[0]

    onnx_path     = EXPORT_DIR / f"mlp_{args.ckpt}.onnx"
    tflite_path   = EXPORT_DIR / f"mlp_{args.ckpt}.tflite"
    tflite_int8   = EXPORT_DIR / f"mlp_{args.ckpt}.int8.tflite"

    export_onnx(ckpt_path, onnx_path)
    sm_dir = onnx_to_tflite_float32(onnx_path, tflite_path)
    calib = build_calibration_set(args.calibration_samples)
    tflite_quantize_int8(sm_dir, calib, tflite_int8)

    print()
    print("=== Export complete ===")
    print(f"  ONNX           : {onnx_path}")
    print(f"  TFLite float32 : {tflite_path}")
    print(f"  TFLite INT8    : {tflite_int8}  ← deploy NPU board")
    print()
    print("Deploy NPU :")
    print(f"  scp {tflite_int8} root@192.168.0.9:/usr/lib/mastering_npu.tflite")


if __name__ == '__main__':
    main()
