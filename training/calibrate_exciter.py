#!/usr/bin/env python3
"""Calibration du surrogate exciter sur le VRAI Calf Exciter (LV2 PC).

1. Mesure : bruit rose → Calf Exciter seul, grille amount × drive
   → gain dB par bande (20 bandes loss) vs bypass.
2. Mesure le surrogate actuel sur la même grille.
3. Affiche les deux cartes → base pour le fit du surrogate corrigé.
"""
import sys, numpy as np, torch
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from train_v5_17 import rms_db_per_band_stereo, BANDS_HZ, SR
from surrogate_exciter import CalfExciterSurrogate
from lv2_chain import Chain as RealLV2Chain

BLOCK = 480
rng = np.random.default_rng(3)

# Bruit rose 5 s
N = SR * 5
white = rng.standard_normal(N + 1)
spec = np.fft.rfft(white)
fr = np.fft.rfftfreq(len(white), 1/SR)
spec[1:] /= np.sqrt(fr[1:])
pink = np.fft.irfft(spec)[:N].astype(np.float32)
pink *= 0.1 / np.sqrt((pink**2).mean())     # RMS -20 dB
N = N // BLOCK * BLOCK
pink = pink[:N]

def bands_db(sig):
    st = torch.from_numpy(np.stack([sig, sig])[None]).float()
    return rms_db_per_band_stereo(st, SR)[0].numpy()

base_b = bands_db(pink)

# ---- Vrai Calf Exciter (seul dans la chaîne) ----
chain = RealLV2Chain(sr=SR, block=BLOCK)
chain.add('http://calf.sourceforge.net/plugins/Exciter')

def run_calf(amount, drive, freq=8000.0, ceil_=1.0):
    chain.set_param(0, 'amount', float(amount))
    chain.set_param(0, 'drive',  float(drive))
    chain.set_param(0, 'freq',   float(freq))
    chain.set_param(0, 'ceil',   float(ceil_))
    out = np.zeros(N, dtype=np.float32)
    for blk in range(N // BLOCK):
        s = blk * BLOCK
        l, r = chain.process(pink[s:s+BLOCK], pink[s:s+BLOCK])
        out[s:s+BLOCK] = 0.5 * (l + r)
    return out

# ---- Surrogate actuel ----
surro = CalfExciterSurrogate(sr=SR).eval()
def run_surro(amount, drive, freq=8000.0, ceil_=1.0):
    B = 2
    xt = torch.from_numpy(np.stack([np.stack([pink, pink])]*B))
    def t(v): return torch.full((B,), float(v))
    with torch.no_grad():
        y = surro(xt, amount=t(amount), drive=t(drive),
                  freq_hz=t(freq), ceiling=t(ceil_))
    return y[0, 0].numpy()

GRID_AMOUNT = [0.25, 0.5, 1.0]
GRID_DRIVE  = [1.0, 3.0, 6.0]

# Zones de résumé : high = bandes 10-14 (6.7k-13.3k), air = 15-19 (13.3k-20k)
print('Gain dB vs bypass — zone AIR (13-20 kHz) et HIGH (6.7-13 kHz)')
print(f'{"amount":>7s} {"drive":>6s} | {"Calf high":>9s} {"Calf air":>9s} | {"Surr high":>9s} {"Surr air":>9s} | {"Δhigh":>7s} {"Δair":>7s}')
results = {}
for am in GRID_AMOUNT:
    for dr in GRID_DRIVE:
        yc = run_calf(am, dr)
        ys = run_surro(am, dr)
        cb = bands_db(yc) - base_b
        sb = bands_db(ys) - base_b
        c_high, c_air = cb[10:15].mean(), cb[15:20].mean()
        s_high, s_air = sb[10:15].mean(), sb[15:20].mean()
        results[(am, dr)] = (cb, sb)
        print(f'{am:>7.2f} {dr:>6.1f} | {c_high:>+9.2f} {c_air:>+9.2f} | {s_high:>+9.2f} {s_air:>+9.2f} | {s_high-c_high:>+7.2f} {s_air-c_air:>+7.2f}')

# Détail par bande pour amount=1.0 drive=6.0 (cas extrême)
print('\nDétail par bande (amount=1.0, drive=6.0) :')
cb, sb = results[(1.0, 6.0)]
print(f'{"bande":>6s} {"lo-hi Hz":>15s} {"Calf":>8s} {"Surro":>8s} {"Δ":>8s}')
for i in range(8, 20):
    lo, hi = BANDS_HZ[i]
    print(f'{i:>6d} {f"{lo:.0f}-{hi:.0f}":>15s} {cb[i]:>+8.2f} {sb[i]:>+8.2f} {sb[i]-cb[i]:>+8.2f}')

np.savez('/tmp/exciter_calibration.npz',
         grid_amount=GRID_AMOUNT, grid_drive=GRID_DRIVE,
         **{f'calf_{am}_{dr}': results[(am, dr)][0] for am in GRID_AMOUNT for dr in GRID_DRIVE},
         **{f'surr_{am}_{dr}': results[(am, dr)][1] for am in GRID_AMOUNT for dr in GRID_DRIVE},
         base=base_b)
print('\nMesures → /tmp/exciter_calibration.npz')
