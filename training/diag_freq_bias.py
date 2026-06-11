#!/usr/bin/env python3
"""Diagnostic des biais fréquence-dépendants dans encoder + loss (hypothèse utilisateur).

T1 : bruit des features mel par bande sur signal STATIONNAIRE (bruit rose)
     → si la feature varie sur un signal constant, c'est du bruit de mesure.
T2 : stabilité énergie par trame 10 ms : basses vs aigus sur musique réelle.
T3 : sensibilité de la loss (Pearson + L_band) à une erreur de +5 dB selon la bande.
T4 : fiabilité de rms_db_per_band (loss) sur chunks 100 ms : std entre chunks adjacents.
T5 : clamp ±20 dB : combien de deltas demandés par bande sont clampés ?
T6 : erreur par bande du modèle final (où se concentre l'erreur réelle).
"""
import sys, numpy as np, torch
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from features_v2 import compute_features_v2, N_MELS, _MEL_FB, FFT_SIZE
from train_v5_17 import rms_db_per_band_stereo, pearson_corr_loss, BANDS_HZ, BAND_WEIGHTS_NP, SR

rng = np.random.default_rng(0)

print('='*72)
print('T1 : bruit des features Mel par bande — bruit rose STATIONNAIRE 10 s')
print('='*72)
# Bruit rose : énergie constante par octave, signal statistiquement stationnaire
N = SR * 10
white = rng.standard_normal(N + 1)
# pink filter (Voss approx via FFT 1/f)
spec = np.fft.rfft(white)
freqs = np.fft.rfftfreq(len(white), 1/SR)
spec[1:] /= np.sqrt(freqs[1:])
pink = np.fft.irfft(spec)[:N].astype(np.float32)
pink /= np.abs(pink).max()

feats = compute_features_v2(pink, sr=SR)     # (n_trames, 85)
mel = feats[:, :N_MELS]                       # log10 mel
# Signal stationnaire → la feature DEVRAIT être constante. std = bruit de mesure.
band_centers = np.geomspace(20, 20000, N_MELS+1)
band_centers = np.sqrt(band_centers[:-1] * band_centers[1:])
print(f'{"mel band":>10s} {"center Hz":>10s} {"std (dB)":>10s}   (signal stationnaire : std = bruit de mesure)')
sel_bands = [0, 2, 4, 8, 16, 24, 32, 40, 48, 56, 63]
for b in sel_bands:
    std_db = 10 * mel[:, b].std()    # log10 → dB : ×10
    print(f'{b:>10d} {band_centers[b]:>10.1f} {std_db:>10.2f}')

print()
print('='*72)
print('T2 : énergie par trame 10 ms — stabilité basses vs aigus (musique réelle)')
print('='*72)
from dataset_loader import iter_pairs, load_audio
pair = next(iter_pairs())
raw_full, _ = load_audio(pair.raw_path, target_sr=SR)
x = 0.5*(raw_full[:SR*30, 0] + raw_full[:SR*30, 1]).astype(np.float32)   # 30 s
feats_m = compute_features_v2(x, sr=SR)
mel_m = feats_m[:, :N_MELS]
print(f'{"mel band":>10s} {"center Hz":>10s} {"std (dB)":>10s}   (musique : mélange contenu + bruit mesure)')
for b in sel_bands:
    print(f'{b:>10d} {band_centers[b]:>10.1f} {10*mel_m[:, b].std():>10.2f}')

print()
print('='*72)
print('T3 : sensibilité de la loss à +5 dB d\'erreur selon la zone')
print('='*72)
# Construire un profil de bandes réaliste (20 bandes loss) depuis la musique
xt = torch.from_numpy(np.stack([x[:SR*5], x[:SR*5]])[None])    # (1, 2, N)
base_b = rms_db_per_band_stereo(xt, SR)                         # (1, 20)
print('Profil dB par bande (musique) :', np.round(base_b[0].numpy(), 1))
# Cas A : +5 dB d'erreur sur bandes 8-11 (mediums ~1-4 kHz)
# Cas B : +5 dB d'erreur sur bandes 16-19 (air 13-20 kHz)
for nm, idx in [('mediums (b8-11)', range(8, 12)), ('air (b16-19)', range(16, 20))]:
    err_b = base_b.clone()
    for i in idx:
        err_b[0, i] += 5.0
    one_minus_r = pearson_corr_loss(err_b, base_b).item()
    w = torch.tensor(BAND_WEIGHTS_NP)
    l_band = (((err_b - base_b) ** 2) * w).mean().item()
    print(f'  +5 dB sur {nm:<18s} : (1-r) = {one_minus_r:.5f}   L_band brut = {l_band:.2f} (avant /100 → {l_band/100:.3f})')

