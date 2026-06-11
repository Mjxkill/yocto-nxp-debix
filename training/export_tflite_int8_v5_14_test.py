#!/usr/bin/env python3
"""V9.5.12 Step B : Export TFLite INT8 v5.12 via ai-edge-quantizer.

Approche en 2 temps :
  1. PyTorch model → TFLite float32 via litert-torch (déjà fait)
  2. TFLite float32 → TFLite INT8 via ai-edge-quantizer + calibration sur
     features réelles du cache 200ms

Usage :
    python3 export_tflite_int8_v5_12.py
"""

from pathlib import Path
import sys
import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).parent))
from model import MasteringConv1DLargeV5_14, N_FEATURES_IN_V2


WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT_DIR  = WORKSPACE / 'checkpoints'
CACHE_DIR = WORKSPACE / 'cache'
EXPORT_DIR = WORKSPACE / 'exports'
EXPORT_DIR.mkdir(parents=True, exist_ok=True)

CKPT_TAG  = 'v5_12_full_epoch029'
N_FRAMES  = 19
N_CALIB   = 200


def step1_pytorch_to_tflite_f32(out_path: Path):
    """PyTorch → TFLite float32 via litert-torch."""
    ckpt = torch.load(str(CKPT_DIR / f"conv_{CKPT_TAG}.pt"),
                      map_location='cpu', weights_only=True)
    model = MasteringConv1DLargeV5_14(n_input=N_FEATURES_IN_V2)
    model.load_state_dict(ckpt['model'])
    model.eval()

    import litert_torch
    edge = litert_torch.convert(model, (torch.randn(1, 11, N_FRAMES),))
    edge.export(str(out_path))
    print(f"  TFLite float32 : {out_path.stat().st_size//1024} KB")
    return model


def step2_quantize_int8(f32_path: Path, int8_path: Path, calib: np.ndarray):
    """TFLite float32 → TFLite INT8 via ai-edge-quantizer."""
    from ai_edge_quantizer import Quantizer, qtyping

    q = Quantizer(str(f32_path))

    # Quantize Conv2D + FullyConnected + Mean (les ops principaux du modèle)
    # Activation INT8 + Weight INT8 channelwise
    for op in [qtyping.TFLOperationName.CONV_2D,
               qtyping.TFLOperationName.FULLY_CONNECTED,
               qtyping.TFLOperationName.MEAN]:
        q.add_static_config(
            regex='.*',
            operation_name=op,
            activation_num_bits=8,
            weight_num_bits=8,
            weight_granularity=qtyping.QuantGranularity.CHANNELWISE,
        )

    # Calibration data en format expected : dict signature → iterable de dicts
    # Pour un single-signature model on utilise 'serving_default'
    print(f"  Calibrating with {len(calib)} samples...")
    calib_iter = [{'args_0': calib[k:k+1]} for k in range(len(calib))]
    calib_result = q.calibrate({'serving_default': calib_iter})

    # Quantize + serialize
    print("  Quantizing + serializing...")
    res = q.quantize(calibration_result=calib_result,
                     serialize_to_path=str(int8_path))
    sz = int8_path.stat().st_size
    print(f"  TFLite INT8 : {sz//1024} KB")


def step3_compare(model: MasteringConv1DLargeV5_14, f32_path: Path, int8_path: Path,
                  calib: np.ndarray):
    """Compare output PyTorch float32 vs TFLite INT8."""
    import litert_torch
    # Load INT8 via litert_torch.load
    int8_model = litert_torch.load(str(int8_path))
    f32_model_l = litert_torch.load(str(f32_path))

    y_torch = model(torch.from_numpy(calib).float()).detach().numpy()
    y_int8  = []
    y_f32   = []
    for k in range(len(calib)):
        out_f32  = f32_model_l(calib[k:k+1])
        out_int8 = int8_model(calib[k:k+1])
        # litert_torch.load returns a tuple/list ; first element is the output tensor
        y_f32.append(np.asarray(out_f32[0]  if isinstance(out_f32,  (list, tuple)) else out_f32).reshape(1, -1))
        y_int8.append(np.asarray(out_int8[0] if isinstance(out_int8, (list, tuple)) else out_int8).reshape(1, -1))
    y_int8 = np.concatenate(y_int8, axis=0)
    y_f32  = np.concatenate(y_f32,  axis=0)

    d_int8 = np.abs(y_int8 - y_torch)
    d_f32  = np.abs(y_f32  - y_torch)
    print()
    print(f"  PyTorch vs TFLite f32  : mean={d_f32.mean():.5f}  max={d_f32.max():.5f}")
    print(f"  PyTorch vs TFLite INT8 : mean={d_int8.mean():.5f}  max={d_int8.max():.5f}")
    print(f"  TFLite f32 vs INT8     : mean={np.abs(y_int8 - y_f32).mean():.5f}  max={np.abs(y_int8 - y_f32).max():.5f}")


def main():
    # Build calibration set
    cache = np.load(str(CACHE_DIR / 'pair_cache_v5_5_200ms.npz'), allow_pickle=True)
    pairs = list(cache['pairs'])
    np.random.seed(42)
    idx = np.random.choice(len(pairs), size=N_CALIB, replace=False)
    calib_list = []
    for i in idx:
        feats = pairs[i]['features']                     # (N_frames, 11)
        ft = np.asarray(feats, dtype=np.float32).T.reshape(1, 11, -1)   # (1, 11, Nf)
        Nf = ft.shape[-1]
        if Nf >= N_FRAMES:
            ft = ft[..., :N_FRAMES]
        else:
            ft = np.pad(ft, ((0,0),(0,0),(0, N_FRAMES-Nf)))
        calib_list.append(ft)
    calib = np.concatenate(calib_list, axis=0).astype(np.float32)
    print(f"Calibration set : {calib.shape}")

    f32_path  = EXPORT_DIR / f"mastering_{CKPT_TAG}.tflite"
    int8_path = EXPORT_DIR / f"mastering_{CKPT_TAG}_int8.tflite"

    print()
    print("[1/3] PyTorch → TFLite float32 (litert-torch)")
    model = step1_pytorch_to_tflite_f32(f32_path)

    print()
    print("[2/3] TFLite f32 → TFLite INT8 (ai-edge-quantizer + calibration)")
    step2_quantize_int8(f32_path, int8_path, calib)

    print()
    print("[3/3] Compare PyTorch vs TFLite f32 vs TFLite INT8")
    step3_compare(model, f32_path, int8_path, calib)


if __name__ == '__main__':
    main()
