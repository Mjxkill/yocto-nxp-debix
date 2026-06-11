#!/usr/bin/env python3
"""V5.17 — éval écoute PC : 5 morceaux masterisés par le modèle (streaming 100 Hz).

Pipeline FIDÈLE au board :
  - mono (mid), fenêtre features 100 ms glissante, hop 10 ms
  - modèle XXL_conv2d 26 params, prédiction toutes les 10 ms
  - chaîne LV2 RÉELLE (LSP EQ x16 + Calf Exciter + LSP Limiter),
    set_param toutes les 10 ms, état des filtres conservé (streaming)
  - StereoTools absent (modèle mono) — chaîne 3 plugins

Sorties : /home/michael/data/mastering_workspace/eval_v5_17/
  <slug>_raw.wav     extrait brut (60 s)
  <slug>_model.wav   masterisé par le modèle
  <slug>_target.wav  master humain (référence, mono mid)
"""
import sys, numpy as np, torch, soundfile as sf
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from model import (MasteringXXL_conv2d, denormalize_params_v5_17,
                   N_FEATURES_IN_V3, N_PARAMS_OUT_V5_17)
from features_v2 import compute_features_v2, N_FEATURES_V2, FRAME_SIZE, HOP_SIZE
from dataset_loader import iter_pairs, load_audio
from lv2_chain import Chain as RealLV2Chain

WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT      = WORKSPACE / 'checkpoints' / 'conv_v5_17_full_epoch029.pt'
OUT_DIR   = WORKSPACE / 'eval_v5_17'
OUT_DIR.mkdir(parents=True, exist_ok=True)

SR        = 48000
BLOCK     = 480            # 10 ms = hop du push params
N_FRAMES  = 10             # fenêtre modèle = 10 trames de 10 ms = 100 ms
SEG_START = 0.0            # morceau complet
SEG_DUR   = 100000.0       # morceau complet (cap à la durée réelle)
DEVICE    = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

SLUGS = [
    'cambridge-bigmeansoundmachine-contraband',
    'musdb-music-delta-rock',
    'dsd-bks-bulldozer',
    'musdb-aimee-norwich-child',
    'cambridge-nickibluhmandthegramblers',
]

LV2_URIS = [
    'http://lsp-plug.in/plugins/lv2/para_equalizer_x16_stereo',
    'http://calf.sourceforge.net/plugins/Exciter',
    'http://lsp-plug.in/plugins/lv2/limiter_stereo',
]


def rms_db(x): return 20*np.log10(np.sqrt((x**2).mean())+1e-12)
def crest_db(x): return 20*np.log10((np.abs(x).max()+1e-12)/(np.sqrt((x**2).mean())+1e-12))


def apply_params(chain, P):
    """P = dict denormalize_params_v5_17 (squeezed, no batch). Slots :
    0 = LSP EQ x16, 1 = Calf Exciter, 2 = LSP Limiter."""
    eq = P['eq']
    for b in range(16):
        chain.set_param(0, f'g_{b}', float(10.0 ** (eq['gain_db'][b].item() / 20.0)))
    ex = P['exciter']
    chain.set_param(1, 'amount', float(ex['amount'].item()))
    chain.set_param(1, 'drive',  float(ex['drive'].item()))
    chain.set_param(1, 'freq',   float(ex['freq_hz'].item()))
    chain.set_param(1, 'ceil',   float(ex['ceiling'].item()))
    lm = P['limiter']
    chain.set_param(2, 'th',    float(10.0 ** (lm['threshold_db'].item() / 20.0)))
    chain.set_param(2, 'g_in',  float(10.0 ** (lm['input_db'].item() / 20.0)))
    chain.set_param(2, 'g_out', float(10.0 ** (lm['output_db'].item() / 20.0)))
    chain.set_param(2, 'at',    float(lm['attack_ms'].item()))
    chain.set_param(2, 'rt',    float(lm['release_ms'].item()))