print()
print('='*72)
print('T4 : fiabilité de la mesure loss par bande sur chunks 100 ms adjacents')
print('='*72)
# Chunks adjacents d'un passage stable : si la mesure était fiable, std faible.
seg = x[SR*10:SR*12]   # 2 s
chunks = [seg[i*4800:(i+1)*4800] for i in range(20)]
bvals = []
for c in chunks:
    ct = torch.from_numpy(np.stack([c, c])[None])
    bvals.append(rms_db_per_band_stereo(ct, SR)[0].numpy())
bvals = np.stack(bvals)    # (20 chunks, 20 bandes)
print(f'{"bande":>6s} {"lo-hi Hz":>16s} {"std inter-chunk dB":>20s}')
for i in [0, 1, 2, 3, 5, 7, 9, 11, 14, 17, 19]:
    lo, hi = BANDS_HZ[i]
    print(f'{i:>6d} {f"{lo:.0f}-{hi:.0f}":>16s} {bvals[:, i].std():>20.2f}')

print()
print('='*72)
print('T5 : clamp ±20 dB — deltas demandés par bande (200 chunks réels)')
print('='*72)
CACHE = '/home/michael/data/mastering_workspace/cache/pair_cache_v5_16_mel_100ms.npz'
data = np.load(CACHE, allow_pickle=True)
pairs = data['pairs']
sel = rng.permutation(len(pairs))[:200]
deltas = []
for idx in sel:
    p = pairs[idx]
    rms = np.sqrt((p['raw'] ** 2).mean()) + 1e-12
    if 20*np.log10(rms) < -35: continue
    rt = torch.from_numpy(p['raw'][None])
    tgt_mid = 0.5*(p['target'][0] + p['target'][1])
    tt = torch.from_numpy(np.stack([tgt_mid, tgt_mid])[None])
    rb = rms_db_per_band_stereo(rt.float(), SR)[0].numpy()
    tb = rms_db_per_band_stereo(tt.float(), SR)[0].numpy()
    deltas.append(tb - rb)
deltas = np.stack(deltas)
print(f'{"bande":>6s} {"lo-hi Hz":>16s} {"delta demandé mean":>18s} {"p95":>8s} {"% clampé >20dB":>15s}')
for i in [0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 19]:
    lo, hi = BANDS_HZ[i]
    d = deltas[:, i]
    pct_clamp = 100.0 * (np.abs(d) > 20).mean()
    print(f'{i:>6d} {f"{lo:.0f}-{hi:.0f}":>16s} {d.mean():>+18.1f} {np.percentile(d, 95):>+8.1f} {pct_clamp:>14.1f}%')

print()
print('='*72)
print('T6 : erreur par bande du MODÈLE FINAL (sortie chaîne vs target, 64 chunks)')
print('='*72)
from model import MasteringXXL_conv2d, denormalize_params_v5_17, N_FEATURES_IN_V3, N_PARAMS_OUT_V5_17
from surrogate_chain import MasteringChainSurrogate
m = MasteringXXL_conv2d(n_input=N_FEATURES_IN_V3, n_out=N_PARAMS_OUT_V5_17)
ck = torch.load('/home/michael/data/mastering_workspace/checkpoints/conv_v5_17_full_epoch029.pt',
                map_location='cpu', weights_only=False)
m.load_state_dict(ck['model']); m.eval()
chain = MasteringChainSurrogate(sr=SR).eval()
for p in chain.parameters(): p.requires_grad_(False)
sub = []
for idx in sel:
    p = pairs[idx]
    rms = np.sqrt((p['raw'] ** 2).mean()) + 1e-12
    if 20*np.log10(rms) >= -35: sub.append(idx)
    if len(sub) == 64: break
raws  = torch.from_numpy(np.stack([pairs[i]['raw'] for i in sub])).float()
tgts  = []
for i in sub:
    tm = 0.5*(pairs[i]['target'][0] + pairs[i]['target'][1])
    tgts.append(np.stack([tm, tm]))
tgts  = torch.from_numpy(np.stack(tgts)).float()
fts   = torch.from_numpy(np.stack([pairs[i]['features'] for i in sub])).float().permute(0, 2, 1)
with torch.no_grad():
    pn = m(fts)
    params = denormalize_params_v5_17(pn)
    outs = chain(raws, params=params)
ob = rms_db_per_band_stereo(outs, SR).numpy()
tb = rms_db_per_band_stereo(tgts, SR).numpy()
err = ob - tb
print(f'{"bande":>6s} {"lo-hi Hz":>16s} {"err mean dB":>12s} {"|err| mean":>11s}')
for i in range(20):
    lo, hi = BANDS_HZ[i]
    print(f'{i:>6d} {f"{lo:.0f}-{hi:.0f}":>16s} {err[:, i].mean():>+12.2f} {np.abs(err[:, i]).mean():>11.2f}')
