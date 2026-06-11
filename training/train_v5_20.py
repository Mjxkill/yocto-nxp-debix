#!/usr/bin/env python3
"""
V9.5.3-v5.20 — fix écoute v5.19 ep18 : sub manquant, himid baveux, souffle HF.

Constat FFT bulldozer + écoute (2026-06-10) :
  - sub (20-60 Hz) : -7 dB vs target — la pondération 1/std écrasait les basses
  - himid (2-6 kHz) : +5 dB "baveux"
  - air : le modèle pousse l'ENVELOPPE HF (amplifie le noise floor = souffle)
    au lieu d'utiliser l'exciter (la loss spectrale ne distingue pas bruit
    et harmoniques — l'oreille si)

Correctifs :
  1. Poids bandes : base 1.0 partout (sub réapprend), himid ×1.25, air ×1.5
  2. Tilt 2 : Δ(sub − mid)² — contraint la fondation
  3. CAP enveloppe > 8 kHz : ±6 dB (au lieu de ±12) → l'air doit venir de l'exciter
  4. Loss spectral flatness 10-20 kHz : pénalise l'air bruité (souffle)
  5. Warm restart depuis conv_v5_19_full_epoch021.pt, batch 32, cosine 1e-4→1e-6

Constat écoute v5.18 ep5 : l'EQ à bandes larges (Q=1) booste le bruit avec le
signal (sifflement 14-16 kHz audible partout). Le modèle prédit maintenant une
ENVELOPPE de 64 gains (log 20 Hz - 20 kHz, ±12 dB) appliquée :
  - training : masque fréquentiel différentiable (surrogate_spectral_env)
  - board   : FIR 256 taps phase linéaire (latence 2.67 ms) — parité 0.43 dB

Sortie modèle : 74 = 64 env + 4 exciter + 6 limiter. Chaîne : env → exciter →
limiter (EQ paramétrique et stereo retirés ; mono par canal inchangé).
Loss : v5.18 + L_smooth (douceur de l'enveloppe, anti musical-noise).

Causes racines corrigées :
  1. Encoder : features_v3 (125) — Mel court terme garanti ≥2 bins (200Hz-20k)
     + FFT 8192 longue → 16 bandes BF précises 20-630 Hz + delta-Mel + carto
     de normalisation par bande (bruit de mesure égalisé).
  2. Surrogate exciter CALIBRÉ sur le vrai Calf (erreur 2.15 dB vs +5..19 avant).
  3. Loss : pondération par bande ∝ fiabilité de mesure (T4) × air ×2
     (l'héritage v5.12 pondérait les MEDIUMS ×2 — supprimé) + TILT loss
     Δ(air−mid)² qui vise la balance perçue directement.

Hérité v5.17 : MONO (mid), 26 outputs, filtre silences, Huber RMS, input 18 dB.

Différences vs v5.16 (suite diag plateau L_rms, validé utilisateur 2026-06-10) :
- **MONO** : target = mid du master (L+R)/2. Le modèle est appliqué
  indépendamment par canal sur le board (2 invocations NPU / cycle 10 ms).
  → élimine l'erreur side irréductible (raw mono ne peut pas créer du stéréo)
- **Output 26 params** : 16 EQ + 4 exciter + 6 limiter. Stereo RETIRÉ
  (neutre dans denormalize_params_v5_17).
- **Filtre silences** : chunks avec raw_rms < -35 dB exclus au chargement
  (10% du dataset = cibles impossibles +20..+161 dB qui polluaient le gradient).
- **Huber loss sur L_rms** (delta=3 dB) : quadratique < 3 dB, linéaire au-delà
  → les outliers restants ne dominent plus.
- **input_db élargi 0..+18 dB** (PARAM_RANGES_V5_17) : diag montrait lim_ig
  saturé à 0.77 — le modèle réclamait du headroom.

Héritées v5.16 : Mel+MFCC 85 features, fenêtre 100 ms hop 10 ms (sliding),
XXL_conv2d, anti-mode-collapse, L_compress=Δcrest², cosine LR 3e-4 → 1e-6.

Cache RÉUTILISÉ : pair_cache_v5_16_mel_100ms.npz (filtrage fait au load).
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
from torch.optim.lr_scheduler import CosineAnnealingLR

from dataset_loader import iter_pairs, load_audio
from features_v3 import compute_features_v3, N_FEATURES_V3
from model import (
    MasteringXXL_conv2d, denormalize_params_v5_17, denormalize_params_v5_19,
    N_FEATURES_IN_V3, N_PARAMS_OUT_V5_17, N_PARAMS_OUT_V5_19, n_parameters,
)
from surrogate_spectral_env import SpectralEnvSurrogate, ENV_FREQS
ENV_HF_MASK = torch.tensor(ENV_FREQS > 8000.0)   # V5.20 cap anti-souffle
from surrogate_chain import MasteringChainSurrogate


WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT_DIR  = WORKSPACE / 'checkpoints'
LOG_DIR   = WORKSPACE / 'logs'
CACHE_DIR = WORKSPACE / 'cache'

CHUNK_SEC = 0.1                    # V5.16 : window 100 ms (vs 200 ms)
SR        = 48000
CHUNK_N   = int(CHUNK_SEC * SR)    # 4 800 samples
HOP_SEC   = 0.01                   # V5.16 : sliding hop 10 ms (= 90% overlap)
HOP_N     = int(HOP_SEC * SR)      #   480 samples
DEVICE    = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

EPOCHS_DEFAULT = 30
LR_DEFAULT     = 1e-4    # V5.20 : warm restart → LR réduit
LR_MIN         = 1e-6    # V5.15 : eta_min cosine annealing
WD_DEFAULT     = 1e-3
BATCH_DEFAULT  = 32      # V5.20 : gradient moins bruité (mesures chunk bruitées)

# V5.8 : 20 bandes hybrides
#   6 bandes log    20-1000 Hz  (bass/medium détail)
#   14 bandes linéaires 1000-20000 Hz (aigus fins, finesse > 4 kHz)
# V5.12 : 4 log low + 4 log mid + 12 lin high pour finesse > 4 kHz
_log_low    = np.logspace(np.log10(20.0), np.log10(500.0), 5)
_log_mid    = np.logspace(np.log10(500.0), np.log10(4000.0), 5)
_lin_high   = np.linspace(4000.0, 20000.0, 13)
_edges      = np.concatenate([_log_low, _log_mid[1:], _lin_high[1:]])
BANDS_HZ    = [(float(_edges[i]), float(_edges[i+1])) for i in range(20)]

# V5.18 — pondération par bande = fiabilité de mesure × accent sur l'air.
# Fiabilité : 1/std inter-chunk mesurée (diag_freq_bias T4) — les basses sont
# 3× plus bruitées sur 100 ms, leur gradient est pour partie du bruit.
# Air : ×2 sur les bandes > 8 kHz (l'utilisateur veut "l'air" du master ;
# l'héritage v5.12 qui pondérait les MEDIUMS ×2 est SUPPRIMÉ).
_STD_T4 = np.array([12.0, 13.7, 10.1, 11.6, 11.5, 11.4, 9.5, 7.6,
                    6.0, 4.5, 4.9, 5.3, 5.0, 4.9, 4.8, 4.3,
                    4.0, 3.8, 3.85, 3.9], dtype=np.float32)
BAND_WEIGHTS_NP = (1.0 / _STD_T4)
BAND_WEIGHTS_NP[12:] *= 2.0          # air > 9.3 kHz
BAND_WEIGHTS_NP *= 20.0 / BAND_WEIGHTS_NP.sum()      # normalise (mean = 1)
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
SCALE_COMPRESS = 5.0     # V5.15 : weight ×5 (était 25 → /25 = 0.04 ; maintenant /5 = 0.2)
SCALE_MEL      = 1.0     # L1 mel-spec typique ~1
SCALE_TILT     = 25.0    # V5.18 : Δtilt typique 5-10 dB → ²=25-100 → /25 = 1-4
SCALE_SMOOTH   = 4.0
SCALE_FLAT     = 0.01    # V5.20 : ΔSF typique 0.05-0.2 → ²=0.0025-0.04 → /0.01 = 0.25-4     # V5.19 : douceur enveloppe ; marche 2 dB entre bandes → 4/4 = 1


def _mel_logspec_simple(x, sr=SR, n_fft=2048, n_mels=64):
    """Mel-spectrogram approx (loss perceptuelle V5.13).
    x : (B, 2, N). Returns (B, n_mels, n_frames) log scale.
    """
    B, C, N = x.shape
    x_mid = 0.5 * (x[:, 0] + x[:, 1])    # (B, N)
    win = torch.hann_window(n_fft, device=x.device)
    spec = torch.stft(x_mid, n_fft=n_fft, hop_length=n_fft // 4,
                      win_length=n_fft, window=win, return_complex=True)
    pwr = spec.abs() ** 2                 # (B, n_freq, n_frames)
    # Mel filterbank approximé via log bins. Pour simplicité, on prend
    # 64 bandes log-spaced 20 Hz → 20 kHz.
    n_freq = pwr.shape[1]
    freqs = torch.linspace(0, sr / 2, n_freq, device=x.device)
    mel_edges = torch.from_numpy(np.geomspace(20.0, sr/2 - 1, n_mels + 1)).float().to(x.device)
    mel = torch.zeros(B, n_mels, pwr.shape[-1], device=x.device)
    for m in range(n_mels):
        mask = (freqs >= mel_edges[m]) & (freqs < mel_edges[m+1])
        if mask.any():
            mel[:, m] = pwr[:, mask].sum(dim=1)
    return torch.log10(mel + 1e-10)


def loss_v5_12(raw, output, target, params_norm=None, env_db=None):
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

    # V5.17 — Huber sur Δrms (delta=3 dB) : quadratique sous 3 dB, linéaire
    # au-delà → les chunks outliers ne dominent plus le gradient (vs MSE pur).
    d_rms = rms_db_batched(output) - rms_db_batched(target)
    HUBER_DELTA = 3.0
    abs_d = d_rms.abs()
    L_rms = torch.where(abs_d <= HUBER_DELTA,
                        0.5 * d_rms ** 2,
                        HUBER_DELTA * (abs_d - 0.5 * HUBER_DELTA)).mean()
    L_crest = ((crest_batched(output) - crest_batched(target))**2).mean()

    # V5.16 (FIX) — Loss compression DÉCOUPLÉE en 2 termes (vs v5.13 piégeable).
    # AVANT : L_compress = Δcrest² + Δvariance_par_bande² → le modèle minimisait la
    # variance par bande en COUPANT l'EQ massivement (-10 dB sur les bandes basses),
    # détruisant l'audio au lieu de comprimer. C'est le piège du gradient observé
    # sur le checkpoint epoch 5.
    # MAINTENANT : juste Δcrest² (vrai indicateur de compression).
    # Le terme "loudness match" plus bas force la RMS output à matcher la RMS target,
    # ce qui empêche le cut EQ destructif.
    L_compress = ((crest_batched(output) - crest_batched(target))**2).mean()

    # V5.13 — Loss perceptuelle : L1 sur log Mel-spectrogram
    out_mel = _mel_logspec_simple(output, sr=SR)
    tgt_mel = _mel_logspec_simple(target, sr=SR)
    L_mel = torch.abs(out_mel - tgt_mel).mean()

    # V5.18 — TILT loss : balance air/mid perçue (cause du son "sourd" v5.17).
    # tilt = mean(bandes 15-19 = 13.3-20 kHz) − mean(bandes 6-8 = 1.4-5.3 kHz)
    out_tilt = out_b[:, 15:20].mean(dim=-1) - out_b[:, 6:9].mean(dim=-1)
    tgt_tilt = tgt_b[:, 15:20].mean(dim=-1) - tgt_b[:, 6:9].mean(dim=-1)
    L_tilt = ((out_tilt - tgt_tilt) ** 2).mean()

    # V5.20 — TILT SUB : balance fondation. sub = bandes 0-1 (20-100 Hz),
    # mid = bandes 6-8. Constat : modèle -7 dB de sub vs target (bulldozer).
    out_tsub = out_b[:, 0:2].mean(dim=-1) - out_b[:, 6:9].mean(dim=-1)
    tgt_tsub = tgt_b[:, 0:2].mean(dim=-1) - tgt_b[:, 6:9].mean(dim=-1)
    L_tsub = ((out_tsub - tgt_tsub) ** 2).mean()

    # V5.20 — SPECTRAL FLATNESS 10-20 kHz : distingue souffle (SF→1) des
    # harmoniques (SF→0). Le modèle poussait l'enveloppe HF (amplifie le
    # noise floor) au lieu de l'exciter ; la loss énergie ne le voyait pas.
    def _flatness_hf(sig):
        n_fft = 2048
        win = torch.hann_window(n_fft, device=sig.device)
        mid = 0.5 * (sig[:, 0] + sig[:, 1])
        sp = torch.stft(mid, n_fft=n_fft, hop_length=n_fft // 2,
                        win_length=n_fft, window=win, return_complex=True)
        pwr = sp.abs() ** 2 + 1e-12                  # (B, bins, frames)
        n_bins = pwr.shape[1]
        lo = int(10000 / (SR / 2) * (n_bins - 1))
        p = pwr[:, lo:, :]
        sf = torch.exp(torch.log(p).mean(dim=1)) / p.mean(dim=1)   # (B, frames)
        return sf.mean(dim=-1)                       # (B,)
    L_flat = ((_flatness_hf(output) - _flatness_hf(target)) ** 2).mean()

    # V5.16 FIX — LOUDNESS MATCH : poids fort pour interdire le cut EQ destructif.
    # SCALE_RMS_NEW = 5 → contribution ~0.4 si Δrms ~ 1 dB (au lieu de /25 → 0.08).
    # Si modèle coupe -8 dB → contribution = 64/5 = 12.8 = DOMINANT.
    SCALE_RMS_NEW = 5.0
    L_rms_n        = L_rms        / SCALE_RMS_NEW
    L_delta_band_n = L_delta_band / SCALE_DELTA_BAND
    L_delta_corr_n = L_delta_corr / SCALE_DELTA_CORR
    L_crest_n      = L_crest      / SCALE_CREST
    # SCALE_COMPRESS = 5 fait L_compress (= Δcrest²) ~ contribution proportionnée
    # (typique Δcrest = 3 → /5 = 1.8, modéré).
    L_compress_n   = L_compress   / SCALE_COMPRESS
    L_mel_n        = L_mel        / SCALE_MEL

    L_tilt_n = L_tilt / SCALE_TILT
    L_tsub_n = L_tsub / SCALE_TILT
    L_flat_n = L_flat / SCALE_FLAT

    # V5.19 — douceur de l'enveloppe spectrale (anti musical-noise) :
    # pénalise les marches entre bandes adjacentes.
    if env_db is not None:
        L_smooth = ((env_db[:, 1:] - env_db[:, :-1]) ** 2).mean() / SCALE_SMOOTH
    else:
        L_smooth = torch.tensor(0.0, device=out_b.device)
    L_smooth_n = L_smooth

    total = (L_rms_n + L_delta_band_n + L_delta_corr_n + L_crest_n
             + L_compress_n + L_mel_n + L_tilt_n + L_tsub_n + L_flat_n
             + L_smooth_n)

    # V5.16 — ANTI-mode-collapse : pénalise std=0 (modèle qui prédit constante).
    # Si std des params dans le batch est trop basse, augmente la loss.
    # MIN_STD = 0.1 (~10% du range normalisé) : on veut au moins ça de diversité.
    L_temp = torch.tensor(0.0, device=out_b.device)
    if params_norm is not None and params_norm.shape[0] > 1:
        MIN_STD = 0.1
        std_per_param = params_norm.std(dim=0)         # (N_PARAMS,)
        # ReLU-like : pénalité si std < MIN_STD
        deficit = torch.clamp(MIN_STD - std_per_param, min=0.0)
        L_temp = (deficit ** 2).mean() * 50.0           # poids fort
        total = total + L_temp

    return {
        'total':           total,
        'rms_db_delta':    L_rms.detach().sqrt(),
        'crest_delta':     L_crest.detach().sqrt(),
        'corr_one_minus':  L_delta_corr.detach(),
        'band_delta_db':   L_delta_band.detach().sqrt(),
        'temp_coh':        L_temp.detach(),
        # V5.16 — détail des composantes pondérées (= contribution au total)
        'L_rms_n':         L_rms_n.detach(),
        'L_band_n':        L_delta_band_n.detach(),
        'L_corr_n':        L_delta_corr_n.detach(),
        'L_crest_n':       L_crest_n.detach(),
        'L_compress_n':    L_compress_n.detach(),
        'L_mel_n':         L_mel_n.detach(),
        'L_tilt_n':        L_tilt_n.detach(),
        'L_smooth_n':      L_smooth_n.detach(),
        'L_tsub_n':        L_tsub_n.detach(),
        'L_flat_n':        L_flat_n.detach(),
    }


# Alias pour ne pas casser le code train downstream
loss_v5_9 = loss_v5_12
loss_v5_4 = loss_v5_12


N_AUGMENTATIONS = 4              # par paire : original + 3 augmentations
N_CHUNKS_PER_PAIR_MAX = 100      # V5.14 : subsample pour cap durée d'epoch


def _augment_chunk(r, t, aug_idx, rng):
    """V5.13 — applique une augmentation déterministe au chunk (raw, target).

    aug_idx :
      0 : original (no aug)
      1 : random gain ±6 dB (appliqué à raw seul, target intact)
      2 : random gain ±3 dB (raw) + pre-EQ low order
      3 : channel swap L/R (sur target ; raw mono inchangé) + petit gain raw
    """
    if aug_idx == 0:
        return r, t
    if aug_idx == 1:
        gain_db = rng.uniform(-6, 6)
        factor = 10.0 ** (gain_db / 20.0)
        return (r.astype(np.float32) * factor).astype(np.float32), t
    if aug_idx == 2:
        gain_db = rng.uniform(-3, 3)
        factor = 10.0 ** (gain_db / 20.0)
        r_aug = r.astype(np.float32) * factor
        # Pre-EQ random : tilt low-shelf 100 Hz, gain ±3 dB
        from scipy.signal import iirfilter, sosfiltfilt
        tilt_db = rng.uniform(-3, 3)
        # Simple 1-pole HP/LP mix
        cutoff = 800.0
        sos = iirfilter(2, cutoff / (SR / 2), btype='low', ftype='butter', output='sos')
        r_low = sosfiltfilt(sos, r_aug.astype(np.float64), axis=0).astype(np.float32)
        r_aug = (r_aug + (tilt_db / 6.0) * r_low).astype(np.float32)
        return r_aug, t
    if aug_idx == 3:
        # Channel swap target L↔R + tiny gain raw
        if t.shape[1] == 2:
            t_aug = t[:, ::-1].copy().astype(np.float32)
        else:
            t_aug = t
        gain_db = rng.uniform(-1, 1)
        factor = 10.0 ** (gain_db / 20.0)
        return (r.astype(np.float32) * factor).astype(np.float32), t_aug
    return r, t


def _build_pair_cache_200ms(max_pairs):
    """V5.13 : construit le cache avec 200 ms chunks × augmentations."""
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    cache_path = CACHE_DIR / 'pair_cache_v5_18_mel125_100ms.npz'
    print(f"Building cache {int(CHUNK_SEC*1000)} ms chunks × {N_AUGMENTATIONS} aug ({max_pairs} paires)...")
    pairs = []
    rng = np.random.default_rng(seed=42)
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
        # V5.16 — sliding window : chunk = CHUNK_N samples, hop = HOP_N samples.
        # Nombre de positions possibles = (n - CHUNK_N) // HOP_N + 1
        n_positions = max(0, (n - CHUNK_N) // HOP_N + 1)
        if n_positions == 0:
            continue
        # Subsample N_CHUNKS_PER_PAIR_MAX positions aléatoires pour cap durée.
        if n_positions > N_CHUNKS_PER_PAIR_MAX:
            chunk_indices = rng.choice(n_positions, size=N_CHUNKS_PER_PAIR_MAX, replace=False)
        else:
            chunk_indices = np.arange(n_positions)
        for c in chunk_indices:
            i0 = int(c) * HOP_N
            i1 = i0 + CHUNK_N
            r_base = raw[i0:i1].astype(np.float32)
            t_base = tgt[i0:i1].astype(np.float32)
            for aug_idx in range(N_AUGMENTATIONS):
                r, t = _augment_chunk(r_base, t_base, aug_idx, rng)
                feats = compute_features_v3(r, sr=SR)
                if len(feats) == 0:
                    continue
                pairs.append({
                    'raw': r.T,
                    'target': t.T,
                    'features': feats,
                    'slug': pair.slug,
                    'aug': aug_idx,
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
    print(f"V9.5.3-v5.20 — sub/himid/anti-souffle (warm restart v5.19 ep21, batch 32)")
    print(f"  chunk_sec={CHUNK_SEC} lr={lr} batch={batch_size} epochs={epochs}")

    # Cache v5.16 réutilisé (mêmes chunks/features) ; transformations v5.17
    # appliquées au chargement.
    cache_path = CACHE_DIR / 'pair_cache_v5_18_mel125_100ms.npz'
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
    n_before = len(pairs)

    # V5.17 — (a) filtre silences : raw_rms < -35 dB exclus ;
    #         (b) target MONO : mid du master dupliqué sur 2 canaux
    #             (compatible chaîne/loss stéréo existantes, side=0).
    SILENCE_DB = -35.0
    kept = []
    for p in pairs:
        raw = p['raw']                      # (2, N) mono dupliqué
        rms = np.sqrt((raw ** 2).mean()) + 1e-12
        if 20 * np.log10(rms) < SILENCE_DB:
            continue
        tgt = p['target']                   # (2, N) vrai stéréo
        mid = 0.5 * (tgt[0] + tgt[1])
        p['target'] = np.stack([mid, mid]).astype(np.float32)
        kept.append(p)
    pairs = kept
    print(f"Total chunks: {len(pairs)} (filtre silences : {n_before - len(pairs)} exclus / {n_before})")

    model = MasteringXXL_conv2d(n_input=N_FEATURES_V3,
                                 n_out=N_PARAMS_OUT_V5_19).to(DEVICE)
    # V5.20 — warm restart depuis v5.19 epoch 021
    _warm = CKPT_DIR / 'conv_v5_19_full_epoch021.pt'
    if _warm.exists():
        _ck = torch.load(str(_warm), map_location=DEVICE, weights_only=False)
        model.load_state_dict(_ck['model'])
        print(f"Warm restart depuis {_warm.name} (epoch {_ck.get('epoch')})")
    print(f"Model: MasteringXXL_conv2d — {n_parameters(model):,} params (in={N_FEATURES_V3}, out={N_PARAMS_OUT_V5_19})")
    env_surro = SpectralEnvSurrogate(sr=SR).to(DEVICE)
    for m in model.modules():
        if isinstance(m, nn.Linear):
            nn.init.zeros_(m.bias)
    chain = MasteringChainSurrogate(sr=SR).to(DEVICE)
    chain.eval()
    for p in chain.parameters():
        p.requires_grad_(False)

    optimizer = optim.Adam(model.parameters(), lr=lr, weight_decay=weight_decay)
    # V5.15 : cosine annealing 3e-4 → 1e-6 sur N epochs (vs ReduceLROnPlateau)
    scheduler = CosineAnnealingLR(optimizer, T_max=epochs, eta_min=LR_MIN)
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
        # V5.16 — accumulateurs détaillés
        ep_L_rms = ep_L_band = ep_L_corr = ep_L_crest = 0.0
        ep_L_comp = ep_L_mel = ep_L_mc = ep_L_tilt = ep_L_smooth = ep_L_tsub = ep_L_flat = 0.0
        ep_rms  = 0.0
        ep_corr = 0.0
        for b in range(n_batches):
            batch = pairs[b * batch_size:(b + 1) * batch_size]
            # V5.17 : skip batch < 2 — le surrogate exciter/limiter ne gère pas
            # B=1 (chemin batché exige shape[0] > 1 pour lfilter).
            if len(batch) < 2:
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
            p19 = denormalize_params_v5_19(params_norm)
            env_db = p19['env']['gains_db']                  # (B, 64)
            # V5.20 — CAP anti-souffle : au-dessus de 8 kHz, enveloppe ±6 dB max
            # (l'air doit venir de l'exciter, pas du boost du noise floor)
            env_db = torch.where(ENV_HF_MASK.to(env_db.device),
                                  env_db.clamp(-6.0, 6.0), env_db)
            x_env = env_surro(raws, env_db)                  # enveloppe d'abord
            # Chaîne existante avec EQ + stereo NEUTRES (gains 0 / passthrough),
            # exciter + limiter pilotés par le modèle.
            Bsz = env_db.shape[0]
            from model import FIXED_EQ_FREQS_V5_14, FIXED_EQ_QS_V5_14
            neutral_eq = {
                'freq': torch.tensor([list(FIXED_EQ_FREQS_V5_14)] * Bsz, device=DEVICE),
                'gain_db': torch.zeros(Bsz, 16, device=DEVICE),
                'q': torch.tensor([list(FIXED_EQ_QS_V5_14)] * Bsz, device=DEVICE),
            }
            def cst(v):
                return torch.full((Bsz,), float(v), device=DEVICE)
            params = {
                'eq': neutral_eq,
                'exciter': p19['exciter'],
                'stereo': {'balance': cst(0.0), 'mid_gain': cst(1.0),
                           'side_gain': cst(1.0), 'sm_swap': cst(0.0)},
                'limiter': p19['limiter'],
            }
            output = chain(x_env, params=params)

            losses = loss_v5_12(raws, output, targets, params_norm=params_norm,
                                env_db=env_db)
            losses['total'].backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=1.0)
            optimizer.step()
            ep_loss += losses['total'].item()
            ep_rms  += losses['rms_db_delta'].item()
            ep_corr += losses['corr_one_minus'].item()
            ep_L_rms  += losses['L_rms_n'].item()
            ep_L_band += losses['L_band_n'].item()
            ep_L_corr += losses['L_corr_n'].item()
            ep_L_crest+= losses['L_crest_n'].item()
            ep_L_comp += losses['L_compress_n'].item()
            ep_L_mel  += losses['L_mel_n'].item()
            ep_L_tilt += losses['L_tilt_n'].item()
            ep_L_smooth += losses['L_smooth_n'].item()
            ep_L_tsub += losses['L_tsub_n'].item()
            ep_L_flat += losses['L_flat_n'].item()
            ep_L_mc   += losses['temp_coh'].item()

        dt = time.time() - t0
        m_loss = ep_loss / n_batches
        m_rms  = ep_rms  / n_batches
        m_corr = ep_corr / n_batches
        scheduler.step()   # cosine : pas d'arg (vs ReduceLROnPlateau)
        cur_lr = optimizer.param_groups[0]['lr']
        rec = {
            'epoch': epoch,
            'loss': m_loss,
            'mean_rms_delta_db': m_rms,
            'mean_corr_one_minus': m_corr,
            'lr': cur_lr,
            'dt_sec': round(dt, 1),
            # V5.16 — contributions pondérées au total (= valeur effective dans la loss)
            'L_rms':      round(ep_L_rms  / n_batches, 4),
            'L_band':     round(ep_L_band / n_batches, 4),
            'L_corr':     round(ep_L_corr / n_batches, 4),
            'L_crest':    round(ep_L_crest/ n_batches, 4),
            'L_compress': round(ep_L_comp / n_batches, 4),
            'L_mel':      round(ep_L_mel  / n_batches, 4),
            'L_tilt':     round(ep_L_tilt / n_batches, 4),   # balance air/mid
            'L_smooth':   round(ep_L_smooth / n_batches, 4), # douceur enveloppe
            'L_tsub':     round(ep_L_tsub / n_batches, 4),   # fondation sub
            'L_flat':     round(ep_L_flat / n_batches, 4),   # anti-souffle HF
            'L_mc':       round(ep_L_mc   / n_batches, 4),   # anti-mode-collapse
        }
        log_f.write(json.dumps(rec) + '\n')
        log_f.flush()
        print(f"  epoch {epoch:>3d} loss={m_loss:.2f} | "
              f"L_rms={rec['L_rms']:.2f} L_band={rec['L_band']:.2f} "
              f"L_corr={rec['L_corr']:.2f} L_crest={rec['L_crest']:.2f} "
              f"L_comp={rec['L_compress']:.2f} L_mel={rec['L_mel']:.2f} L_tilt={rec['L_tilt']:.2f} L_sm={rec['L_smooth']:.2f} L_tsub={rec['L_tsub']:.2f} L_flat={rec['L_flat']:.2f} "
              f"L_mc={rec['L_mc']:.2f} | rms_delta={m_rms:+.2f}dB "
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
    ap.add_argument('--tag', type=str, default='v5_20')
    args = ap.parse_args()
    train(n_pairs=args.n_pairs, epochs=args.epochs, lr=args.lr,
          weight_decay=args.wd, batch_size=args.batch, tag=args.tag)
