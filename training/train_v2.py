#!/usr/bin/env python3
"""
V9.5.3-v2 — Training loop refactoré.

Changements vs v1 :
    A. LR 1e-4 + ReduceLROnPlateau (au lieu de 1e-3 fixe)
    B. Loss reformulée : drop wav MSE, garde RMS + spectral + crest
    C. Features par frame + modèle Conv1D (au lieu de moyenne 5s + MLP)
    D. Chunks 1s (au lieu de 5s) → ~5× plus rapide par epoch
    E. Mid-only features (drop side, artifact dataset)

Pipeline :
    1. iter_pairs() du dataset utilisateur
    2. Chunks de 1s (48000 samples), raw + target
    3. Features par frame : (N_frames, 11) — chunk 1s → ~94 frames
    4. MasteringConv1D forward → 62 params normalisés (global / chunk)
    5. denormalize → dict pour MasteringChainSurrogate
    6. chain(raw_chunk, params) → output
    7. Loss : 1·MSE_spec + 0.5·ΔRMS² + 0.5·Δcrest²
    8. Backward + Adam + ReduceLROnPlateau

Checkpoints + logs : /home/michael/data/mastering_workspace/
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
from features import (
    compute_features_mid_only, N_FEATURES_MID_ONLY,
)
from model import (
    MasteringConv1D, denormalize_params,
    N_FEATURES_IN_V2, N_PARAMS_OUT,
)
from surrogate_chain import MasteringChainSurrogate


WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT_DIR  = WORKSPACE / 'checkpoints'
LOG_DIR   = WORKSPACE / 'logs'
CACHE_DIR = WORKSPACE / 'cache'

CHUNK_SEC = 1.0                    # V9.5.3-v2 D : 1s au lieu de 5s
SR        = 48000
CHUNK_N   = int(CHUNK_SEC * SR)    # 48 000 samples
DEVICE    = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

EPOCHS_DEFAULT = 30
LR_DEFAULT     = 1e-4              # V9.5.3-v2 A
BATCH_DEFAULT  = 8                 # un peu plus large car chunks plus courts


def stft_mag(x: torch.Tensor, n_fft: int = 1024) -> torch.Tensor:
    win = torch.hann_window(n_fft, device=x.device)
    spec = torch.stft(x.reshape(-1, x.shape[-1]), n_fft=n_fft,
                      hop_length=n_fft // 4, win_length=n_fft,
                      window=win, return_complex=True)
    return spec.abs()


def rms_db(x: torch.Tensor) -> torch.Tensor:
    return 20.0 * torch.log10(torch.sqrt(torch.mean(x**2) + 1e-12) + 1e-12)


def crest(x: torch.Tensor) -> torch.Tensor:
    peak = x.abs().max() + 1e-12
    rms  = torch.sqrt(torch.mean(x**2) + 1e-12)
    return peak / rms


def loss_v2(output: torch.Tensor, target: torch.Tensor) -> dict:
    """V9.5.3-v2 B : loss reformulée. Drop wav MSE (qui saturait), garde :
        - spectral MSE
        - RMS dB delta (loudness matching)
        - crest delta (dynamic compression matching)
    Pondération : RMS dominant (l'objectif principal du mastering).
    """
    L_spectral = nn.functional.mse_loss(stft_mag(output), stft_mag(target))
    L_rms      = (rms_db(output) - rms_db(target))**2
    L_crest    = (crest(output) - crest(target))**2
    total = 1.0 * L_spectral + 0.5 * L_rms + 0.5 * L_crest
    return {
        'total': total,
        'spectral': L_spectral.detach(),
        'rms_db_delta': L_rms.detach().sqrt(),
        'crest_delta':  L_crest.detach().sqrt(),
    }


def build_pair_cache_v2(max_pairs: int = None, cache_tag: str = 'v2') -> list:
    """Cache features par frame (mid-only) + audio chunks 1s."""
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    cache_path = CACHE_DIR / f'pair_cache_{cache_tag}_1s.npz'
    if cache_path.exists():
        print(f"Loading cache from {cache_path}...")
        d = np.load(str(cache_path), allow_pickle=True)
        return list(d['pairs'])

    print(f"Building cache (chunks {CHUNK_SEC}s, mid-only features)...")
    pairs = []
    for i, pair in enumerate(iter_pairs()):
        if max_pairs is not None and i >= max_pairs:
            break
        try:
            raw, sr = load_audio(pair.raw_path, target_sr=SR)
            tgt, _  = load_audio(pair.master_path, target_sr=SR)
        except Exception as e:
            print(f"  skip {pair.slug}: {e}")
            continue
        n = min(len(raw), len(tgt))
        n_chunks = n // CHUNK_N
        if n_chunks == 0:
            continue
        for c in range(n_chunks):
            i0 = c * CHUNK_N
            i1 = i0 + CHUNK_N
            r = raw[i0:i1].astype(np.float32)
            t = tgt[i0:i1].astype(np.float32)
            # Features par frame : (N_frames, 11)
            feats = compute_features_mid_only(r, sr=SR)
            if len(feats) == 0:
                continue
            pairs.append({
                'raw': r.T,           # (2, N)
                'target': t.T,
                'features': feats,    # (N_frames, 11)
                'slug': pair.slug,
            })
        if (i + 1) % 10 == 0:
            print(f"  {i+1} pairs done, {len(pairs)} chunks")
    print(f"Saving cache → {cache_path}")
    np.savez(str(cache_path), pairs=np.array(pairs, dtype=object))
    return pairs


def train(n_pairs: int = 30, epochs: int = EPOCHS_DEFAULT,
          lr: float = LR_DEFAULT, batch_size: int = BATCH_DEFAULT,
          tag: str = 'v2'):
    CKPT_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Device: {DEVICE}")
    print(f"V9.5.3-v2 — Conv1D + mid-only features + spec/rms/crest loss")
    print(f"  chunk_sec={CHUNK_SEC} sr={SR} chunk_N={CHUNK_N}")
    print(f"  lr={lr} batch_size={batch_size} epochs={epochs}")
    print(f"Building pair cache ({n_pairs} pairs)...")
    pairs = build_pair_cache_v2(max_pairs=n_pairs)
    print(f"Total chunks: {len(pairs)}")
    if not pairs:
        print("No pairs available, abort.")
        sys.exit(1)

    model = MasteringConv1D(n_input=N_FEATURES_IN_V2).to(DEVICE)
    chain = MasteringChainSurrogate(sr=SR).to(DEVICE)
    chain.eval()
    for p in chain.parameters():
        p.requires_grad_(False)

    optimizer = optim.Adam(model.parameters(), lr=lr)
    scheduler = ReduceLROnPlateau(optimizer, mode='min', factor=0.5,
                                   patience=3, min_lr=1e-6)
    log_path = LOG_DIR / f"train_{tag}.jsonl"
    log_f = open(log_path, 'a', buffering=1)   # line-buffered

    n_params = sum(p.numel() for p in model.parameters())
    print(f"MasteringConv1D : {n_params:,} params trainable, chain fixed.")
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
            # Features : list of (N_frames, 11) — pad/truncate à fixed length pour batch
            n_frames_min = min(p['features'].shape[0] for p in batch)
            feats = torch.from_numpy(
                np.stack([p['features'][:n_frames_min] for p in batch])
            ).to(DEVICE)
            # (B, N_frames, 11) → (B, 11, N_frames) pour Conv1D
            feats = feats.permute(0, 2, 1).contiguous()

            optimizer.zero_grad()
            params_norm = model(feats)              # (B, 62)
            outs = []
            for i in range(raws.shape[0]):
                params = denormalize_params(params_norm[i])
                out = chain(raws[i], params=params)
                outs.append(out)
            output = torch.stack(outs)

            losses = loss_v2(output, targets)
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
    ap.add_argument('--batch', type=int, default=BATCH_DEFAULT)
    ap.add_argument('--tag', type=str, default='v2')
    args = ap.parse_args()
    train(n_pairs=args.n_pairs, epochs=args.epochs, lr=args.lr,
          batch_size=args.batch, tag=args.tag)
