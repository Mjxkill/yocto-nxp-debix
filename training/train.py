#!/usr/bin/env python3
"""
V9.5.3 phase 6 — Training loop POC.

Pipeline :
    1. iter_pairs() du dataset utilisateur (259 paires)
    2. Chunks de CHUNK_SEC secondes (raw + target)
    3. Features moyennées sur le chunk → 17 floats
    4. MasteringMLP forward → 62 params normalisés
    5. denormalize → dict pour MasteringChainSurrogate
    6. chain(raw_chunk, params) → output
    7. Loss combinée vs target
    8. Backward + optim Adam

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

from dataset_loader import iter_pairs, load_audio
from features import compute_features, N_FEATURES, N_BANDS
from model import MasteringMLP, denormalize_params, N_PARAMS_OUT
from surrogate_chain import MasteringChainSurrogate


WORKSPACE   = Path('/home/michael/data/mastering_workspace')
CKPT_DIR    = WORKSPACE / 'checkpoints'
LOG_DIR     = WORKSPACE / 'logs'
CACHE_DIR   = WORKSPACE / 'cache'

CHUNK_SEC   = 5.0                  # durée de chunk audio par sample training
SR          = 48000
CHUNK_N     = int(CHUNK_SEC * SR)  # 240 000 samples
DEVICE      = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

EPOCHS      = 20
LR          = 1e-3
BATCH_SIZE  = 4                    # 4 chunks à la fois (OOM avec plus)


def stft_mag(x: torch.Tensor, n_fft: int = 1024) -> torch.Tensor:
    """STFT magnitude pour spectral loss."""
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


def loss_combined(output: torch.Tensor, target: torch.Tensor) -> dict:
    """Loss multi-objectifs. output, target shape (B, 2, N)."""
    L_wav      = nn.functional.mse_loss(output, target)
    L_spectral = nn.functional.mse_loss(stft_mag(output), stft_mag(target))
    L_rms      = (rms_db(output) - rms_db(target))**2
    L_crest    = (crest(output) - crest(target))**2
    total = 1.0 * L_wav + 0.5 * L_spectral + 0.05 * L_rms + 0.05 * L_crest
    return {
        'total': total,
        'wav': L_wav.detach(),
        'spectral': L_spectral.detach(),
        'rms_db_delta': L_rms.detach().sqrt(),
        'crest_delta':  L_crest.detach().sqrt(),
    }


def build_pair_cache(max_pairs: int = None) -> list:
    """Pre-load + cache audio + features par paire.

    Returns list of dicts {raw, target, features_mean}.
    Charge en RAM si <= 16 GB, sinon sur disk dans CACHE_DIR.
    """
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    cache_path = CACHE_DIR / 'pair_cache_5s.npz'
    if cache_path.exists():
        print(f"Loading cache from {cache_path}...")
        d = np.load(str(cache_path), allow_pickle=True)
        return list(d['pairs'])
    print(f"Building cache (this may take a few minutes)...")
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
        # Truncate au plus court
        n = min(len(raw), len(tgt))
        n_chunks = n // CHUNK_N
        if n_chunks == 0:
            continue
        for c in range(n_chunks):
            i0 = c * CHUNK_N
            i1 = i0 + CHUNK_N
            r = raw[i0:i1].astype(np.float32)
            t = tgt[i0:i1].astype(np.float32)
            feats = compute_features(r, sr=SR).mean(axis=0)  # (17,)
            pairs.append({
                'raw': r.T,      # (2, N)
                'target': t.T,
                'features': feats,
                'slug': pair.slug,
            })
        if (i + 1) % 10 == 0:
            print(f"  {i+1} pairs done, {len(pairs)} chunks")
    print(f"Saving cache → {cache_path}")
    np.savez(str(cache_path), pairs=np.array(pairs, dtype=object))
    return pairs


def train(n_pairs: int = 30, epochs: int = EPOCHS, lr: float = LR,
          batch_size: int = BATCH_SIZE, tag: str = 'v1'):
    """Entraîne le MLP sur n_pairs paires du dataset.

    Cache pré-calculé dans WORKSPACE/cache/.
    Checkpoint sauvé après chaque epoch dans WORKSPACE/checkpoints/.
    """
    CKPT_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Device: {DEVICE}")
    print(f"Building pair cache ({n_pairs} pairs)...")
    pairs = build_pair_cache(max_pairs=n_pairs)
    print(f"Total chunks: {len(pairs)}")
    if not pairs:
        print("No pairs available, abort.")
        sys.exit(1)

    model = MasteringMLP().to(DEVICE)
    chain = MasteringChainSurrogate(sr=SR).to(DEVICE)
    chain.eval()   # surrogate fixed (NPU prédit ses params)
    for p in chain.parameters():
        p.requires_grad_(False)

    optimizer = optim.Adam(model.parameters(), lr=lr)
    log_path = LOG_DIR / f"train_{tag}.jsonl"
    log_f = open(log_path, 'a')

    print(f"Training MLP {sum(p.numel() for p in model.parameters()):,} params, "
          f"chain fixed (surrogate).")
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
            feats   = torch.from_numpy(np.stack([p['features'] for p in batch])).to(DEVICE)

            optimizer.zero_grad()
            params_norm = model(feats)              # (B, 62)
            # Process batch one by one (chain params not batched in surrogate
            # current impl ; could be batched as upgrade)
            outs = []
            for i in range(raws.shape[0]):
                params = denormalize_params(params_norm[i])
                out = chain(raws[i], params=params)
                outs.append(out)
            output = torch.stack(outs)                # (B, 2, N)

            losses = loss_combined(output, targets)
            losses['total'].backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=1.0)
            optimizer.step()
            epoch_loss += losses['total'].item()
            epoch_rms  += losses['rms_db_delta'].item()

        dt = time.time() - t0
        mean_loss = epoch_loss / n_batches
        mean_rms  = epoch_rms / n_batches
        rec = {
            'epoch': epoch,
            'loss': mean_loss,
            'mean_rms_delta_db': mean_rms,
            'dt_sec': round(dt, 1),
        }
        log_f.write(json.dumps(rec) + '\n')
        log_f.flush()
        print(f"  epoch {epoch:>3d} loss={mean_loss:.5f} "
              f"rms_delta={mean_rms:+.2f}dB time={dt:.1f}s")

        ckpt = CKPT_DIR / f"mlp_{tag}_epoch{epoch:03d}.pt"
        torch.save({'model': model.state_dict(),
                    'epoch': epoch,
                    'loss': mean_loss}, str(ckpt))
    log_f.close()
    print(f"Done. Final checkpoint: {ckpt}")


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('--n_pairs', type=int, default=30)
    ap.add_argument('--epochs', type=int, default=EPOCHS)
    ap.add_argument('--lr', type=float, default=LR)
    ap.add_argument('--batch', type=int, default=BATCH_SIZE)
    ap.add_argument('--tag', type=str, default='v1')
    args = ap.parse_args()
    train(n_pairs=args.n_pairs, epochs=args.epochs, lr=args.lr,
          batch_size=args.batch, tag=args.tag)
