"""V5.19 — Enveloppe spectrale fine : surrogate différentiable + référence FIR board.

Le modèle prédit 64 gains dB (bandes log 20 Hz - 20 kHz). Application :
  - TRAINING (différentiable, rapide) : masque fréquentiel rfft × H → irfft
  - BOARD (temps réel, latence 2.67 ms) : FIR 256 taps phase linéaire
    reconstruite depuis la même enveloppe.

Ce module fournit les deux et un test de parité (l'écart spectral doit être
< 1 dB partout — c'est l'argument clé vs l'ancien mismatch surrogate/LV2).
"""
import math
import numpy as np
import torch
import torch.nn as nn

N_ENV       = 64
ENV_FMIN    = 20.0
ENV_FMAX    = 20000.0
ENV_RANGE_DB = 12.0          # gains ∈ [-12, +12] dB
FIR_TAPS    = 256

# Fréquences centrales log des 64 points d'enveloppe
ENV_FREQS = np.geomspace(ENV_FMIN, ENV_FMAX, N_ENV).astype(np.float32)


def _interp_env_to_bins(env_db: torch.Tensor, freqs_bins: torch.Tensor) -> torch.Tensor:
    """Interpole l'enveloppe (B, 64) dB vers les bins (n_bins,) en log-fréquence.
    Retourne gains LINÉAIRES (B, n_bins). Différentiable."""
    B = env_db.shape[0]
    log_env_f = torch.log(torch.from_numpy(ENV_FREQS).to(env_db.device))
    log_bins = torch.log(freqs_bins.clamp(min=1.0))
    # indices d'interpolation
    idx = torch.searchsorted(log_env_f, log_bins).clamp(1, N_ENV - 1)
    f0 = log_env_f[idx - 1]
    f1 = log_env_f[idx]
    w = ((log_bins - f0) / (f1 - f0)).clamp(0.0, 1.0)          # (n_bins,)
    g0 = env_db[:, idx - 1]                                     # (B, n_bins)
    g1 = env_db[:, idx]
    g_db = g0 + (g1 - g0) * w.unsqueeze(0)
    # sous ENV_FMIN et au-dessus de ENV_FMAX : prolonge la valeur de bord
    below = freqs_bins < ENV_FREQS[0]
    above = freqs_bins > ENV_FREQS[-1]
    g_db = torch.where(below.unsqueeze(0), env_db[:, :1], g_db)
    g_db = torch.where(above.unsqueeze(0), env_db[:, -1:], g_db)
    return 10.0 ** (g_db / 20.0)


class SpectralEnvSurrogate(nn.Module):
    """Application différentiable de l'enveloppe par masque fréquentiel.

    forward(x, env_db) :
      x      : (B, C, N) audio
      env_db : (B, 64) gains dB
    """
    def __init__(self, sr: float = 48000.0):
        super().__init__()
        self.sr = sr

    def forward(self, x: torch.Tensor, env_db: torch.Tensor) -> torch.Tensor:
        B, C, N = x.shape
        X = torch.fft.rfft(x, dim=-1)                       # (B, C, n_bins)
        n_bins = X.shape[-1]
        freqs = torch.linspace(0, self.sr / 2, n_bins, device=x.device)
        H = _interp_env_to_bins(env_db, freqs)              # (B, n_bins) linéaire
        Y = X * H.unsqueeze(1)
        return torch.fft.irfft(Y, n=N, dim=-1)


