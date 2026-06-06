#!/usr/bin/env python3
"""
V9.5.3-v2 — Diagnostic : pourquoi le RMS delta stagne ?

Charge le checkpoint smoke v2, run sur quelques chunks, affiche :
  - Distribution params_norm (sigmoid output) : tous proches de 0.5 ?
  - Params denormalized : valeurs réelles
  - RMS delta input → surrogate output → vs target
  - Identification : modèle prédit defaults ? gradient nul ?
"""

import numpy as np
import torch
from pathlib import Path

from features import compute_features_mid_only, N_FEATURES_MID_ONLY
from model import MasteringConv1D, denormalize_params, N_FEATURES_IN_V2, N_PARAMS_OUT, PARAM_LAYOUT
from surrogate_chain import MasteringChainSurrogate


WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT_PATH = WORKSPACE / 'checkpoints' / 'conv_v4_full_epoch029.pt'
CACHE_PATH = WORKSPACE / 'cache' / 'pair_cache_v2_1s.npz'

DEVICE = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
SR = 48000


def rms_db(x):
    return 20 * np.log10(np.sqrt(np.mean(x**2)) + 1e-12)


def main():
    print(f"Loading ckpt {CKPT_PATH.name}...")
    ckpt = torch.load(str(CKPT_PATH), map_location=DEVICE, weights_only=True)
    model = MasteringConv1D(n_input=N_FEATURES_IN_V2).to(DEVICE)
    model.load_state_dict(ckpt['model'])
    model.eval()
    print(f"  epoch {ckpt['epoch']}, loss {ckpt['loss']:.4f}")

    chain = MasteringChainSurrogate(sr=SR).to(DEVICE)
    chain.eval()

    print(f"\nLoading cache {CACHE_PATH.name}...")
    d = np.load(str(CACHE_PATH), allow_pickle=True)
    pairs = list(d['pairs'])
    print(f"  {len(pairs)} chunks available")

    np.random.seed(42)
    sample_idx = np.random.choice(len(pairs), size=10, replace=False)

    # ===== Diag 1 : distribution params_norm =====
    print("\n=== Diag 1 : distribution params_norm (sigmoid output 0..1) ===")
    all_norm = []
    for idx in sample_idx:
        p = pairs[idx]
        feats = torch.from_numpy(p['features']).to(DEVICE)
        feats = feats.permute(1, 0).unsqueeze(0)   # (1, 11, N_frames)
        with torch.no_grad():
            params_norm = model(feats)[0].cpu().numpy()
        all_norm.append(params_norm)
    all_norm = np.stack(all_norm)   # (10, 62)
    print(f"  Across 10 samples, per-param stats :")
    print(f"  {'param':30s}  {'mean':>8s}  {'std':>8s}  {'min':>8s}  {'max':>8s}")
    print(f"  {'-'*30}  {'-'*8}  {'-'*8}  {'-'*8}  {'-'*8}")
    for name, idx in PARAM_LAYOUT.items():
        if isinstance(idx, slice):
            vals = all_norm[:, idx].flatten()
        else:
            vals = all_norm[:, idx]
        print(f"  {name:30s}  {vals.mean():8.3f}  {vals.std():8.3f}  "
              f"{vals.min():8.3f}  {vals.max():8.3f}")

    # ===== Diag 2 : RMS delta input → output → vs target =====
    print("\n=== Diag 2 : RMS delta sur 10 chunks ===")
    print(f"  {'idx':>4s}  {'raw_rms':>8s}  {'tgt_rms':>8s}  {'sur_rms':>8s}  "
          f"{'Δtgt-raw':>10s}  {'Δtgt-sur':>10s}  {'Δsur-raw':>10s}")
    raw_deltas = []
    pred_deltas = []
    for idx in sample_idx:
        p = pairs[idx]
        feats = torch.from_numpy(p['features']).to(DEVICE)
        feats = feats.permute(1, 0).unsqueeze(0)
        raw = torch.from_numpy(p['raw']).to(DEVICE)
        tgt_np = p['target']
        with torch.no_grad():
            params_norm = model(feats)
            params = denormalize_params(params_norm[0])
            sur = chain(raw, params=params).cpu().numpy()
        r_raw = rms_db(p['raw'])
        r_tgt = rms_db(tgt_np)
        r_sur = rms_db(sur)
        dt = r_tgt - r_raw
        ds = r_tgt - r_sur
        da = r_sur - r_raw
        raw_deltas.append(dt)
        pred_deltas.append(da)
        print(f"  {idx:>4d}  {r_raw:+8.1f}  {r_tgt:+8.1f}  {r_sur:+8.1f}  "
              f"{dt:+10.2f}  {ds:+10.2f}  {da:+10.2f}")
    print()
    print(f"  Mean delta target - raw       : {np.mean(raw_deltas):+.2f} dB  (ce que master fait)")
    print(f"  Mean delta surrogate - raw    : {np.mean(pred_deltas):+.2f} dB  (ce que notre modèle fait)")
    print(f"  → modèle utilise seulement {abs(np.mean(pred_deltas)/np.mean(raw_deltas))*100:.0f}% de l'amplitude requise" if np.mean(raw_deltas) != 0 else "")

    # ===== Diag 3 : check params spécifiques limiter (input_db / output_db) =====
    print("\n=== Diag 3 : params limiter (responsables loudness) ===")
    li_input  = all_norm[:, PARAM_LAYOUT['limiter.input_db']]
    li_output = all_norm[:, PARAM_LAYOUT['limiter.output_db']]
    li_thresh = all_norm[:, PARAM_LAYOUT['limiter.threshold_db']]
    print(f"  limiter.input_db   normalized : mean={li_input.mean():.3f} std={li_input.std():.4f}")
    print(f"  limiter.output_db  normalized : mean={li_output.mean():.3f} std={li_output.std():.4f}")
    print(f"  limiter.threshold  normalized : mean={li_thresh.mean():.3f} std={li_thresh.std():.4f}")
    print()
    print("  Range réel limiter.input_db : 0..12 dB → 0.5 = +6 dB")
    print(f"  → modèle prédit en moyenne +{li_input.mean() * 12:.1f} dB input gain")
    print(f"     Target attend ~+{np.mean(raw_deltas):.1f} dB de gain net → mismatch")


if __name__ == '__main__':
    main()
