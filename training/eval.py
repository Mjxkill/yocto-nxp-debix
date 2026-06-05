#!/usr/bin/env python3
"""
V9.5.3 phase 7 — Évaluation modèle entraîné.

Pour chaque paire hold-out :
    1. Compute features
    2. MLP predict params
    3. Apply params à :
        a. Surrogate PyTorch (référence training)
        b. Vraie chaîne LV2 PC (référence production)
    4. Compare outputs entre eux + vs target masterisé

Métriques :
    - RMS dB delta (output vs target)
    - Crest factor delta
    - Spectral magnitude MSE
    - Inter-system delta (surrogate vs LV2)

Usage :
    python3 eval.py --ckpt v1_epoch049 [--n_pairs 5]
"""

import argparse
import os
from pathlib import Path

import numpy as np
import torch
import soundfile as sf

from dataset_loader import iter_pairs, load_audio, audio_stats
from features import compute_features
from model import MasteringMLP, denormalize_params
from surrogate_chain import MasteringChainSurrogate
from lv2_chain import Chain as RealLV2Chain


WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT_DIR  = WORKSPACE / 'checkpoints'
OUT_DIR   = WORKSPACE / 'outputs'

SR        = 48000
CHUNK_SEC = 5.0
CHUNK_N   = int(CHUNK_SEC * SR)
DEVICE    = torch.device('cuda' if torch.cuda.is_available() else 'cpu')


REAL_LV2_URIS = [
    'http://lsp-plug.in/plugins/lv2/para_equalizer_x16_stereo',
    'http://calf.sourceforge.net/plugins/Exciter',
    'http://calf.sourceforge.net/plugins/StereoTools',
    'http://lsp-plug.in/plugins/lv2/limiter_stereo',
]


def apply_params_to_real_lv2(real_chain: RealLV2Chain, params: dict) -> None:
    """Set params on the real LV2 chain via chain.set_param.

    Note : mapping noms surrogate → noms réels plugin. Le surrogate utilise
    nos propres noms ('eq.freq', 'eq.gain_db', ...), les plugins LV2 ont
    des conventions par bande (LSP : f_0, g_0, q_0 ... f_15, g_15, q_15).
    """
    eq = params['eq']
    for b in range(16):
        # LSP Para EQ x16 noms : f_<b>, g_<b>, q_<b>, ft_<b>=2 (peaking)
        try:
            real_chain.set_param(0, f'f_{b}', float(eq['freq'][b].item()))
        except Exception:
            pass
        try:
            # LSP gain est linéaire (1.0 = +0 dB) — dénorm en dB → 10^(dB/20)
            g_lin = float(10.0 ** (eq['gain_db'][b].item() / 20.0))
            real_chain.set_param(0, f'g_{b}', g_lin)
        except Exception:
            pass
        try:
            real_chain.set_param(0, f'q_{b}', float(eq['q'][b].item()))
        except Exception:
            pass
    # Exciter (slot 1) — Calf Exciter noms : amount, drive, freq, ceil
    ex = params['exciter']
    name_map_ex = {
        'amount':  'amount',
        'drive':   'drive',
        'freq_hz': 'freq',
        'ceiling': 'ceil',
    }
    for our, real in name_map_ex.items():
        try:
            real_chain.set_param(1, real, float(ex[our].item()))
        except Exception:
            pass
    # StereoTools (slot 2)
    st = params['stereo']
    name_map_st = {
        'balance':   'balance',
        'mid_gain':  'mlevel',
        'side_gain': 'slevel',
        'sm_swap':   'softclip',   # approximatif (Calf n'a pas swap pur)
    }
    for our, real in name_map_st.items():
        try:
            real_chain.set_param(2, real, float(st[our].item()))
        except Exception:
            pass
    # LSP Limiter (slot 3) — noms : th (threshold), boost, alr, alr_attack...
    lm = params['limiter']
    try:
        # threshold en linéaire pour LSP
        th_lin = float(10.0 ** (lm['threshold_db'].item() / 20.0))
        real_chain.set_param(3, 'th', th_lin)
    except Exception:
        pass


