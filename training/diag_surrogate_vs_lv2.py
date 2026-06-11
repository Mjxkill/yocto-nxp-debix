#!/usr/bin/env python3
"""Confirmation cause 2 : surrogate PyTorch vs vraie chaîne LV2 — mêmes params, même audio.

Mesure le spectre par bande des deux sorties. Si le surrogate génère plus d'air
que la LV2 avec les mêmes params, le modèle optimise dans un simulateur faux.

3 jeux de params testés :
  P1 = params moyens prédits par le modèle v5.17 (amount 0.4, EQ air +3)
  P2 = air agressif (EQ +12 sur 5 dernières bandes, exciter amount=1.0 drive=6)
  P3 = neutre (EQ 0, exciter amount=0)
"""
import sys, numpy as np, torch
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from model import FIXED_EQ_FREQS_V5_14, FIXED_EQ_QS_V5_14
from surrogate_chain import MasteringChainSurrogate
from train_v5_17 import rms_db_per_band_stereo, BANDS_HZ, SR
from dataset_loader import iter_pairs, load_audio
from lv2_chain import Chain as RealLV2Chain

BLOCK = 480

# Audio test : 5 s de musique réelle
pair = next(iter_pairs())
raw_full, _ = load_audio(pair.raw_path, target_sr=SR)
x = 0.5*(raw_full[SR*60:SR*65, 0] + raw_full[SR*60:SR*65, 1]).astype(np.float32)
N = len(x) // BLOCK * BLOCK
x = x[:N]

def param_set(eq_gains, amount, drive, exc_freq, exc_ceil, th_db, in_db, out_db, at, rt):
    return dict(eq_gains=np.array(eq_gains, dtype=np.float32), amount=amount, drive=drive,
                exc_freq=exc_freq, exc_ceil=exc_ceil, th_db=th_db, in_db=in_db,
                out_db=out_db, at=at, rt=rt)

P1 = param_set([4.7,0.9,-0.5,-1.2,-0.4,-0.1,-3.1,-1.5,-0.5,0.1,1.9,4.6,-1.7,2.2,-1.0,3.3],
               amount=0.41, drive=4.85, exc_freq=8000.0, exc_ceil=0.75,
               th_db=-9.3, in_db=6.8, out_db=-2.8, at=4.2, rt=99.0)
P2 = param_set([0]*11 + [12.0]*5,
               amount=1.0, drive=6.0, exc_freq=8000.0, exc_ceil=1.0,
               th_db=-6.0, in_db=3.0, out_db=0.0, at=4.0, rt=100.0)
P3 = param_set([0]*16, amount=0.0, drive=1.0, exc_freq=8000.0, exc_ceil=1.0,
               th_db=0.0, in_db=0.0, out_db=0.0, at=4.0, rt=100.0)

# ---- Surrogate ----
chain_s = MasteringChainSurrogate(sr=SR).eval()
for p in chain_s.parameters(): p.requires_grad_(False)

def run_surrogate(P):
    B = 2  # surrogate exige batch > 1 — duplique
    xt = torch.from_numpy(np.stack([np.stack([x, x])]*B))   # (2, 2, N)
    def t(v): return torch.full((B,), float(v))
    params = {
        'eq': {'freq': torch.tensor([list(FIXED_EQ_FREQS_V5_14)]*B),
               'gain_db': torch.tensor([list(P['eq_gains'])]*B),
               'q': torch.tensor([list(FIXED_EQ_QS_V5_14)]*B)},
        'exciter': {'amount': t(P['amount']), 'drive': t(P['drive']),
                    'freq_hz': t(P['exc_freq']), 'ceiling': t(P['exc_ceil'])},
        'stereo': {'balance': t(0.0), 'mid_gain': t(1.0), 'side_gain': t(1.0), 'sm_swap': t(0.0)},
        'limiter': {'threshold_db': t(P['th_db']), 'ceiling_lin': t(0.95),
                    'attack_ms': t(P['at']), 'release_ms': t(P['rt']),
                    'input_db': t(P['in_db']), 'output_db': t(P['out_db'])},
    }
    with torch.no_grad():
        y = chain_s(xt, params=params)
    return y[0, 0].numpy()

