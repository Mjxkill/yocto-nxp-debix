#!/usr/bin/env python3
"""V5.14 — bench vitesse NPU pour plusieurs architectures (random init).

Export chaque variante → TFLite INT8 → SCP board → bench NPU latency.
But : choisir l'archi optimale pour le push 100 Hz (cycle 10 ms).
"""

import os, sys, time, subprocess, numpy as np, torch
import torch.nn as nn
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from model import N_FEATURES_IN_V2, N_PARAMS_OUT_V5_14

import litert_torch
from ai_edge_quantizer import Quantizer, qtyping

WORKSPACE = Path('/home/michael/data/mastering_workspace')
EXPORT_DIR = WORKSPACE / 'tflite_export'
EXPORT_DIR.mkdir(parents=True, exist_ok=True)

N_FRAMES = 19


# === Architectures ==========================================================

def model_S_relu(n_in=11, n_out=30):
    """Small ReLU : 64 → 128 channels."""
    return nn.Sequential(
        nn.Conv1d(n_in, 64, 5, padding=2), nn.ReLU(),
        nn.Conv1d(64, 128, 5, padding=2), nn.ReLU(),
        nn.AdaptiveAvgPool1d(1), nn.Flatten(),
        nn.Linear(128, n_out), nn.Sigmoid(),
    )

def model_M_relu(n_in=11, n_out=30):
    """Medium ReLU : 128 → 256."""
    return nn.Sequential(
        nn.Conv1d(n_in, 128, 5, padding=2), nn.ReLU(),
        nn.Conv1d(128, 256, 5, padding=2), nn.ReLU(),
        nn.AdaptiveAvgPool1d(1), nn.Flatten(),
        nn.Linear(256, n_out), nn.Sigmoid(),
    )

def model_L_relu(n_in=11, n_out=30):
    """Large ReLU 4 layers (current arch but ReLU + no BN)."""
    return nn.Sequential(
        nn.Conv1d(n_in, 128, 5, padding=2), nn.ReLU(),
        nn.Conv1d(128, 256, 5, padding=2), nn.ReLU(),
        nn.Conv1d(256, 256, 3, padding=1), nn.ReLU(),
        nn.Conv1d(256, 256, 3, padding=1), nn.ReLU(),
        nn.AdaptiveAvgPool1d(1), nn.Flatten(),
        nn.Linear(256, 256), nn.ReLU(),
        nn.Linear(256, n_out), nn.Sigmoid(),
    )

def model_L_gelu_bn(n_in=11, n_out=30):
    """Large GELU + BatchNorm (current v5.14)."""
    return nn.Sequential(
        nn.Conv1d(n_in, 128, 5, padding=2), nn.BatchNorm1d(128), nn.GELU(),
        nn.Conv1d(128, 256, 5, padding=2), nn.BatchNorm1d(256), nn.GELU(),
        nn.Conv1d(256, 256, 3, padding=1), nn.BatchNorm1d(256), nn.GELU(),
        nn.Conv1d(256, 256, 3, padding=1), nn.BatchNorm1d(256), nn.GELU(),
        nn.AdaptiveAvgPool1d(1), nn.Flatten(),
        nn.Linear(256, 256), nn.GELU(),
        nn.Linear(256, n_out), nn.Sigmoid(),
    )

def model_XL_relu(n_in=11, n_out=30):
    """Extra large ReLU 256→512 channels."""
    return nn.Sequential(
        nn.Conv1d(n_in, 256, 5, padding=2), nn.ReLU(),
        nn.Conv1d(256, 512, 5, padding=2), nn.ReLU(),
        nn.Conv1d(512, 512, 3, padding=1), nn.ReLU(),
        nn.AdaptiveAvgPool1d(1), nn.Flatten(),
        nn.Linear(512, 256), nn.ReLU(),
        nn.Linear(256, n_out), nn.Sigmoid(),
    )

def model_XXL_relu(n_in=11, n_out=30):
    """5M params : 512 → 1024 channels."""
    return nn.Sequential(
        nn.Conv1d(n_in, 512, 5, padding=2), nn.ReLU(),
        nn.Conv1d(512, 1024, 5, padding=2), nn.ReLU(),
        nn.Conv1d(1024, 1024, 3, padding=1), nn.ReLU(),
        nn.AdaptiveAvgPool1d(1), nn.Flatten(),
        nn.Linear(1024, 512), nn.ReLU(),
        nn.Linear(512, n_out), nn.Sigmoid(),
    )

