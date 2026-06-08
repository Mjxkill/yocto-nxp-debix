#!/usr/bin/env python3
"""
V9.5.3-v5.4 — Losses orthogonales NORMALISÉES (contribution égale).

v5.3 avait des magnitudes très différentes (L_crest dominait, L_corr négligeable).
v5.4 : chaque loss divisée par sa magnitude typique au début → contribution ~1
chacune → poids 1.0 partout = vrai équilibre.

Loss :
   (ΔRMS_dB² / 25)      ← typique 5dB² = 25 → norm ~1 au début
 + (1 - corr_Pearson)   ← naturellement borné [0,2]
 + (Δcrest² / 100)      ← typique crest_delta² = 100 → norm ~1

total = L_rms_norm + L_corr + L_crest_norm   (poids 1 partout)
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

BANDS_HZ = [(20, 100), (100, 500), (500, 2000), (2000, 8000), (8000, 20000)]


def stft_mag(x, n_fft=2048):
    win = torch.hann_window(n_fft, device=x.device)
    spec = torch.stft(x.reshape(-1, x.shape[-1]), n_fft=n_fft,
                      hop_length=n_fft // 4, win_length=n_fft,
                      window=win, return_complex=True)
    return spec.abs()


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


def rms_db_per_band_stereo(x: torch.Tensor, sr: float,
                            bands_hz=BANDS_HZ,
                            n_fft: int = 2048) -> torch.Tensor:
    """V5.1 — RMS dB par bande sur signal stéréo (combine L+R).
    x : (B, 2, N). Returns (B, n_bands) en dB.
    """
    B, C, N = x.shape
    x_flat = x.reshape(B * C, N)
    win = torch.hann_window(n_fft, device=x.device)
    spec = torch.stft(x_flat, n_fft=n_fft, hop_length=n_fft // 4,
                      win_length=n_fft, window=win, return_complex=True)
    pwr = spec.abs() ** 2          # (B*C, n_freq, n_frames)
    pwr = pwr.reshape(B, C, pwr.shape[1], pwr.shape[2]).sum(dim=1)  # combine L+R
    freqs = torch.linspace(0, sr / 2, pwr.shape[-2], device=x.device)
    out_list = []
    for lo, hi in bands_hz:
        mask = (freqs >= lo) & (freqs < hi)
        e = pwr[:, mask, :].sum(dim=(-2, -1))
        out_list.append(10.0 * torch.log10(e + 1e-12))
    return torch.stack(out_list, dim=-1)


def pearson_corr_loss(out_b: torch.Tensor, tgt_b: torch.Tensor) -> torch.Tensor:
    """Pearson 1 - r. out_b, tgt_b : (B, n_bands)."""
    out_c = out_b - out_b.mean(dim=-1, keepdim=True)
    tgt_c = tgt_b - tgt_b.mean(dim=-1, keepdim=True)
    cov   = (out_c * tgt_c).sum(dim=-1)
    s_out = torch.sqrt((out_c ** 2).sum(dim=-1) + 1e-12)
    s_tgt = torch.sqrt((tgt_c ** 2).sum(dim=-1) + 1e-12)
    corr  = cov / (s_out * s_tgt)
    return (1.0 - corr).mean()


SCALE_RMS   = 25.0   # ΔRMS² typique au début (≈ 5dB²)
SCALE_CORR  = 1.0    # 1-r naturellement borné [0, 2]
SCALE_CREST = 100.0  # Δcrest² typique


def loss_v5_4(output, target):
    """V5.4 — 3 losses orthogonales NORMALISÉES, contribution équilibrée."""
    L_rms   = ((rms_db_batched(output) - rms_db_batched(target))**2).mean()
    L_crest = ((crest_batched(output) - crest_batched(target))**2).mean()

    out_b = rms_db_per_band_stereo(output, SR)
    tgt_b = rms_db_per_band_stereo(target, SR)
    L_corr = pearson_corr_loss(out_b, tgt_b)

    # Normaliser pour contribution équilibrée
    L_rms_n   = L_rms   / SCALE_RMS
    L_corr_n  = L_corr  / SCALE_CORR
    L_crest_n = L_crest / SCALE_CREST

    total = L_rms_n + L_corr_n + L_crest_n   # poids 1 chacun

    return {
        'total': total,
        'rms_db_delta': L_rms.detach().sqrt(),
        'crest_delta':  L_crest.detach().sqrt(),
        'corr_one_minus': L_corr.detach(),
        # contribution réelle de chaque loss pour debug
        'contrib_rms':   L_rms_n.detach(),
        'contrib_corr':  L_corr_n.detach(),
        'contrib_crest': L_crest_n.detach(),
    }


def train(n_pairs=30, epochs=EPOCHS_DEFAULT, lr=LR_DEFAULT,
          weight_decay=WD_DEFAULT, batch_size=BATCH_DEFAULT, tag='v5_1'):
    CKPT_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Device: {DEVICE}")
    print(f"V9.5.3-v5.4 — losses orthogonales NORMALISÉES (contribution égale)")
    print(f"  chunk_sec={CHUNK_SEC} lr={lr} batch={batch_size} epochs={epochs}")

    cache_path = CACHE_DIR / 'pair_cache_v2_1s.npz'
    if not cache_path.exists():
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

    print(f"MasteringConv1D : {sum(p.numel() for p in model.parameters()):,} params")
    print(f"Logging to {log_path}")

    n_batches = (len(pairs) + batch_size - 1) // batch_size
    for epoch in range(epochs):
        t0 = time.time()
        np.random.shuffle(pairs)
        model.train()
        ep_loss = 0.0
        ep_rms  = 0.0
        ep_corr = 0.0
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

            losses = loss_v5_4(output, targets)
            losses['total'].backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=1.0)
            optimizer.step()
            ep_loss += losses['total'].item()
            ep_rms  += losses['rms_db_delta'].item()
            ep_corr += losses['corr_one_minus'].item()

        dt = time.time() - t0
        m_loss = ep_loss / n_batches
        m_rms  = ep_rms  / n_batches
        m_corr = ep_corr / n_batches
        scheduler.step(m_loss)
        cur_lr = optimizer.param_groups[0]['lr']
        rec = {
            'epoch': epoch,
            'loss': m_loss,
            'mean_rms_delta_db': m_rms,
            'mean_corr_one_minus': m_corr,
            'lr': cur_lr,
            'dt_sec': round(dt, 1),
        }
        log_f.write(json.dumps(rec) + '\n')
        log_f.flush()
        print(f"  epoch {epoch:>3d} loss={m_loss:.4f} "
              f"rms_delta={m_rms:+.2f}dB "
              f"corr={1-m_corr:+.3f} lr={cur_lr:.1e} t={dt:.0f}s",
              flush=True)

        ckpt = CKPT_DIR / f"conv_{tag}_epoch{epoch:03d}.pt"
        torch.save({'model': model.state_dict(),
                    'epoch': epoch,
                    'loss': m_loss,
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
    ap.add_argument('--tag', type=str, default='v5_4')
    args = ap.parse_args()
    train(n_pairs=args.n_pairs, epochs=args.epochs, lr=args.lr,
          weight_decay=args.wd, batch_size=args.batch, tag=args.tag)
