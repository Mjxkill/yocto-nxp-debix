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
    """Set params on real LV2 chain — vraie noms LV2 (vérifiés par lilv).
    Pas de try/except : un crash veut dire mapping faux et doit être fixé.
    """
    # === Slot 0 : LSP Para EQ x16 stereo ===
    # Par défaut ft_X = 0 (OFF). On set ft_X = 2 (bell/peak) pour activer.
    # g_X est en linéaire (0.0158 = -36 dB ... 63 = +36 dB ; def 1.0 = 0 dB)
    # q_X range 0-100 dans LSP (Q standard biquad ~0.3-4 chez nous suffit)
    eq = params['eq']
    for b in range(16):
        real_chain.set_param(0, f'ft_{b}', 1.0)             # 1 = Bell (= peak)
        real_chain.set_param(0, f'f_{b}', float(eq['freq'][b].item()))
        g_lin = float(10.0 ** (eq['gain_db'][b].item() / 20.0))
        real_chain.set_param(0, f'g_{b}', g_lin)
        real_chain.set_param(0, f'q_{b}', float(eq['q'][b].item()))

    # === Slot 1 : Calf Exciter ===
    ex = params['exciter']
    real_chain.set_param(1, 'amount', float(ex['amount'].item()))
    real_chain.set_param(1, 'drive',  float(ex['drive'].item()))
    real_chain.set_param(1, 'freq',   float(ex['freq_hz'].item()))
    real_chain.set_param(1, 'ceil',   float(ex['ceiling'].item()))

    # === Slot 2 : Calf StereoTools ===
    st = params['stereo']
    real_chain.set_param(2, 'balance_in', float(st['balance'].item()))
    real_chain.set_param(2, 'mlev',       float(st['mid_gain'].item()))
    real_chain.set_param(2, 'slev',       float(st['side_gain'].item()))

    # === Slot 3 : LSP Limiter Stereo ===
    lm = params['limiter']
    real_chain.set_param(3, 'th',    float(10.0 ** (lm['threshold_db'].item() / 20.0)))
    real_chain.set_param(3, 'g_in',  float(10.0 ** (lm['input_db'].item()    / 20.0)))
    real_chain.set_param(3, 'g_out', float(10.0 ** (lm['output_db'].item()   / 20.0)))
    real_chain.set_param(3, 'at',    float(lm['attack_ms'].item()))
    real_chain.set_param(3, 'rt',    float(lm['release_ms'].item()))


def cap_params(params, cap_eq_db=3.0, cap_limiter_in_db=3.0,
               force_drive=None, force_balance=None):
    """V9.5.3-v4 — Post-process cap pour réduire le loudness war / boost
    spectral excessif appris par le modèle. Applied à l'inférence (pas au
    training).

    force_drive   : override exciter.drive (range LV2 1..6).
    force_balance : override stereo.balance (range -1..+1, 0 = centre).
    """
    params['eq']['gain_db'] = torch.clamp(params['eq']['gain_db'],
                                          -cap_eq_db, +cap_eq_db)
    params['limiter']['input_db'] = torch.clamp(params['limiter']['input_db'],
                                                 0.0, cap_limiter_in_db)
    if force_drive is not None:
        params['exciter']['drive'] = torch.full_like(
            params['exciter']['drive'], float(force_drive))
    if force_balance is not None:
        params['stereo']['balance'] = torch.full_like(
            params['stereo']['balance'], float(force_balance))
    return params


def evaluate(ckpt_tag, n_pairs=3, max_seconds=20,
             cap_eq_db=3.0, cap_limiter_in_db=3.0,
             force_drive=None, force_balance=None):
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

        # Limit to max_seconds (negative = full track)
        if max_seconds > 0:
            n_full = min(len(raw), len(tgt), max_seconds * SR)
        else:
            n_full = min(len(raw), len(tgt))
        raw = raw[:n_full].astype(np.float32)
        tgt = tgt[:n_full].astype(np.float32)

        # Compute features per 1s chunk, predict params, apply to chain
        # Pour eval, on prédit params globaux moyennés (proxy de sliding window)
        feats = compute_features_mid_only(raw, sr=SR)        # (N_frames, 11)
        feats_t = torch.from_numpy(feats).permute(1, 0).unsqueeze(0).to(DEVICE)
        with torch.no_grad():
            params_norm = model(feats_t)
            params = denormalize_params(params_norm[0])
            params = cap_params(params, cap_eq_db, cap_limiter_in_db,
                                force_drive, force_balance)

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
    ap.add_argument('--seconds', type=int, default=20,
                    help='Limit each track to N seconds (-1 = full track)')
    ap.add_argument('--cap_eq_db', type=float, default=3.0,
                    help='Clamp EQ gain_db to ±X dB (post-process cap)')
    ap.add_argument('--force_drive', type=float, default=None,
                    help='Override exciter drive [1..6]. None = utilise param prédit')
    ap.add_argument('--force_balance', type=float, default=None,
                    help='Override stereo balance [-1..+1]. 0 = centre.')
    ap.add_argument('--cap_limiter_in_db', type=float, default=3.0,
                    help='Clamp limiter input_db to [0, X] dB (post-process cap)')
    args = ap.parse_args()
    evaluate(args.ckpt, n_pairs=args.n_pairs, max_seconds=args.seconds,
             cap_eq_db=args.cap_eq_db, cap_limiter_in_db=args.cap_limiter_in_db,
             force_drive=args.force_drive, force_balance=args.force_balance)
