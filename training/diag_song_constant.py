#!/usr/bin/env python3
"""Test song-constant : UN jeu de 26 params par MORCEAU (constant sur tous ses
chunks) optimisé directement. C'est généralisable par définition (= ce qu'un
ingé mastering fait : régler une fois pour le morceau).

Compare :
  - loss song-constant (borne généralisable statique)
  - loss modèle v5.18 (~1.19)
  - borne par-chunk (0.346, overfit au bruit)

8 morceaux × 40 chunks chacun. Les params d'un morceau sont PARTAGÉS par ses
40 chunks → l'optimum ne peut pas fitter le bruit chunk-par-chunk.
"""
import sys, numpy as np, torch
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from model import denormalize_params_v5_17, N_PARAMS_OUT_V5_17
from surrogate_chain import MasteringChainSurrogate
from train_v5_18 import loss_v5_12, SR

CACHE = '/home/michael/data/mastering_workspace/cache/pair_cache_v5_18_mel125_100ms.npz'
N_SONGS  = 8
N_CHUNKS_PER_SONG = 40
N_STEPS  = 300
BATCH    = 40        # tous les chunks d'un morceau par batch
DEVICE = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

print('Loading cache...')
data = np.load(CACHE, allow_pickle=True)
pairs = data['pairs']
rng = np.random.default_rng(21)

# Groupe par slug
from collections import defaultdict
by_slug = defaultdict(list)
for i, p in enumerate(pairs):
    rms = np.sqrt((p['raw'] ** 2).mean()) + 1e-12
    if 20 * np.log10(rms) >= -35.0:
        by_slug[p['slug']].append(i)

slugs = [s for s, l in by_slug.items() if len(l) >= N_CHUNKS_PER_SONG]
rng.shuffle(slugs)
slugs = slugs[:N_SONGS]
print(f'{len(slugs)} morceaux sélectionnés')

chain = MasteringChainSurrogate(sr=SR).to(DEVICE).eval()
for p in chain.parameters(): p.requires_grad_(False)

all_final = []
for slug in slugs:
    idxs = by_slug[slug][:N_CHUNKS_PER_SONG]
    raws = torch.from_numpy(np.stack([pairs[i]['raw'] for i in idxs])).float().to(DEVICE)
    tgts = []
    for i in idxs:
        tm = 0.5 * (pairs[i]['target'][0] + pairs[i]['target'][1])
        tgts.append(np.stack([tm, tm]))
    tgts = torch.from_numpy(np.stack(tgts)).float().to(DEVICE)

    # UN seul logit (26,) partagé par les 40 chunks du morceau
    logit = torch.zeros(N_PARAMS_OUT_V5_17, device=DEVICE, requires_grad=True)
    opt = torch.optim.Adam([logit], lr=0.05)
    final = None
    for step in range(N_STEPS):
        opt.zero_grad()
        pn = torch.sigmoid(logit).unsqueeze(0).expand(len(idxs), -1)
        params = denormalize_params_v5_17(pn)
        out = chain(raws, params=params)
        losses = loss_v5_12(raws, out, tgts, params_norm=None)   # pas de L_mc (params constants)
        losses['total'].backward()
        opt.step()
        final = losses
    all_final.append({k: v.item() for k, v in final.items() if k != 'total'} | {'total': final['total'].item()})
    print(f'  {slug[:42]:<42s} : loss song-constant = {final["total"].item():.4f}')

print()
mean_total = np.mean([f['total'] for f in all_final])
print(f'=== RÉSULTATS ===')
print(f'loss song-constant (moyenne {N_SONGS} morceaux) : {mean_total:.4f}')
print(f'loss modèle v5.18 (epoch 4)                    : ~1.19')
print(f'borne par-chunk (overfit bruit)                : 0.346')
print()
print('Composantes moyennes song-constant :')
for k in ['L_rms_n', 'L_band_n', 'L_corr_n', 'L_crest_n', 'L_compress_n', 'L_mel_n', 'L_tilt_n']:
    print(f'  {k:<14s} : {np.mean([f[k] for f in all_final]):.4f}')
