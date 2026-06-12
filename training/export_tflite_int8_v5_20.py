#!/usr/bin/env python3
"""V5.20 — export TFLite INT8 du checkpoint final (mono, 74 outputs = env 64 + exciter 4 + limiter 6).

PyTorch ckpt ep29 → TFLite F32 (litert-torch) → INT8 (ai-edge-quantizer,
calibration 200 chunks réels du cache).
"""
import sys, numpy as np, torch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from model import MasteringXXL_conv2d, N_PARAMS_OUT_V5_19
from features_v3 import N_FEATURES_V3

import litert_torch
from ai_edge_quantizer import Quantizer, qtyping

WORKSPACE  = Path('/home/michael/data/mastering_workspace')
CKPT       = WORKSPACE / 'checkpoints' / 'conv_v5_20_full_epoch029.pt'
CACHE      = WORKSPACE / 'cache' / 'pair_cache_v5_18_mel125_100ms.npz'
EXPORT_DIR = WORKSPACE / 'tflite_export'
EXPORT_DIR.mkdir(parents=True, exist_ok=True)

N_FRAMES = 10   # 10 trames × 10 ms = fenêtre 100 ms

OUT_F32  = EXPORT_DIR / 'mastering_v5_20_f32.tflite'
OUT_INT8 = EXPORT_DIR / 'mastering_v5_20_int8.tflite'

# Step 1 : load ckpt + export F32
print('Step 1 : load checkpoint + export TFLite F32')
model = MasteringXXL_conv2d(n_input=N_FEATURES_V3, n_out=N_PARAMS_OUT_V5_19)
ckpt = torch.load(str(CKPT), map_location='cpu', weights_only=False)
model.load_state_dict(ckpt['model'])
model.eval()
example = (torch.randn(1, N_FEATURES_V3, N_FRAMES),)
edge = litert_torch.convert(model, example)
edge.export(str(OUT_F32))
print(f'  → {OUT_F32.name} : {OUT_F32.stat().st_size//1024} KB')

# Step 2 : calibration avec 200 chunks RÉELS du cache (features Mel+MFCC)
print('Step 2 : INT8 quantization (calibration 200 chunks réels)')
data = np.load(str(CACHE), allow_pickle=True)
pairs = data['pairs']
rng = np.random.default_rng(7)
calib = []
for idx in rng.permutation(len(pairs)):
    p = pairs[idx]
    rms = np.sqrt((p['raw'] ** 2).mean()) + 1e-12
    if 20 * np.log10(rms) < -35.0:        # mêmes filtres qu'au training
        continue
    f = p['features']                      # (T, 85)
    if f.shape[0] < N_FRAMES:
        continue
    calib.append(f[:N_FRAMES].T[None].astype(np.float32))   # (1, 85, 10)
    if len(calib) == 200:
        break
print(f'  calibration : {len(calib)} chunks réels')

q = Quantizer(str(OUT_F32))
for op in [qtyping.TFLOperationName.CONV_2D,
           qtyping.TFLOperationName.FULLY_CONNECTED,
           qtyping.TFLOperationName.MEAN]:
    q.add_static_config(
        regex='.*', operation_name=op,
        activation_num_bits=8, weight_num_bits=8,
        weight_granularity=qtyping.QuantGranularity.CHANNELWISE,
    )
calib_iter = [{'args_0': c} for c in calib]
calib_result = q.calibrate({'serving_default': calib_iter})
q.quantize(calibration_result=calib_result, serialize_to_path=str(OUT_INT8))
print(f'  → {OUT_INT8.name} : {OUT_INT8.stat().st_size//1024} KB')

# Step 3 : vérif parité F32 PyTorch vs INT8 TFLite sur 20 chunks
print('Step 3 : parité PyTorch F32 vs TFLite INT8')
from ai_edge_litert.interpreter import Interpreter
itp = Interpreter(model_path=str(OUT_INT8))
itp.allocate_tensors()
inp = itp.get_input_details()[0]
out = itp.get_output_details()[0]
errs = []
for c in calib[:20]:
    with torch.no_grad():
        ref = model(torch.from_numpy(c)).numpy()[0]
    itp.set_tensor(inp['index'], c)
    itp.invoke()
    got = itp.get_tensor(out['index'])[0]
    errs.append(np.abs(ref - got))
errs = np.stack(errs)
print(f'  |Δ| outputs normalisés : mean {errs.mean():.4f}  max {errs.max():.4f}')
print(f'  (sortie ∈ [0,1] ; <0.03 = OK pour INT8)')
print('DONE')
