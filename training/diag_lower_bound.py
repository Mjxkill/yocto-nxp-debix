#!/usr/bin/env python3
"""Borne inférieure de la loss atteignable par la chaîne (optimisation directe).

Pour 20 chunks : descente de gradient directement sur les 26 params normalisés
(PAS de modèle) pour minimiser la loss v5.18 de CE chunk. 300 steps Adam.

Compare : loss_optim_directe vs loss du modèle v5.18 (courant) vs loss params neutres.
"""
import sys, numpy as np, torch
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from model import denormalize_params_v5_17, N_PARAMS_OUT_V5_17
from surrogate_chain import MasteringChainSurrogate
from train_v5_18 import loss_v5_12, SR

CACHE = '/home/michael/data/mastering_workspace/cache/pair_cache_v5_18_mel125_100ms.npz'
N_CHUNKS = 20
N_STEPS  = 300
DEVICE = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

print('Loading cache...')
data = np.load(CACHE, allow_pickle=True)
pairs = data['pairs']
rng = np.random.default_rng(11)

sel = []
for idx in rng.permutation(len(pairs)):
    p = pairs[idx]
    rms = np.sqrt((p['raw'] ** 2).mean()) + 1e-12
    if 20 * np.log10(rms) >= -35.0:
        sel.append(idx)
    if len(sel) == N_CHUNKS:
        break

chain = MasteringChainSurrogate(sr=SR).to(DEVICE).eval()
for p in chain.parameters(): p.requires_grad_(False)

raws = torch.from_numpy(np.stack([pairs[i]['raw'] for i in sel])).float().to(DEVICE)
tgts = []
for i in sel:
    tm = 0.5 * (pairs[i]['target'][0] + pairs[i]['target'][1])
    tgts.append(np.stack([tm, tm]))
tgts = torch.from_numpy(np.stack(tgts)).float().to(DEVICE)

# Params libres par chunk (logit pour rester dans [0,1] via sigmoid)
logits = torch.zeros(N_CHUNKS, N_PARAMS_OUT_V5_17, device=DEVICE, requires_grad=True)
opt = torch.optim.Adam([logits], lr=0.05)

print(f'Optimisation directe {N_CHUNKS} chunks × {N_STEPS} steps...')
loss_first = None
for step in range(N_STEPS):
    opt.zero_grad()
    pn = torch.sigmoid(logits)
    params = denormalize_params_v5_17(pn)
    out = chain(raws, params=params)
    losses = loss_v5_12(raws, out, tgts, params_norm=pn)
    losses['total'].backward()
    opt.step()
    if step == 0:
        loss_first = losses['total'].item()
    if step % 50 == 0 or step == N_STEPS - 1:
        print(f'  step {step:>3d} : loss {losses["total"].item():.4f}')

print()
print(f'loss params neutres (step 0, sigmoid(0)=0.5) : {loss_first:.4f}')
print(f'loss BORNE INFÉRIEURE (optim directe)        : {losses["total"].item():.4f}')
print(f'loss modèle v5.18 (epoch courant)            : ~1.19')
print()
print('Détail composantes à la borne :')
for k in ['L_rms_n', 'L_band_n', 'L_corr_n', 'L_crest_n', 'L_compress_n', 'L_mel_n', 'L_tilt_n']:
    print(f'  {k:<14s} : {losses[k].item():.4f}')