def evaluate(ckpt_tag: str, n_pairs: int = 5, save_wavs: bool = True):
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Loading checkpoint {ckpt_tag}...")
    ckpt_path = CKPT_DIR / f"mlp_{ckpt_tag}.pt"
    if not ckpt_path.exists():
        # Try with full name
        candidates = list(CKPT_DIR.glob(f"mlp_*{ckpt_tag}*.pt"))
        if not candidates:
            print(f"  no ckpt matching '{ckpt_tag}' found in {CKPT_DIR}")
            return
        ckpt_path = candidates[0]
        print(f"  using {ckpt_path.name}")

    ckpt = torch.load(str(ckpt_path), map_location=DEVICE, weights_only=True)
    model = MasteringMLP().to(DEVICE)
    model.load_state_dict(ckpt['model'])
    model.eval()
    print(f"  loaded epoch {ckpt.get('epoch')}, loss {ckpt.get('loss'):.5f}")

    surrog = MasteringChainSurrogate(sr=SR).to(DEVICE)
    surrog.eval()

    print("Loading real LV2 chain (PC)...")
    real_chain = RealLV2Chain(sr=SR, block=96)
    for uri in REAL_LV2_URIS:
        real_chain.add(uri)

    print()
    print(f"{'slug':40s} {'rms_tgt':>8s} {'rms_sur':>8s} {'rms_lv2':>8s} "
          f"{'Δsur':>6s} {'Δlv2':>6s} {'cr_tgt':>6s} {'cr_sur':>6s}")
    print("-" * 110)

    deltas_surrog = []
    deltas_lv2 = []
    inter_sys = []
    for i, pair in enumerate(iter_pairs()):
        if i >= n_pairs:
            break
        try:
            raw, _ = load_audio(pair.raw_path)
            tgt, _ = load_audio(pair.master_path)
        except Exception as e:
            print(f"  skip {pair.slug}: {e}")
            continue
        n = min(len(raw), len(tgt), CHUNK_N)
        raw = raw[:n].astype(np.float32)
        tgt = tgt[:n].astype(np.float32)

        feats = compute_features(raw, sr=SR).mean(axis=0)
        feats_t = torch.from_numpy(feats).unsqueeze(0).to(DEVICE)

        with torch.no_grad():
            params_norm = model(feats_t)
            params = denormalize_params(params_norm[0])

        # Surrogate output
        raw_t = torch.from_numpy(raw.T).to(DEVICE)
        with torch.no_grad():
            out_sur = surrog(raw_t, params=params).cpu().numpy().T

        # Real LV2 output
        apply_params_to_real_lv2(real_chain, params)
        out_lv2_l, out_lv2_r = real_chain.process_wav(raw[:, 0], raw[:, 1])
        out_lv2 = np.stack([out_lv2_l, out_lv2_r], axis=1)

        # Stats
        s_tgt = audio_stats(tgt)
        s_sur = audio_stats(out_sur)
        s_lv2 = audio_stats(out_lv2)
        d_sur = s_sur['rms_db'] - s_tgt['rms_db']
        d_lv2 = s_lv2['rms_db'] - s_tgt['rms_db']
        inter = s_sur['rms_db'] - s_lv2['rms_db']

        print(f"{pair.slug[:40]:40s} "
              f"{s_tgt['rms_db']:+8.1f} {s_sur['rms_db']:+8.1f} {s_lv2['rms_db']:+8.1f} "
              f"{d_sur:+6.2f} {d_lv2:+6.2f} {s_tgt['crest']:6.2f} {s_sur['crest']:6.2f}")

        deltas_surrog.append(d_sur)
        deltas_lv2.append(d_lv2)
        inter_sys.append(inter)

        if save_wavs and i < 3:
            sf.write(str(OUT_DIR / f"{pair.slug}_surrog.wav"),  out_sur, SR)
            sf.write(str(OUT_DIR / f"{pair.slug}_lv2.wav"),     out_lv2, SR)

    print()
    print("=== summary ===")
    print(f"  mean |Δsurrog vs target| : {np.mean(np.abs(deltas_surrog)):.2f} dB")
    print(f"  mean |Δlv2    vs target| : {np.mean(np.abs(deltas_lv2)):.2f} dB")
    print(f"  mean |inter-system delta| (surrog vs lv2) : {np.mean(np.abs(inter_sys)):.2f} dB")
    print(f"  outputs WAV : {OUT_DIR}")


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--ckpt', type=str, required=True,
                    help="tag du checkpoint, ex 'v1_epoch049'")
    ap.add_argument('--n_pairs', type=int, default=5)
    ap.add_argument('--no-wavs', action='store_true')
    args = ap.parse_args()
    evaluate(args.ckpt, n_pairs=args.n_pairs, save_wavs=not args.no_wavs)