def model_XXXL_relu(n_in=11, n_out=30):
    """10M+ params : 768 → 1536 channels."""
    return nn.Sequential(
        nn.Conv1d(n_in, 768, 5, padding=2), nn.ReLU(),
        nn.Conv1d(768, 1536, 5, padding=2), nn.ReLU(),
        nn.Conv1d(1536, 1536, 3, padding=1), nn.ReLU(),
        nn.AdaptiveAvgPool1d(1), nn.Flatten(),
        nn.Linear(1536, 768), nn.ReLU(),
        nn.Linear(768, n_out), nn.Sigmoid(),
    )

def model_MEGA_relu(n_in=11, n_out=30):
    """20M+ params : 1024 → 2048 channels."""
    return nn.Sequential(
        nn.Conv1d(n_in, 1024, 5, padding=2), nn.ReLU(),
        nn.Conv1d(1024, 2048, 5, padding=2), nn.ReLU(),
        nn.Conv1d(2048, 2048, 3, padding=1), nn.ReLU(),
        nn.AdaptiveAvgPool1d(1), nn.Flatten(),
        nn.Linear(2048, 1024), nn.ReLU(),
        nn.Linear(1024, n_out), nn.Sigmoid(),
    )

def model_DEEP_relu(n_in=11, n_out=30):
    """Deeper model : 6 layers of 384 channels."""
    return nn.Sequential(
        nn.Conv1d(n_in, 384, 5, padding=2), nn.ReLU(),
        nn.Conv1d(384, 384, 3, padding=1), nn.ReLU(),
        nn.Conv1d(384, 384, 3, padding=1), nn.ReLU(),
        nn.Conv1d(384, 384, 3, padding=1), nn.ReLU(),
        nn.Conv1d(384, 384, 3, padding=1), nn.ReLU(),
        nn.Conv1d(384, 384, 3, padding=1), nn.ReLU(),
        nn.AdaptiveAvgPool1d(1), nn.Flatten(),
        nn.Linear(384, 384), nn.ReLU(),
        nn.Linear(384, n_out), nn.Sigmoid(),
    )

# === Variantes Conv2D ======================================================
# L'input (B, 11, 19) est reshape en (B, 11, 1, 19) — features=channels,
# height=1, width=time. Convolutions appliquées sur l'axe temporel uniquement
# avec kernel (1, K) → mêmes maths que Conv1D mais layout NPU-friendly.

class Reshape2D(nn.Module):
    def forward(self, x): return x.unsqueeze(2)   # (B, C, T) → (B, C, 1, T)

