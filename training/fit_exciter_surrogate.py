#!/usr/bin/env python3
"""Fit du surrogate exciter corrigé sur les mesures du vrai Calf.

Forme candidate (différentiable, simple) :
  high = HPF2(x, freq_mult × freq)        # ordre 2 = pente plus raide
  wet  = tanh(drive × high)
  y    = x + amount × beta(drive) × wet
  beta(drive) = b0 / (1 + b1 × drive)     # compense la croissance linéaire en drive

Paramètres fittés : freq_mult, b0, b1 — minimisent l'écart par bande (10-20 kHz)
sur la grille amount × drive mesurée.
"""
import sys, numpy as np, torch
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from train_v5_17 import rms_db_per_band_stereo, BANDS_HZ, SR
from surrogate_exciter import highpass_biquad
from surrogate_eq import lfilter_batched

cal = np.load('/tmp/exciter_calibration.npz')
GRID_AMOUNT = list(cal['grid_amount'])
GRID_DRIVE  = list(cal['grid_drive'])
base_b = cal['base']

# Bruit rose identique (re-seed)
rng = np.random.default_rng(3)
N = SR * 5
white = rng.standard_normal(N + 1)
spec = np.fft.rfft(white)
fr = np.fft.rfftfreq(len(white), 1/SR)
spec[1:] /= np.sqrt(fr[1:])
pink = np.fft.irfft(spec)[:N].astype(np.float32)
pink *= 0.1 / np.sqrt((pink**2).mean())
N = N // 480 * 480
pink = pink[:N]
xt = torch.from_numpy(np.stack([np.stack([pink, pink])]*2))   # (2,2,N)

def bands_db_t(y):
    return rms_db_per_band_stereo(y[:1], SR)[0].numpy()

def run_candidate(amount, drive, freq, freq_mult, b0, b1):
    B = 2
    t = lambda v: torch.full((B,), float(v))
    f_eff = t(freq * freq_mult)
    q = torch.full((B,), 0.707)
    b_co, a_co = highpass_biquad(f_eff, q, SR)
    with torch.no_grad():
        h1 = lfilter_batched(xt, a_co, b_co)
        h2 = lfilter_batched(h1, a_co, b_co)       # ordre 2
        wet = torch.tanh(t(drive).view(B,1,1) * h2)
        beta = b0 / (1.0 + b1 * drive)
        y = xt + t(amount).view(B,1,1) * beta * wet
    return y

def grid_error(freq_mult, b0, b1, verbose=False):
    errs = []
    for am in GRID_AMOUNT:
        for dr in GRID_DRIVE:
            calf_b = cal[f'calf_{am}_{dr}']
            y = run_candidate(am, dr, 8000.0, freq_mult, b0, b1)
            sb = bands_db_t(y) - base_b
            # erreur sur les bandes 10-19 (6.7-20 kHz), pondérée air ×2
            w = np.ones(10); w[5:] = 2.0
            e = np.abs(sb[10:20] - calf_b[10:20]) * w
            errs.append(e.mean())
            if verbose:
                print(f'  a={am} d={dr} : air surr {sb[15:20].mean():+.2f} vs calf {calf_b[15:20].mean():+.2f}')
    return np.mean(errs)

# Recherche en grille grossière puis raffinement
best = (None, 1e9)
for fm in [1.0, 1.3, 1.6, 2.0]:
    for b0 in [0.1, 0.2, 0.4, 0.7, 1.0]:
        for b1 in [0.0, 0.3, 0.7, 1.5]:
            e = grid_error(fm, b0, b1)
            if e < best[1]:
                best = ((fm, b0, b1), e)
print(f'Grossier : best {best[0]} err {best[1]:.2f} dB')

fm0, b00, b10 = best[0]
for fm in np.linspace(max(0.8, fm0-0.3), fm0+0.3, 5):
    for b0 in np.linspace(max(0.05, b00-0.15), b00+0.15, 5):
        for b1 in np.linspace(max(0.0, b10-0.3), b10+0.3, 5):
            e = grid_error(fm, b0, b1)
            if e < best[1]:
                best = ((fm, b0, b1), e)
print(f'Raffiné : best {best[0]} err {best[1]:.2f} dB')

fm, b0, b1 = best[0]
print(f'\nValidation par point de grille (air 13-20 kHz) :')
grid_error(fm, b0, b1, verbose=True)
print(f'\nPARAMS FITTÉS : freq_mult={fm:.3f}  b0={b0:.3f}  b1={b1:.3f}  err={best[1]:.2f} dB')
