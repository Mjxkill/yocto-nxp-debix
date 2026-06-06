#!/usr/bin/env python3
"""
V9.5.3-v4 — Training avec batched chain forward.

Différence vs v3 : la boucle `for i in range(B): chain(raws[i], params)` est
remplacée par un unique appel `chain(raws, params=denormalize_params_batched(norm))`
où raws est (B, 2, N) et chaque param est (B,) ou (B, 16).

Gain attendu : ×2-3 sur le temps par batch (la GIL Python est minimisée,
lfilter batched amortit l'overhead).
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


def stft_mag(x, n_fft=1024):
    win = torch.hann_window(n_fft, device=x.device)
    spec = torch.stft(x.reshape(-1, x.shape[-1]), n_fft=n_fft,
                      hop_length=n_fft // 4, win_length=n_fft,
                      window=win, return_complex=True)
    return spec.abs()


def rms_db_batched(x):
    """(B, ..., N) → (B,) RMS dB per batch."""
    B = x.shape[0]
    return 20.0 * torch.log10(
        torch.sqrt(torch.mean(x.reshape(B, -1)**2, dim=-1) + 1e-12) + 1e-12)


def crest_batched(x):
    """(B, ..., N) → (B,) crest factor per batch."""
    B = x.shape[0]
    xf = x.reshape(B, -1)
    peak = xf.abs().max(dim=-1).values + 1e-12
    rms  = torch.sqrt(torch.mean(xf**2, dim=-1) + 1e-12)
    return peak / rms


def loss_v4(output, target):
    """V9.5.3-v4 : loss batched. RMS poids 15."""
    L_spectral = nn.functional.mse_loss(stft_mag(output), stft_mag(target))
    L_rms      = ((rms_db_batched(output) - rms_db_batched(target))**2).mean()
    L_crest    = ((crest_batched(output) - crest_batched(target))**2).mean()
    total = 1.0 * L_spectral + 15.0 * L_rms + 0.5 * L_crest
    return {
        'total': total,
        'spectral': L_spectral.detach(),
        'rms_db_delta': L_rms.detach().sqrt(),
        'crest_delta': L_crest.detach().sqrt(),
    }


def train(n_pairs=30, epochs=EPOCHS_DEFAULT, lr=LR_DEFAULT,
          weight_decay=WD_DEFAULT, batch_size=BATCH_DEFAULT, tag='v4'):
    CKPT_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Device: {DEVICE}")
    print(f"V9.5.3-v4 — batched chain forward (×2-3 speedup expected)")
    print(f"  chunk_sec={CHUNK_SEC} lr={lr} batch={batch_size} epochs={epochs}")

    cache_path = CACHE_DIR / 'pair_cache_v2_1s.npz'
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
            params_norm = model(feats)                     # (B, 62)
            # V9.5.3-v4 : un seul forward chain pour tout le batch
            params = denormalize_params_batched(params_norm)
            output = chain(raws, params=params)            # (B, 2, N)

            losses = loss_v4(output, targets)
            losses['total'].backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=1.0)
            optimizer.step()
            epoch_loss += losses['total'].item()
            epoch_rms  += losses['rms_db_delta'].item()

        dt = time.time() - t0
        mean_loss = epoch_loss / n_batches
        mean_rms  = epoch_rms / n_batches
        scheduler.step(mean_loss)
        cur_lr = optimizer.param_groups[0]['lr']
        rec = {
            'epoch': epoch,
            'loss': mean_loss,
            'mean_rms_delta_db': mean_rms,
            'lr': cur_lr,
            'dt_sec': round(dt, 1),
        }
        log_f.write(json.dumps(rec) + '\n')
        log_f.flush()
        print(f"  epoch {epoch:>3d} loss={mean_loss:.5f} "
              f"rms_delta={mean_rms:+.2f}dB lr={cur_lr:.1e} time={dt:.1f}s",
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
    ap.add_argument('--tag', type=str, default='v4')
    args = ap.parse_args()
    train(n_pairs=args.n_pairs, epochs=args.epochs, lr=args.lr,
          weight_decay=args.wd, batch_size=args.batch, tag=args.tag)