# ---- Vraie chaîne LV2 ----
LV2_URIS = ['http://lsp-plug.in/plugins/lv2/para_equalizer_x16_stereo',
            'http://calf.sourceforge.net/plugins/Exciter',
            'http://lsp-plug.in/plugins/lv2/limiter_stereo']
chain_r = RealLV2Chain(sr=SR, block=BLOCK)
for uri in LV2_URIS: chain_r.add(uri)
for b in range(16):
    chain_r.set_param(0, f'ft_{b}', 1.0)
    chain_r.set_param(0, f'f_{b}', float(FIXED_EQ_FREQS_V5_14[b]))
    chain_r.set_param(0, f'q_{b}', float(FIXED_EQ_QS_V5_14[b]))

def run_lv2(P):
    for b in range(16):
        chain_r.set_param(0, f'g_{b}', float(10.0 ** (P['eq_gains'][b] / 20.0)))
    chain_r.set_param(1, 'amount', float(P['amount']))
    chain_r.set_param(1, 'drive',  float(P['drive']))
    chain_r.set_param(1, 'freq',   float(P['exc_freq']))
    chain_r.set_param(1, 'ceil',   float(P['exc_ceil']))
    chain_r.set_param(2, 'th',    float(10.0 ** (P['th_db'] / 20.0)))
    chain_r.set_param(2, 'g_in',  float(10.0 ** (P['in_db'] / 20.0)))
    chain_r.set_param(2, 'g_out', float(10.0 ** (P['out_db'] / 20.0)))
    chain_r.set_param(2, 'at',    float(P['at']))
    chain_r.set_param(2, 'rt',    float(P['rt']))
    out = np.zeros(N, dtype=np.float32)
    for blk in range(N // BLOCK):
        s = blk * BLOCK
        l, r = chain_r.process(x[s:s+BLOCK], x[s:s+BLOCK])
        out[s:s+BLOCK] = 0.5 * (l + r)
    return out

def bands_db(sig):
    st = torch.from_numpy(np.stack([sig, sig])[None]).float()
    return rms_db_per_band_stereo(st, SR)[0].numpy()

raw_b = bands_db(x)
print(f'Audio test : {pair.slug[:40]}, 5 s @ 60 s')
for nm, P in [('P1 (params modèle)', P1), ('P2 (air agressif)', P2), ('P3 (neutre)', P3)]:
    y_s = run_surrogate(P)
    y_r = run_lv2(P)
    sb = bands_db(y_s) - raw_b      # delta vs raw
    rb = bands_db(y_r) - raw_b
    print(f'\n=== {nm} ===')
    print(f'{"bande":>6s} {"lo-hi Hz":>15s} {"Δsurrogate":>11s} {"ΔLV2":>8s} {"surr-LV2":>9s}')
    for i in [0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 19]:
        lo, hi = BANDS_HZ[i]
        print(f'{i:>6d} {f"{lo:.0f}-{hi:.0f}":>15s} {sb[i]:>+11.2f} {rb[i]:>+8.2f} {sb[i]-rb[i]:>+9.2f}')
    # Résumé zones
    mid_s, mid_r = sb[6:9].mean(), rb[6:9].mean()
    air_s, air_r = sb[15:20].mean(), rb[15:20].mean()
    print(f'  → mid (1.4-5.3k) : surr {mid_s:+.1f} dB | LV2 {mid_r:+.1f} dB')
    print(f'  → air (13-20k)   : surr {air_s:+.1f} dB | LV2 {air_r:+.1f} dB | ÉCART {air_s-air_r:+.1f} dB')
