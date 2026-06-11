#!/usr/bin/env python3
"""V5.19 — éval écoute : enveloppe spectrale via FIR 256 streaming + LV2 (exciter+limiter).

Pipeline FIDÈLE au board :
  - mono, fenêtre 100 ms hop 10 ms, prédiction 74 params à 100 Hz
  - enveloppe 64 pts → FIR 256 taps phase linéaire RECONSTRUITE à chaque bloc
    (10 ms), appliquée en overlap-save (état continu, pas de clics)
  - exciter + limiter : chaîne LV2 réelle, set_param à 100 Hz
  - PAS d'EQ paramétrique (remplacé par l'enveloppe)

Sorties : eval_v5_19_ep<N>/<slug>_{raw,model,target}.wav
"""
import sys, numpy as np, torch, soundfile as sf
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from model import (MasteringXXL_conv2d, denormalize_params_v5_19,
                   N_PARAMS_OUT_V5_19)
from features_v3 import compute_features_v3, N_FEATURES_V3
from surrogate_spectral_env import fir_from_env, FIR_TAPS
from dataset_loader import iter_pairs, load_audio
from lv2_chain import Chain as RealLV2Chain

EPOCH = int(sys.argv[1]) if len(sys.argv) > 1 else 5

WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT      = WORKSPACE / 'checkpoints' / f'conv_v5_19_full_epoch{EPOCH:03d}.pt'
OUT_DIR   = WORKSPACE / f'eval_v5_19_ep{EPOCH}'
OUT_DIR.mkdir(parents=True, exist_ok=True)

SR        = 48000
BLOCK     = 480
N_FRAMES  = 10
DEVICE    = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

SLUGS = [
    'cambridge-bigmeansoundmachine-contraband',
    'musdb-music-delta-rock',
    'dsd-bks-bulldozer',
    'musdb-aimee-norwich-child',
    'cambridge-nickibluhmandthegramblers',
]
LV2_URIS = [
    'http://calf.sourceforge.net/plugins/Exciter',
    'http://lsp-plug.in/plugins/lv2/limiter_stereo',
]

def rms_db(x): return 20*np.log10(np.sqrt((x**2).mean())+1e-12)
def crest_db(x): return 20*np.log10((np.abs(x).max()+1e-12)/(np.sqrt((x**2).mean())+1e-12))


def apply_dyn_params(chain, P, i):
    ex = P['exciter']
    chain.set_param(0, 'amount', float(ex['amount'][i]))
    chain.set_param(0, 'drive',  float(ex['drive'][i]))
    chain.set_param(0, 'freq',   float(ex['freq_hz'][i]))
    chain.set_param(0, 'ceil',   float(ex['ceiling'][i]))
    lm = P['limiter']
    chain.set_param(1, 'th',    float(10.0 ** (lm['threshold_db'][i] / 20.0)))
    chain.set_param(1, 'g_in',  float(10.0 ** (lm['input_db'][i] / 20.0)))
    chain.set_param(1, 'g_out', float(10.0 ** (lm['output_db'][i] / 20.0)))
    chain.set_param(1, 'at',    float(lm['attack_ms'][i]))
    chain.set_param(1, 'rt',    float(lm['release_ms'][i]))


def main():
    print(f'Loading model {CKPT.name}...')
    model = MasteringXXL_conv2d(n_input=N_FEATURES_V3, n_out=N_PARAMS_OUT_V5_19)
    ckpt = torch.load(str(CKPT), map_location=DEVICE, weights_only=False)
    model.load_state_dict(ckpt['model'])
    model.to(DEVICE).eval()

    wanted = {s: None for s in SLUGS}
    for pair in iter_pairs():
        for s in SLUGS:
            if s in pair.slug and wanted[s] is None:
                wanted[s] = pair
    pairs = [p for p in wanted.values() if p is not None]
    print(f'{len(pairs)} morceaux')

    chain = RealLV2Chain(sr=SR, block=BLOCK)
    for uri in LV2_URIS:
        chain.add(uri)

    for pair in pairs:
        slug = pair.slug[:40]
        print(f'\n=== {slug} ===', flush=True)
        raw_full, _ = load_audio(pair.raw_path, target_sr=SR)
        tgt_full, _ = load_audio(pair.master_path, target_sr=SR)
        n = min(len(raw_full), len(tgt_full))
        raw = 0.5 * (raw_full[:n, 0] + raw_full[:n, 1])
        tgt = 0.5 * (tgt_full[:n, 0] + tgt_full[:n, 1])
        n_blocks = len(raw) // BLOCK

        print('  features...', end=' ', flush=True)
        feats = compute_features_v3(raw.astype(np.float32), sr=SR)
        n_tr = feats.shape[0]

        print('predict...', end=' ', flush=True)
        windows = []
        for t in range(n_tr):
            lo = max(0, t - N_FRAMES + 1)
            w = feats[lo:t+1]
            if w.shape[0] < N_FRAMES:
                w = np.concatenate([np.repeat(w[:1], N_FRAMES - w.shape[0], axis=0), w])
            windows.append(w.T)
        windows = torch.from_numpy(np.stack(windows)).to(DEVICE)
        preds = []
        with torch.no_grad():
            for k in range(0, len(windows), 512):
                preds.append(model(windows[k:k+512]).cpu())
        preds = torch.cat(preds)
        P = denormalize_params_v5_19(preds)            # batch dicts
        envs = P['env']['gains_db'].numpy()            # (n_tr, 64)

        print('fir+lv2 stream...', flush=True)
        x = raw.astype(np.float32)
        out = np.zeros(n_blocks * BLOCK, dtype=np.float32)
        tail = np.zeros(FIR_TAPS - 1, dtype=np.float32)   # overlap-save state
        for blk in range(n_blocks):
            t = min(blk, n_tr - 1)
            h = fir_from_env(envs[t], SR)
            seg = np.concatenate([tail, x[blk*BLOCK:(blk+1)*BLOCK]])
            y = np.convolve(seg, h, mode='valid')          # (BLOCK,) après state
            # np.convolve(valid) sur (tail+BLOCK) donne BLOCK échantillons
            apply_dyn_params(chain, P, t)
            l, r = chain.process(y.astype(np.float32), y.astype(np.float32))
            out[blk*BLOCK:(blk+1)*BLOCK] = 0.5 * (l + r)
            tail = seg[-(FIR_TAPS - 1):]

        base = OUT_DIR / slug
        sf.write(str(base) + '_raw.wav',    np.stack([raw[:len(out)]]*2, 1), SR)
        sf.write(str(base) + '_model.wav',  np.stack([out]*2, 1), SR)
        sf.write(str(base) + '_target.wav', np.stack([tgt[:len(out)]]*2, 1), SR)
        print(f'  raw    : rms {rms_db(raw):+7.2f}  crest {crest_db(raw):5.2f}')
        print(f'  model  : rms {rms_db(out):+7.2f}  crest {crest_db(out):5.2f}')
        print(f'  target : rms {rms_db(tgt):+7.2f}  crest {crest_db(tgt):5.2f}')
        print(f'  Δrms {rms_db(out)-rms_db(tgt):+.2f} dB | Δcrest {crest_db(out)-crest_db(tgt):+.2f} dB')

    print(f'\nWAVs : {OUT_DIR}')


if __name__ == '__main__':
    main()
