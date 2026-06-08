#!/usr/bin/env python3
"""
V9.5.3-v5 — Training avec loss B (MSE par bande) + corr (Pearson).

Différences vs v4 :
  1. Loss spectral envelope : MSE_dB par 5 bandes (mid + side séparé)
  2. Loss corrélation Pearson : force la forme spectrale à matcher
     (indépendante du niveau absolu)
  3. PARAM_RANGES.stereo centrées (cf model.py v5)

Loss totale :
   1.0 * MSE_spectrale_global    (existant)
 + 0.5 * ΔRMS_dB²                (loudness absolu)
 + 0.2 * Δcrest²                 (dynamique)
 + 3.0 * mean(corr_mid_loss)     ← shape matching mid
 + 1.5 * mean(corr_side_loss)    ← shape matching side
 + 2.0 * MSE_band_db_mid         ← niveau par bande mid
 + 1.0 * MSE_band_db_side        ← niveau par bande side
"""

import os
import sys
import time
import json
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
from torch.optim.lr_scheduler import ReduceLROnPlateau

from dataset_loader import iter_pairs, load_audio
from features import compute_features_mid_only, N_FEATURES_MID_ONLY
from model import (
    MasteringConv1D, denormalize_params_batched,
    N_FEATURES_IN_V2, N_PARAMS_OUT,
)
from surrogate_chain import MasteringChainSurrogate


WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT_DIR  = WORKSPACE / 'checkpoints'
LOG_DIR   = WORKSPACE / 'logs'
CACHE_DIR = WORKSPACE / 'cache'

CHUNK_SEC = 1.0
SR        = 48000
CHUNK_N   = int(CHUNK_SEC * SR)
DEVICE    = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

EPOCHS_DEFAULT = 30
LR_DEFAULT     = 1e-4
WD_DEFAULT     = 1e-3
BATCH_DEFAULT  = 8

# 5 bandes pour le spectral loss (mêmes que features.py)
BANDS_HZ = [(20, 100), (100, 500), (500, 2000), (2000, 8000), (8000, 20000)]


def stft_mag(x, n_fft=2048):
    win = torch.hann_window(n_fft, device=x.device)
    spec = torch.stft(x.reshape(-1, x.shape[-1]), n_fft=n_fft,
                      hop_length=n_fft // 4, win_length=n_fft,
                      window=win, return_complex=True)
    return spec.abs()


def rms_db(x: torch.Tensor) -> torch.Tensor:
    """(..., N) → scalar."""
    return 20.0 * torch.log10(torch.sqrt(torch.mean(x**2) + 1e-12) + 1e-12)


def rms_db_batched(x):
    B = x.shape[0]
    return 20.0 * torch.log10(
        torch.sqrt(torch.mean(x.reshape(B, -1)**2, dim=-1) + 1e-12) + 1e-12)


def crest_batched(x):
    B = x.shape[0]
    xf = x.reshape(B, -1)
    peak = xf.abs().max(dim=-1).values + 1e-12
    rms  = torch.sqrt(torch.mean(xf**2, dim=-1) + 1e-12)
    return peak / rms


def rms_db_per_band(x: torch.Tensor, sr: float, bands_hz=BANDS_HZ,
                    n_fft: int = 2048) -> torch.Tensor:
    """V9.5.3-v5 — RMS dB par bande de fréquence.

    x : (B, N) signal mono (mid ou side, pas stereo)
    Returns : (B, len(bands)) RMS dB par bande
    """
    win = torch.hann_window(n_fft, device=x.device)
    spec = torch.stft(x, n_fft=n_fft, hop_length=n_fft // 4,
                      win_length=n_fft, window=win, return_complex=True)
    pwr = spec.abs() ** 2                            # (B, n_freq, n_frames)
    freqs = torch.linspace(0, sr / 2, pwr.shape[-2], device=x.device)
    out_list = []
    for lo, hi in bands_hz:
        mask = (freqs >= lo) & (freqs < hi)
        e = pwr[:, mask, :].sum(dim=(-2, -1))         # énergie totale par bande
        out_list.append(10.0 * torch.log10(e + 1e-12))
    return torch.stack(out_list, dim=-1)              # (B, n_bands)