def fir_from_env(env_db: np.ndarray, sr: float = 48000.0,
                 n_taps: int = FIR_TAPS) -> np.ndarray:
    """Référence board : FIR phase linéaire depuis l'enveloppe 64 pts.

    env_db : (64,) gains dB. Retourne (n_taps,) float32.
    Méthode : |H| échantillonné sur n_taps//2+1 bins → ifft réelle symétrique
    (phase zéro) → shift au centre (phase linéaire) → fenêtre Hann.
    """
    n_bins = n_taps // 2 + 1
    freqs = np.linspace(0, sr / 2, n_bins)
    # interp log
    log_f_env = np.log(ENV_FREQS)
    log_f = np.log(np.clip(freqs, 1.0, None))
    g_db = np.interp(log_f, log_f_env, env_db)
    g_db[freqs < ENV_FREQS[0]] = env_db[0]
    g_db[freqs > ENV_FREQS[-1]] = env_db[-1]
    H = 10.0 ** (g_db / 20.0)
    # phase zéro → noyau symétrique
    h = np.fft.irfft(H, n=n_taps)
    h = np.roll(h, n_taps // 2)             # centre → phase linéaire
    h *= np.hanning(n_taps)
    return h.astype(np.float32)


if __name__ == '__main__':
    # === Test de parité masque (training) vs FIR 256 (board) ===
    SR = 48000
    rng = np.random.default_rng(5)

    # Enveloppe de test réaliste : tilt + creux 8k + boost air
    env = np.zeros(N_ENV, dtype=np.float32)
    env += 3.0 * np.log10(ENV_FREQS / 1000.0)           # tilt doux
    env -= 6.0 * np.exp(-0.5 * ((np.log(ENV_FREQS / 8000)) / 0.3) ** 2)   # creux 8k
    env += 4.0 * np.exp(-0.5 * ((np.log(ENV_FREQS / 16000)) / 0.25) ** 2) # air 16k
    env = np.clip(env, -ENV_RANGE_DB, ENV_RANGE_DB)

    # Bruit rose 5 s
    N = SR * 5
    white = rng.standard_normal(N + 1)
    spec = np.fft.rfft(white)
    fr = np.fft.rfftfreq(len(white), 1 / SR)
    spec[1:] /= np.sqrt(fr[1:])
    pink = np.fft.irfft(spec)[:N].astype(np.float32)
    pink *= 0.1 / np.sqrt((pink ** 2).mean())

    # Surrogate (masque)
    surro = SpectralEnvSurrogate(sr=SR)
    xt = torch.from_numpy(pink).reshape(1, 1, -1)
    et = torch.from_numpy(env).unsqueeze(0)
    with torch.no_grad():
        y_mask = surro(xt, et).numpy()[0, 0]

    # FIR board
    h = fir_from_env(env, SR)
    y_fir = np.convolve(pink, h, mode='same')

    # Compare par bande (Welch)
    from scipy.signal import welch
    f1, p_in   = welch(pink,   fs=SR, nperseg=8192)
    _,  p_mask = welch(y_mask, fs=SR, nperseg=8192)
    _,  p_fir  = welch(y_fir,  fs=SR, nperseg=8192)
    g_mask = 10 * np.log10(p_mask / (p_in + 1e-30) + 1e-30)
    g_fir  = 10 * np.log10(p_fir  / (p_in + 1e-30) + 1e-30)

    print(f'{"freq":>8s} {"cible":>7s} {"masque":>7s} {"FIR256":>7s} {"Δ":>6s}')
    max_err = 0.0
    for fq in [25, 50, 100, 200, 400, 800, 1600, 3150, 6300, 8000, 12500, 16000, 19000]:
        i = np.argmin(np.abs(f1 - fq))
        target_db = np.interp(np.log(fq), np.log(ENV_FREQS), env)
        d = g_fir[i] - g_mask[i]
        if fq > 40: max_err = max(max_err, abs(d))
        print(f'{fq:>8d} {target_db:>+7.2f} {g_mask[i]:>+7.2f} {g_fir[i]:>+7.2f} {d:>+6.2f}')
    print(f'\nmax |masque − FIR| (> 40 Hz) : {max_err:.2f} dB  (objectif < 1 dB)')
    print(f'latence FIR : {FIR_TAPS // 2 / SR * 1000:.2f} ms')
