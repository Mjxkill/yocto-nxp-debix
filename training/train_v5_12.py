#!/usr/bin/env python3
"""
V9.5.3-v5.12 — Bandes fines aigus + pondération 500-2K + cohérence temporelle (output-raw vs target-raw).

Base v5.5 (Pearson, 5 bandes) → v5.8 :
- 6 bandes log 20-1000 Hz (détail conservé en bass/medium)
- 14 bandes linéaires 1000-20000 Hz (finesse régulière dans les aigus)
- Loss inchangée (Pearson sur 20 bandes ; pas de MSE pour éviter divergence)

Loss :
   (ΔRMS_dB² / 25)
 + (1 - corr_Pearson_20hybrid)
 + (Δcrest² / 100)
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

CHUNK_SEC = 0.2                    # V5.5 : window 200 ms pour réactivité
SR        = 48000
CHUNK_N   = int(CHUNK_SEC * SR)    # 9 600 samples
DEVICE    = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

EPOCHS_DEFAULT = 30
LR_DEFAULT     = 1e-4
WD_DEFAULT     = 1e-3
BATCH_DEFAULT  = 8

# V5.8 : 20 bandes hybrides
#   6 bandes log    20-1000 Hz  (bass/medium détail)
#   14 bandes linéaires 1000-20000 Hz (aigus fins, finesse > 4 kHz)
# V5.12 : 4 log low + 4 log mid + 12 lin high pour finesse > 4 kHz
_log_low    = np.logspace(np.log10(20.0), np.log10(500.0), 5)
_log_mid    = np.logspace(np.log10(500.0), np.log10(4000.0), 5)
_lin_high   = np.linspace(4000.0, 20000.0, 13)
_edges      = np.concatenate([_log_low, _log_mid[1:], _lin_high[1:]])
BANDS_HZ    = [(float(_edges[i]), float(_edges[i+1])) for i in range(20)]

# V5.12 : pondération bandes (×2 dans 500-2K Hz = "boîte de conserve")
# Pénalise davantage les écarts dans cette zone que le master humain réduit
BAND_WEIGHTS_NP = np.ones(20, dtype=np.float32)
for _i, (_lo, _hi) in enumerate(BANDS_HZ):
    _center = 0.5 * (_lo + _hi)
    if 500.0 <= _center <= 2200.0:
        BAND_WEIGHTS_NP[_i] = 2.0
BAND_WEIGHTS = torch.tensor(BAND_WEIGHTS_NP)


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


SCALE_RMS         = 25.0    # ΔRMS² typique
SCALE_DELTA_BAND  = 100.0   # ΔδdB² par bande typique (delta_out - delta_tgt)²
SCALE_DELTA_CORR  = 1.0     # 1-r borné
SCALE_CREST       = 100.0


SCALE_TEMP_COH = 0.05    # variance params normalisés (~ 0.05 = std raisonnable)


def loss_v5_12(raw, output, target, params_norm=None):
    """V5.12 — Delta-matching + pondération bandes 500-2K + cohérence temporelle.

    params_norm : (B, 62) params normalisés du modèle (optionnel pour temporal coh)
    """
    raw_b = rms_db_per_band_stereo(raw, SR)        # (B, 20)
    out_b = rms_db_per_band_stereo(output, SR)
    tgt_b = rms_db_per_band_stereo(target, SR)

    delta_out_c = torch.clamp(out_b - raw_b, -20.0, +20.0)
    delta_tgt_c = torch.clamp(tgt_b - raw_b, -20.0, +20.0)

    # MSE par bande pondéré (poids 2× sur 500-2K)
    w = BAND_WEIGHTS.to(out_b.device)              # (20,)
    L_delta_band = (((delta_out_c - delta_tgt_c) ** 2) * w).mean()

    L_delta_corr = pearson_corr_loss(delta_out_c, delta_tgt_c)

    L_rms   = ((rms_db_batched(output) - rms_db_batched(target))**2).mean()
    L_crest = ((crest_batched(output) - crest_batched(target))**2).mean()

    L_rms_n        = L_rms        / SCALE_RMS
    L_delta_band_n = L_delta_band / SCALE_DELTA_BAND
    L_delta_corr_n = L_delta_corr / SCALE_DELTA_CORR
    L_crest_n      = L_crest      / SCALE_CREST

    total = L_rms_n + L_delta_band_n + L_delta_corr_n + L_crest_n

    # Cohérence temporelle : pénalise la variance des params dans le batch
    # (suppose batches "pseudo-consécutifs" : variations excessives au sein du
    # batch créent du vibrato à l'inférence)
    L_temp = torch.tensor(0.0, device=out_b.device)
    if params_norm is not None and params_norm.shape[0] > 1:
        var_per_param = params_norm.var(dim=0)     # (62,)
        L_temp = var_per_param.mean() / SCALE_TEMP_COH
        total = total + L_temp

    return {
        'total': total,
        'rms_db_delta':    L_rms.detach().sqrt(),
        'crest_delta':     L_crest.detach().sqrt(),
        'corr_one_minus':  L_delta_corr.detach(),
        'band_delta_db':   L_delta_band.detach().sqrt(),
        'temp_coh':        L_temp.detach(),
    }


# Alias pour ne pas casser le code train downstream
loss_v5_9 = loss_v5_12
loss_v5_4 = loss_v5_12


def _build_pair_cache_200ms(max_pairs):
    """V5.5 : construit le cache avec chunks 200 ms (vs 1s en v2)."""
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    cache_path = CACHE_DIR / 'pair_cache_v5_5_200ms.npz'
    print(f"Building cache 200 ms chunks ({max_pairs} paires)...")
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
            feats = compute_features_mid_only(r, sr=SR)
            if len(feats) == 0:
                continue
            pairs.append({
                'raw': r.T,
                'target': t.T,
                'features': feats,
                'slug': pair.slug,
            })
        if (i + 1) % 5 == 0:
            print(f"  {i+1} pairs done, {len(pairs)} chunks")
    print(f"Saving cache → {cache_path}")
    np.savez(str(cache_path), pairs=np.array(pairs, dtype=object))


def train(n_pairs=30, epochs=EPOCHS_DEFAULT, lr=LR_DEFAULT,
          weight_decay=WD_DEFAULT, batch_size=BATCH_DEFAULT, tag='v5_5'):
    CKPT_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Device: {DEVICE}")
    print(f"V9.5.3-v5.12 — bandes fines aigus + pondération 500-2K + temp coherence")
    print(f"  chunk_sec={CHUNK_SEC} lr={lr} batch={batch_size} epochs={epochs}")

    # V5.5 : cache dédié 200 ms
    cache_path = CACHE_DIR / 'pair_cache_v5_5_200ms.npz'
    if not cache_path.exists():
        _build_pair_cache_200ms(n_pairs)
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

            losses = loss_v5_12(raws, output, targets, params_norm=params_norm)
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
    ap.add_argument('--tag', type=str, default='v5_12')
    args = ap.parse_args()
    train(n_pairs=args.n_pairs, epochs=args.epochs, lr=args.lr,
          weight_decay=args.wd, batch_size=args.batch, tag=args.tag)