def pearson_corr_loss(out_b: torch.Tensor, tgt_b: torch.Tensor) -> torch.Tensor:
    """Pearson correlation 1 - r. out_b, tgt_b : (B, n_bands)."""
    out_c = out_b - out_b.mean(dim=-1, keepdim=True)
    tgt_c = tgt_b - tgt_b.mean(dim=-1, keepdim=True)
    cov   = (out_c * tgt_c).sum(dim=-1)
    s_out = torch.sqrt((out_c ** 2).sum(dim=-1) + 1e-12)
    s_tgt = torch.sqrt((tgt_c ** 2).sum(dim=-1) + 1e-12)
    corr  = cov / (s_out * s_tgt)                     # (B,)
    return (1.0 - corr).mean()                         # 0 = parfait


def loss_v5(output: torch.Tensor, target: torch.Tensor) -> dict:
    """V9.5.3-v5 : loss combinée B + corr Pearson sur mid + side séparés."""
    # Decompose stereo en mid + side
    out_mid  = 0.5 * (output[:, 0, :] + output[:, 1, :])
    out_side = 0.5 * (output[:, 0, :] - output[:, 1, :])
    tgt_mid  = 0.5 * (target[:, 0, :] + target[:, 1, :])
    tgt_side = 0.5 * (target[:, 0, :] - target[:, 1, :])

    # Existing v4 losses
    L_spectral_global = nn.functional.mse_loss(stft_mag(output), stft_mag(target))
    L_rms_global      = ((rms_db_batched(output) - rms_db_batched(target))**2).mean()
    L_crest           = ((crest_batched(output) - crest_batched(target))**2).mean()

    # V5 new : per-band RMS dB (mid + side) + corr Pearson (mid + side)
    out_mid_b  = rms_db_per_band(out_mid,  SR)         # (B, 5)
    out_side_b = rms_db_per_band(out_side, SR)
    tgt_mid_b  = rms_db_per_band(tgt_mid,  SR)
    tgt_side_b = rms_db_per_band(tgt_side, SR)

    L_band_mid  = ((out_mid_b  - tgt_mid_b )**2).mean()
    L_band_side = ((out_side_b - tgt_side_b)**2).mean()

    L_corr_mid  = pearson_corr_loss(out_mid_b,  tgt_mid_b )
    L_corr_side = pearson_corr_loss(out_side_b, tgt_side_b)

    total = (1.0 * L_spectral_global
             + 0.5 * L_rms_global
             + 0.2 * L_crest
             + 2.0 * L_band_mid
             + 1.0 * L_band_side
             + 3.0 * L_corr_mid
             + 1.5 * L_corr_side)

    return {
        'total': total,
        'spectral': L_spectral_global.detach(),
        'rms_db_delta': L_rms_global.detach().sqrt(),
        'crest_delta': L_crest.detach().sqrt(),
        'band_mid':  L_band_mid.detach().sqrt(),
        'band_side': L_band_side.detach().sqrt(),
        'corr_mid':  L_corr_mid.detach(),
        'corr_side': L_corr_side.detach(),
    }


