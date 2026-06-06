#!/usr/bin/env python3
"""
V9.5.3-v4 — Eval audio : génère wavs surrogate + LV2 réel pour écoute.

Usage :
    python3 eval_v4.py --ckpt v4_full_epoch029 --n_pairs 3
"""

import argparse
from pathlib import Path

import numpy as np
import torch
import soundfile as sf

from dataset_loader import iter_pairs, load_audio, audio_stats
from features import compute_features_mid_only
from model import MasteringConv1D, denormalize_params, N_FEATURES_IN_V2
from surrogate_chain import MasteringChainSurrogate
from lv2_chain import Chain as RealLV2Chain


WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT_DIR  = WORKSPACE / 'checkpoints'
OUT_DIR   = WORKSPACE / 'outputs'

SR        = 48000
CHUNK_SEC = 1.0
CHUNK_N   = int(CHUNK_SEC * SR)
DEVICE    = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

CHAIN_URIS = [
    'http://lsp-plug.in/plugins/lv2/para_equalizer_x16_stereo',
    'http://calf.sourceforge.net/plugins/Exciter',
    'http://calf.sourceforge.net/plugins/StereoTools',
    'http://lsp-plug.in/plugins/lv2/limiter_stereo',
]


def apply_params_to_real_lv2(real_chain, params):
    """Set params on real LV2 chain (best-effort name mapping)."""
    eq = params['eq']
    for b in range(16):
        try: real_chain.set_param(0, f'f_{b}', float(eq['freq'][b].item()))
        except: pass
        try:
            g_lin = float(10.0 ** (eq['gain_db'][b].item() / 20.0))
            real_chain.set_param(0, f'g_{b}', g_lin)
        except: pass
        try: real_chain.set_param(0, f'q_{b}', float(eq['q'][b].item()))
        except: pass
    ex = params['exciter']
    for our, real in [('amount', 'amount'), ('drive', 'drive'),
                      ('freq_hz', 'freq'), ('ceiling', 'ceil')]:
        try: real_chain.set_param(1, real, float(ex[our].item()))
        except: pass
    st = params['stereo']
    for our, real in [('balance', 'balance'), ('mid_gain', 'mlevel'),
                      ('side_gain', 'slevel')]:
        try: real_chain.set_param(2, real, float(st[our].item()))
        except: pass
    lm = params['limiter']
    try:
        th_lin = float(10.0 ** (lm['threshold_db'].item() / 20.0))
        real_chain.set_param(3, 'th', th_lin)
    except: pass


def evaluate(ckpt_tag, n_pairs=3):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    ckpt_path = CKPT_DIR / f"conv_{ckpt_tag}.pt"
    print(f"Loading {ckpt_path.name}...")
    ckpt = torch.load(str(ckpt_path), map_location=DEVICE, weights_only=True)
    model = MasteringConv1D(n_input=N_FEATURES_IN_V2).to(DEVICE)
    model.load_state_dict(ckpt['model'])
    model.eval()
    print(f"  epoch {ckpt['epoch']}, loss {ckpt['loss']:.4f}")

    surrog = MasteringChainSurrogate(sr=SR).to(DEVICE)
    surrog.eval()

    print("Loading real LV2 chain (PC)...")
    real_chain = RealLV2Chain(sr=SR, block=96)
    for uri in CHAIN_URIS:
        real_chain.add(uri)

    print()
    print(f"{'slug':50s} {'tgt_rms':>8s} {'sur_rms':>8s} {'lv2_rms':>8s} "
          f"{'Δsur':>6s} {'Δlv2':>6s}")
    print("-" * 105)

    for i, pair in enumerate(iter_pairs()):
        if i >= n_pairs:
            break
        raw, _ = load_audio(pair.raw_path)
        tgt, _ = load_audio(pair.master_path)

        # Process first 20s for listening test
        n_full = min(len(raw), len(tgt), 20 * SR)
        raw = raw[:n_full].astype(np.float32)
        tgt = tgt[:n_full].astype(np.float32)

        # Compute features per 1s chunk, predict params, apply to chain
        # Pour eval, on prédit params globaux moyennés (proxy de sliding window)
        feats = compute_features_mid_only(raw, sr=SR)        # (N_frames, 11)
        feats_t = torch.from_numpy(feats).permute(1, 0).unsqueeze(0).to(DEVICE)
        with torch.no_grad():
            params_norm = model(feats_t)
            params = denormalize_params(params_norm[0])

        # Surrogate
        raw_t = torch.from_numpy(raw.T).to(DEVICE)
        with torch.no_grad():
            out_sur = surrog(raw_t, params=params).cpu().numpy().T

        # Real LV2
        apply_params_to_real_lv2(real_chain, params)
        ol, or_ = real_chain.process_wav(raw[:, 0], raw[:, 1])
        out_lv2 = np.stack([ol, or_], axis=1)

        s_tgt = audio_stats(tgt)
        s_sur = audio_stats(out_sur)
        s_lv2 = audio_stats(out_lv2)
        d_sur = s_sur['rms_db'] - s_tgt['rms_db']
        d_lv2 = s_lv2['rms_db'] - s_tgt['rms_db']
        print(f"{pair.slug[:50]:50s} "
              f"{s_tgt['rms_db']:+8.1f} {s_sur['rms_db']:+8.1f} {s_lv2['rms_db']:+8.1f} "
              f"{d_sur:+6.2f} {d_lv2:+6.2f}")

        sf.write(str(OUT_DIR / f"{pair.slug}_raw.wav"),    raw,     SR)
        sf.write(str(OUT_DIR / f"{pair.slug}_target.wav"), tgt,     SR)
        sf.write(str(OUT_DIR / f"{pair.slug}_surrog.wav"), out_sur, SR)
        sf.write(str(OUT_DIR / f"{pair.slug}_lv2.wav"),    out_lv2, SR)
    print()
    print(f"Wavs écrits dans {OUT_DIR}")


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--ckpt', type=str, required=True)
    ap.add_argument('--n_pairs', type=int, default=3)
    args = ap.parse_args()
    evaluate(args.ckpt, n_pairs=args.n_pairs)
