#!/usr/bin/env python3
"""Diag EQ bands : montre la distribution des 16 bandes de l'EQ par fréquence
et gain prédites par le modèle entraîné."""

import numpy as np
import torch
from pathlib import Path

from features import compute_features_mid_only
from model import MasteringConv1D, denormalize_params, N_FEATURES_IN_V2

CKPT_PATH = Path('/home/michael/data/mastering_workspace/checkpoints/conv_v5_8_full_epoch028.pt')
CACHE_PATH = Path('/home/michael/data/mastering_workspace/cache/pair_cache_v5_5_200ms.npz')

DEVICE = torch.device('cuda' if torch.cuda.is_available() else 'cpu')


def main():
    ckpt = torch.load(str(CKPT_PATH), map_location=DEVICE, weights_only=True)
    model = MasteringConv1D(n_input=N_FEATURES_IN_V2).to(DEVICE)
    model.load_state_dict(ckpt['model'])
    model.eval()

    d = np.load(str(CACHE_PATH), allow_pickle=True)
    pairs = list(d['pairs'])

    # 50 chunks aléatoires
    np.random.seed(42)
    idx = np.random.choice(len(pairs), size=50, replace=False)

    all_freqs = []
    all_gains_db = []
    all_qs = []
    for i in idx:
        p = pairs[i]
        feats = torch.from_numpy(p['features']).to(DEVICE)
        feats = feats.permute(1, 0).unsqueeze(0)
        with torch.no_grad():
            params_norm = model(feats)
            params = denormalize_params(params_norm[0])
        all_freqs.append(params['eq']['freq'].cpu().numpy())
        all_gains_db.append(params['eq']['gain_db'].cpu().numpy())
        all_qs.append(params['eq']['q'].cpu().numpy())
    freqs = np.stack(all_freqs)       # (50, 16)
    gains = np.stack(all_gains_db)
    qs    = np.stack(all_qs)

    print("=== EQ bandes — distribution sur 50 chunks ===")
    print(f"{'band':>4s}  {'freq_mean':>10s}  {'freq_std':>9s}  "
          f"{'gain_dB_mean':>13s}  {'gain_dB_std':>11s}  "
          f"{'gain_min':>9s}  {'gain_max':>9s}  {'Q_mean':>7s}")
    print("-" * 90)
    for b in range(16):
        f_mean = freqs[:, b].mean()
        f_std  = freqs[:, b].std()
        g_mean = gains[:, b].mean()
        g_std  = gains[:, b].std()
        g_min  = gains[:, b].min()
        g_max  = gains[:, b].max()
        q_mean = qs[:, b].mean()
        print(f"{b:>4d}  {f_mean:>9.1f}Hz {f_std:>9.1f}  "
              f"{g_mean:>+12.2f}dB {g_std:>10.2f}  "
              f"{g_min:>+8.2f}  {g_max:>+8.2f}  {q_mean:>7.2f}")

    print()
    print("=== Gain moyen total par décade ===")
    decades = [(20, 100), (100, 500), (500, 2000), (2000, 8000), (8000, 20000)]
    decade_names = ['sub-bass', 'low-mid', 'mid', 'high-mid', 'air']
    for (lo, hi), nm in zip(decades, decade_names):
        # Pour chaque chunk, pondère gains par bande in decade
        total = []
        for c in range(len(idx)):
            mask = (freqs[c] >= lo) & (freqs[c] < hi)
            if mask.any():
                total.append(gains[c][mask].mean())
        if total:
            print(f"  {nm:>10s} ({lo:>5d}-{hi:>5d} Hz) : "
                  f"+{np.mean(total):+.2f} dB moy.")


if __name__ == '__main__':
    main()