def train(n_pairs=30, epochs=EPOCHS_DEFAULT, lr=LR_DEFAULT,
          weight_decay=WD_DEFAULT, batch_size=BATCH_DEFAULT, tag='v5'):
    CKPT_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Device: {DEVICE}")
    print(f"V9.5.3-v5 — B+corr loss (5 bandes mid+side, Pearson)")
    print(f"  chunk_sec={CHUNK_SEC} lr={lr} batch={batch_size} epochs={epochs}")

    cache_path = CACHE_DIR / 'pair_cache_v2_1s.npz'
    if not cache_path.exists():
        print(f"Cache absent, build via train_v2.build_pair_cache_v2 ({n_pairs} paires)...")
        from train_v2 import build_pair_cache_v2
        build_pair_cache_v2(max_pairs=n_pairs)
    print(f"Loading cache {cache_path}...")
    d = np.load(str(cache_path), allow_pickle=True)
    pairs = list(d['pairs'])
    if n_pairs is not None:
        slugs_seen = set()
        filtered = []
        for p in pairs:
            slugs_seen.add(p['slug'])
            if len(slugs_seen) <= n_pairs:
                filtered.append(p)
            else:
                break
        pairs = filtered
    print(f"Total chunks: {len(pairs)}")

    model = MasteringConv1D(n_input=N_FEATURES_IN_V2).to(DEVICE)
    for m in model.modules():
        if isinstance(m, nn.Linear):
            nn.init.zeros_(m.bias)
    chain = MasteringChainSurrogate(sr=SR).to(DEVICE)
    chain.eval()
    for p in chain.parameters():
        p.requires_grad_(False)

    optimizer = optim.Adam(model.parameters(), lr=lr, weight_decay=weight_decay)
    scheduler = ReduceLROnPlateau(optimizer, mode='min', factor=0.5,
                                   patience=3, min_lr=1e-6)
    log_path = LOG_DIR / f"train_{tag}.jsonl"
    log_f = open(log_path, 'a', buffering=1)

    n_params = sum(p.numel() for p in model.parameters())
    print(f"MasteringConv1D : {n_params:,} params, weight_decay={weight_decay}")
    print(f"Logging to {log_path}")

    n_batches = (len(pairs) + batch_size - 1) // batch_size
    for epoch in range(epochs):
        t0 = time.time()
        np.random.shuffle(pairs)
        model.train()
        epoch_loss = 0.0
        epoch_rms  = 0.0
        epoch_corr_mid = 0.0
        epoch_corr_side = 0.0
        for b in range(n_batches):
            batch = pairs[b * batch_size:(b + 1) * batch_size]
            if not batch:
                continue
            raws    = torch.from_numpy(np.stack([p['raw']      for p in batch])).to(DEVICE)
            targets = torch.from_numpy(np.stack([p['target']   for p in batch])).to(DEVICE)
            n_frames_min = min(p['features'].shape[0] for p in batch)
            feats = torch.from_numpy(
                np.stack([p['features'][:n_frames_min] for p in batch])
            ).to(DEVICE)
            feats = feats.permute(0, 2, 1).contiguous()

            optimizer.zero_grad()
            params_norm = model(feats)
            params = denormalize_params_batched(params_norm)
            output = chain(raws, params=params)

            losses = loss_v5(output, targets)
            losses['total'].backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=1.0)
            optimizer.step()
            epoch_loss += losses['total'].item()
            epoch_rms  += losses['rms_db_delta'].item()
            epoch_corr_mid  += losses['corr_mid'].item()
            epoch_corr_side += losses['corr_side'].item()

        dt = time.time() - t0
        mean_loss = epoch_loss / n_batches
        mean_rms  = epoch_rms / n_batches
        mean_corr_mid  = epoch_corr_mid / n_batches
        mean_corr_side = epoch_corr_side / n_batches
        scheduler.step(mean_loss)
        cur_lr = optimizer.param_groups[0]['lr']
        rec = {
            'epoch': epoch,
            'loss': mean_loss,
            'mean_rms_delta_db': mean_rms,
            'mean_corr_mid': mean_corr_mid,
            'mean_corr_side': mean_corr_side,
            'lr': cur_lr,
            'dt_sec': round(dt, 1),
        }
        log_f.write(json.dumps(rec) + '\n')
        log_f.flush()
        print(f"  epoch {epoch:>3d} loss={mean_loss:.5f} "
              f"rms_delta={mean_rms:+.2f}dB "
              f"corr_mid={1-mean_corr_mid:.3f} corr_side={1-mean_corr_side:.3f} "
              f"lr={cur_lr:.1e} t={dt:.0f}s",
              flush=True)

        ckpt = CKPT_DIR / f"conv_{tag}_epoch{epoch:03d}.pt"
        torch.save({'model': model.state_dict(),
                    'epoch': epoch,
                    'loss': mean_loss,
                    'lr': cur_lr}, str(ckpt))
    log_f.close()
    print(f"Done. Final checkpoint: {ckpt}")


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('--n_pairs', type=int, default=30)
    ap.add_argument('--epochs', type=int, default=EPOCHS_DEFAULT)
    ap.add_argument('--lr', type=float, default=LR_DEFAULT)
    ap.add_argument('--wd', type=float, default=WD_DEFAULT)
    ap.add_argument('--batch', type=int, default=BATCH_DEFAULT)
    ap.add_argument('--tag', type=str, default='v5')
    args = ap.parse_args()
    train(n_pairs=args.n_pairs, epochs=args.epochs, lr=args.lr,
          weight_decay=args.wd, batch_size=args.batch, tag=args.tag)
