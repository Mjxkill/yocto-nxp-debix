#!/usr/bin/env python3
"""V5.14 — export TFLite INT8 d'un modèle RANDOM INIT pour bench NPU timing.

Le seul but est de mesurer le temps d'inférence du MasteringConv1DLargeV5_14
sur NPU i.MX8MP. Les valeurs en sortie ne servent à rien (poids non
entraînés), donc on saute l'étape de calibration sophistiquée.
"""

import os, sys, numpy as np, torch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from model import MasteringConv1DLargeV5_14, N_FEATURES_IN_V2, N_PARAMS_OUT_V5_14

import litert_torch
from ai_edge_quantizer import Quantizer, qtyping

WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT_DIR  = WORKSPACE / 'checkpoints'
EXPORT_DIR = WORKSPACE / 'tflite_export'
EXPORT_DIR.mkdir(parents=True, exist_ok=True)

N_FRAMES = 19   # 19 × 10.7 ms = 200 ms context

OUT_F32  = EXPORT_DIR / 'mastering_v5_14_random_f32.tflite'
OUT_INT8 = EXPORT_DIR / 'mastering_v5_14_random_int8.tflite'

# Step 1 : random init + export F32
print("Step 1 : random init + export TFLite F32")
model = MasteringConv1DLargeV5_14(n_input=N_FEATURES_IN_V2)
model.eval()
example = (torch.randn(1, N_FEATURES_IN_V2, N_FRAMES),)
edge = litert_torch.convert(model, example)
edge.export(str(OUT_F32))
print(f"  → {OUT_F32.name} : {OUT_F32.stat().st_size//1024} KB")

# Step 2 : INT8 quantization (calibration random)
print("Step 2 : INT8 quantization (calibration 200 samples random)")
calib_data = np.random.randn(200, N_FEATURES_IN_V2, N_FRAMES).astype(np.float32)

q = Quantizer(str(OUT_F32))
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
calib_iter = [{'args_0': calib_data[k:k+1]} for k in range(200)]
calib_result = q.calibrate({'serving_default': calib_iter})
q.quantize(calibration_result=calib_result, serialize_to_path=str(OUT_INT8))
print(f"  → {OUT_INT8.name} : {OUT_INT8.stat().st_size//1024} KB")
print(f"OK — copy to board:  scp {OUT_INT8} root@192.168.0.9:/tmp/")