def model_L_relu_conv2d(n_in=11, n_out=30):
    """L_relu mais Conv2D (1,K) au lieu de Conv1D (K)."""
    return nn.Sequential(
        Reshape2D(),
        nn.Conv2d(n_in, 128, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(128, 256, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(256, 256, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.Conv2d(256, 256, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.AdaptiveAvgPool2d((1, 1)), nn.Flatten(),
        nn.Linear(256, 256), nn.ReLU(),
        nn.Linear(256, n_out), nn.Sigmoid(),
    )

def model_XL_relu_conv2d(n_in=11, n_out=30):
    """XL_relu en Conv2D."""
    return nn.Sequential(
        Reshape2D(),
        nn.Conv2d(n_in, 256, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(256, 512, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.AdaptiveAvgPool2d((1, 1)), nn.Flatten(),
        nn.Linear(512, 256), nn.ReLU(),
        nn.Linear(256, n_out), nn.Sigmoid(),
    )

def model_XXL_relu_conv2d(n_in=11, n_out=30):
    """XXL_relu Conv2D (6.3M params)."""
    return nn.Sequential(
        Reshape2D(),
        nn.Conv2d(n_in, 512, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(512, 1024, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(1024, 1024, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.AdaptiveAvgPool2d((1, 1)), nn.Flatten(),
        nn.Linear(1024, 512), nn.ReLU(),
        nn.Linear(512, n_out), nn.Sigmoid(),
    )

def model_XXXL_relu_conv2d(n_in=11, n_out=30):
    """XXXL_relu Conv2D (14M params)."""
    return nn.Sequential(
        Reshape2D(),
        nn.Conv2d(n_in, 768, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(768, 1536, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(1536, 1536, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.AdaptiveAvgPool2d((1, 1)), nn.Flatten(),
        nn.Linear(1536, 768), nn.ReLU(),
        nn.Linear(768, n_out), nn.Sigmoid(),
    )

def model_MEGA_conv2d(n_in=11, n_out=30):
    """MEGA Conv2D (~25M params)."""
    return nn.Sequential(
        Reshape2D(),
        nn.Conv2d(n_in, 1024, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(1024, 2048, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(2048, 2048, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.AdaptiveAvgPool2d((1, 1)), nn.Flatten(),
        nn.Linear(2048, 1024), nn.ReLU(),
        nn.Linear(1024, n_out), nn.Sigmoid(),
    )

def model_GIGA_conv2d(n_in=11, n_out=30):
    """GIGA Conv2D (~50M params)."""
    return nn.Sequential(
        Reshape2D(),
        nn.Conv2d(n_in, 1536, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(1536, 3072, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(3072, 3072, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.AdaptiveAvgPool2d((1, 1)), nn.Flatten(),
        nn.Linear(3072, 1024), nn.ReLU(),
        nn.Linear(1024, n_out), nn.Sigmoid(),
    )

def model_DEEP_conv2d(n_in=11, n_out=30):
    """Deep Conv2D : 8 couches de 512 channels (~13M params, plus deep)."""
    return nn.Sequential(
        Reshape2D(),
        nn.Conv2d(n_in, 512, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
        nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
        nn.AdaptiveAvgPool2d((1, 1)), nn.Flatten(),
        nn.Linear(512, 512), nn.ReLU(),
        nn.Linear(512, n_out), nn.Sigmoid(),
    )

ARCHS = {
    'MEGA_conv2d':    model_MEGA_conv2d,
    'GIGA_conv2d':    model_GIGA_conv2d,
    'DEEP_conv2d':    model_DEEP_conv2d,
}


def export_int8(name, builder):
    out_f32  = EXPORT_DIR / f'sweep_{name}_f32.tflite'
    out_int8 = EXPORT_DIR / f'sweep_{name}_int8.tflite'

    model = builder()
    model.eval()
    n_params = sum(p.numel() for p in model.parameters())

    print(f"\n=== {name}  ({n_params:,} params) ===")

    # Export F32
    example = (torch.randn(1, N_FEATURES_IN_V2, N_FRAMES),)
    edge = litert_torch.convert(model, example)
    edge.export(str(out_f32))

    # Quantize INT8
    calib_data = np.random.randn(50, N_FEATURES_IN_V2, N_FRAMES).astype(np.float32)
    q = Quantizer(str(out_f32))
    for op in [qtyping.TFLOperationName.CONV_2D,
               qtyping.TFLOperationName.FULLY_CONNECTED,
               qtyping.TFLOperationName.MEAN]:
        q.add_static_config(
            regex='.*', operation_name=op,
            activation_num_bits=8, weight_num_bits=8,
            weight_granularity=qtyping.QuantGranularity.CHANNELWISE,
        )
    calib_iter = [{'args_0': calib_data[k:k+1]} for k in range(50)]
    calib_result = q.calibrate({'serving_default': calib_iter})
    q.quantize(calibration_result=calib_result, serialize_to_path=str(out_int8))

    sz_kb = out_int8.stat().st_size // 1024
    print(f"  → INT8 {sz_kb} KB ({n_params:,} params)")
    return out_int8, n_params, sz_kb


if __name__ == '__main__':
    results = []
    for name, builder in ARCHS.items():
        try:
            int8_path, n_params, sz_kb = export_int8(name, builder)
            results.append((name, n_params, sz_kb, int8_path))
        except Exception as e:
            print(f"  ✗ {name} FAILED: {e}")
            results.append((name, 0, 0, None))

    print("\n\n=== SUMMARY ===")
    print(f"{'name':<15s}  {'params':>10s}  {'INT8 KB':>10s}")
    for name, n, kb, _ in results:
        print(f"{name:<15s}  {n:>10,}  {kb:>10}")

    # Print SCP commands for board bench
    print("\n\n# SCP all to board :")
    for name, _, _, path in results:
        if path:
            print(f"scp {path} root@192.168.0.9:/tmp/")