def main():
    print(f'Loading model {CKPT.name}...')
    model = MasteringXXL_conv2d(n_input=N_FEATURES_IN_V3, n_out=N_PARAMS_OUT_V5_17)
    ckpt = torch.load(str(CKPT), map_location=DEVICE, weights_only=False)
    model.load_state_dict(ckpt['model'])
    model.to(DEVICE).eval()

    wanted = {s: None for s in SLUGS}
    for pair in iter_pairs():
        for s in SLUGS:
            if s in pair.slug and wanted[s] is None:
                wanted[s] = pair
    pairs = [p for p in wanted.values() if p is not None]
    print(f'{len(pairs)} morceaux trouvés')

    # Chaîne LV2 créée UNE FOIS (le binding ne supporte pas la re-création :
    # double free sur la 2e instance). Le state des filtres entre morceaux
    # porte un transitoire de quelques ms, négligeable.
    chain = RealLV2Chain(sr=SR, block=BLOCK)
    for uri in LV2_URIS:
        chain.add(uri)
    from model import FIXED_EQ_FREQS_V5_14, FIXED_EQ_QS_V5_14
    for b in range(16):
        chain.set_param(0, f'ft_{b}', 1.0)
        chain.set_param(0, f'f_{b}', float(FIXED_EQ_FREQS_V5_14[b]))
        chain.set_param(0, f'q_{b}', float(FIXED_EQ_QS_V5_14[b]))

    for pair in pairs:
        slug = pair.slug[:40]
        print(f'\n=== {slug} ===')
        raw_full, _ = load_audio(pair.raw_path, target_sr=SR)      # (N, 2)
        tgt_full, _ = load_audio(pair.master_path, target_sr=SR)
        i0 = int(SEG_START * SR)
        i1 = i0 + int(SEG_DUR * SR)
        if i1 > min(len(raw_full), len(tgt_full)):
            i0 = 0
            i1 = min(len(raw_full), len(tgt_full), int(SEG_DUR * SR))
        raw = 0.5 * (raw_full[i0:i1, 0] + raw_full[i0:i1, 1])      # mono mid
        tgt = 0.5 * (tgt_full[i0:i1, 0] + tgt_full[i0:i1, 1])
        N = len(raw)
        n_blocks = N // BLOCK

        # 1) Features par trame de 10 ms (précalcul, 1 FFT par trame)
        print('  features...', end=' ', flush=True)
        feats = compute_features_v2(raw.astype(np.float32), sr=SR)  # (n_trames, 85)
        n_tr = feats.shape[0]

        # 2) Prédictions batch : fenêtre = 10 trames finissant à la trame t
        print('predict...', end=' ', flush=True)
        windows = []
        for t in range(n_tr):
            lo = max(0, t - N_FRAMES + 1)
            w = feats[lo:t+1]
            if w.shape[0] < N_FRAMES:                      # pad début
                w = np.concatenate([np.repeat(w[:1], N_FRAMES - w.shape[0], axis=0), w])
            windows.append(w.T)                            # (85, 10)
        windows = torch.from_numpy(np.stack(windows)).to(DEVICE)   # (n_tr, 85, 10)
        preds = []
        with torch.no_grad():
            for k in range(0, len(windows), 512):
                preds.append(model(windows[k:k+512]).cpu())
        preds = torch.cat(preds)                                    # (n_tr, 26)

        # 3) Streaming chaîne LV2 réelle : set_param + process par bloc 10 ms
        print('lv2 stream...', flush=True)
        out = np.zeros(n_blocks * BLOCK, dtype=np.float32)
        x = raw.astype(np.float32)
        for blk in range(n_blocks):
            # params depuis la prédiction de la trame qui vient de finir
            t = min(blk, len(preds) - 1)
            P = denormalize_params_v5_17(preds[t])
            apply_params(chain, P)
            s = blk * BLOCK
            l, r = chain.process(x[s:s+BLOCK], x[s:s+BLOCK])
            out[s:s+BLOCK] = 0.5 * (l + r)

        # 4) Sauvegarde + stats
        base = OUT_DIR / slug
        sf.write(str(base) + '_raw.wav',    np.stack([raw[:len(out)]]*2, 1), SR)
        sf.write(str(base) + '_model.wav',  np.stack([out]*2, 1), SR)
        sf.write(str(base) + '_target.wav', np.stack([tgt[:len(out)]]*2, 1), SR)
        print(f'  raw    : rms {rms_db(raw):+7.2f} dB  crest {crest_db(raw):5.2f} dB')
        print(f'  model  : rms {rms_db(out):+7.2f} dB  crest {crest_db(out):5.2f} dB')
        print(f'  target : rms {rms_db(tgt):+7.2f} dB  crest {crest_db(tgt):5.2f} dB')
        print(f'  Δrms model-target : {rms_db(out)-rms_db(tgt):+.2f} dB | Δcrest : {crest_db(out)-crest_db(tgt):+.2f} dB')

    print(f'\nWAVs dans : {OUT_DIR}')


if __name__ == '__main__':
    main()
