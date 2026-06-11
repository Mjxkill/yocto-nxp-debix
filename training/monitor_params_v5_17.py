#!/usr/bin/env python3
"""V5.17 — monitore les params prédits par le modèle sur les morceaux d'éval.

Pour chaque morceau : stats par param (mean/std/min/max sur toutes les trames),
courbe EQ moyenne, et spectre par bande raw vs model vs target (où est l'air ?).
"""
import sys, numpy as np, torch, soundfile as sf
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from model import (MasteringXXL_conv2d, denormalize_params_v5_17,
                   N_FEATURES_IN_V3, N_PARAMS_OUT_V5_17,
                   FIXED_EQ_FREQS_V5_14, PARAM_RANGES_V5_17, PARAM_LAYOUT_V5_17)
from features_v2 import compute_features_v2
from dataset_loader import iter_pairs, load_audio

WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT      = WORKSPACE / 'checkpoints' / 'conv_v5_17_full_epoch029.pt'
EVAL_DIR  = WORKSPACE / 'eval_v5_17'
SR        = 48000
N_FRAMES  = 10
DEVICE    = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

SLUGS = ['cambridge-bigmeansoundmachine-contraband', 'dsd-bks-bulldozer']

model = MasteringXXL_conv2d(n_input=N_FEATURES_IN_V3, n_out=N_PARAMS_OUT_V5_17)
ckpt = torch.load(str(CKPT), map_location=DEVICE, weights_only=False)
model.load_state_dict(ckpt['model'])
model.to(DEVICE).eval()

wanted = {s: None for s in SLUGS}
for pair in iter_pairs():
    for s in SLUGS:
        if s in pair.slug and wanted[s] is None:
            wanted[s] = pair

PNAMES = ([f'eq_{int(f)}Hz' for f in FIXED_EQ_FREQS_V5_14] +
          ['exc_amount', 'exc_drive', 'exc_freq', 'exc_ceil',
           'lim_th_db', 'lim_ceil', 'lim_at_ms', 'lim_rt_ms', 'lim_in_db', 'lim_out_db'])

def spectral_bands(x, sr=SR):
    """Énergie dB dans 6 zones (sub<100, bass<500, lowmid<2k, mid<5k, high<10k, air>10k)."""
    from scipy.signal import welch
    f, p = welch(x, fs=sr, nperseg=8192)
    zones = [(20,100,'sub'), (100,500,'bass'), (500,2000,'lowmid'),
             (2000,5000,'mid'), (5000,10000,'high'), (10000,20000,'air')]
    return {nm: 10*np.log10(p[(f>=lo)&(f<hi)].sum()+1e-15) for lo,hi,nm in zones}

for slug, pair in wanted.items():
    if pair is None: continue
    print(f'\n{"="*70}\n=== {slug}\n{"="*70}')
    raw_full, _ = load_audio(pair.raw_path, target_sr=SR)
    tgt_full, _ = load_audio(pair.master_path, target_sr=SR)
    n = min(len(raw_full), len(tgt_full))
    raw = 0.5*(raw_full[:n,0]+raw_full[:n,1])
    tgt = 0.5*(tgt_full[:n,0]+tgt_full[:n,1])

    feats = compute_features_v2(raw.astype(np.float32), sr=SR)
    n_tr = feats.shape[0]
    windows = []
    for t in range(0, n_tr, 10):     # 1 fenêtre sur 10 (toutes les 100 ms, suffit pour stats)
        lo = max(0, t - N_FRAMES + 1)
        w = feats[lo:t+1]
        if w.shape[0] < N_FRAMES:
            w = np.concatenate([np.repeat(w[:1], N_FRAMES - w.shape[0], axis=0), w])
        windows.append(w.T)
    windows = torch.from_numpy(np.stack(windows)).to(DEVICE)
    with torch.no_grad():
        preds = []
        for k in range(0, len(windows), 512):
            preds.append(model(windows[k:k+512]).cpu())
        preds = torch.cat(preds).numpy()    # (n_win, 26) normalisé [0,1]

    # Denormalise en valeurs réelles pour les stats
    real = np.zeros_like(preds)
    for i, key_idx in enumerate([('eq.gain_db', b) for b in range(16)] +
                                 [('exciter.amount',0),('exciter.drive',0),('exciter.freq_hz',0),('exciter.ceiling',0),
                                  ('limiter.threshold_db',0),('limiter.ceiling_lin',0),('limiter.attack_ms',0),
                                  ('limiter.release_ms',0),('limiter.input_db',0),('limiter.output_db',0)]):
        key = key_idx[0]
        mn, mx = PARAM_RANGES_V5_17[key]
        real[:, i] = preds[:, i] * (mx - mn) + mn

    print(f'\n{len(preds)} prédictions (1 / 100 ms). Params réels appliqués :')
    print(f'{"param":<14s} {"mean":>8s} {"std":>7s} {"min":>8s} {"max":>8s}   {"norm_mean":>9s}')
    for i, nm in enumerate(PNAMES):
        sat = ''
        if preds[:, i].mean() > 0.93: sat = ' ⚠SAT-HIGH'
        if preds[:, i].mean() < 0.07: sat = ' ⚠SAT-LOW'
        print(f'{nm:<14s} {real[:,i].mean():>8.2f} {real[:,i].std():>7.2f} {real[:,i].min():>8.2f} {real[:,i].max():>8.2f}   {preds[:,i].mean():>9.3f}{sat}')

    # Spectre 6 zones : raw vs model.wav vs target
    model_wav_path = EVAL_DIR / f'{pair.slug[:40]}_model.wav'
    if model_wav_path.exists():
        mw, _ = sf.read(str(model_wav_path))
        mw = mw[:, 0] if mw.ndim == 2 else mw
        nm = min(len(raw), len(mw), len(tgt))
        sp_raw = spectral_bands(raw[:nm]); sp_mod = spectral_bands(mw[:nm]); sp_tgt = spectral_bands(tgt[:nm])
        print(f'\nSpectre par zone (dB) :')
        print(f'{"zone":<8s} {"raw":>8s} {"model":>8s} {"target":>8s}   {"mod-raw":>8s} {"mod-tgt":>8s}')
        for z in ['sub','bass','lowmid','mid','high','air']:
            print(f'{z:<8s} {sp_raw[z]:>8.1f} {sp_mod[z]:>8.1f} {sp_tgt[z]:>8.1f}   {sp_mod[z]-sp_raw[z]:>+8.1f} {sp_mod[z]-sp_tgt[z]:>+8.1f}')
