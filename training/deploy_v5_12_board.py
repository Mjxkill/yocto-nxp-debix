#!/usr/bin/env python3
"""V9.5.12 Step A POC : push params v5.12 (calculés sur PC) vers mixer-pro board.

Pipeline :
  1. Load conv_v5_12_full_epoch029.pt
  2. Load wav raw (Cambridge contraband)
  3. Calcule features → predit params → force drive=6 + balance=0
  4. SSH socat -> /run/mixer-pro.sock pour configurer l'insert chain + push params

Pas de NPU, pas d'inférence live — juste un set statique pour valider la chain
LV2 sur la carte.

Usage :
    python3 deploy_v5_12_board.py [--ckpt v5_12_full_epoch029] [--wav <path>] [--host 192.168.0.9]
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).parent))

from model import MasteringConv1D, denormalize_params, N_FEATURES_IN_V2
from features import compute_features_mid_only
from dataset_loader import load_audio


WORKSPACE = Path('/home/michael/data/mastering_workspace')
CKPT_DIR  = WORKSPACE / 'checkpoints'
SR        = 48000

CHAIN_URIS = [
    'http://lsp-plug.in/plugins/lv2/para_equalizer_x16_stereo',
    'http://calf.sourceforge.net/plugins/Exciter',
    'http://calf.sourceforge.net/plugins/StereoTools',
    'http://lsp-plug.in/plugins/lv2/limiter_stereo',
]


def predict_params_mean(ckpt_tag: str, wav_path: Path,
                        force_drive: float = 6.0, force_balance: float = 0.0):
    """Load model + wav, calcule params moyens sur le wav entier."""
    ckpt_path = CKPT_DIR / f"conv_{ckpt_tag}.pt"
    print(f"Loading {ckpt_path.name}...")
    ckpt = torch.load(str(ckpt_path), map_location='cpu', weights_only=True)
    model = MasteringConv1D(n_input=N_FEATURES_IN_V2)
    model.load_state_dict(ckpt['model'])
    model.eval()

    print(f"Loading wav {wav_path.name}...")
    audio, _ = load_audio(str(wav_path), target_sr=SR)
    print(f"  audio shape: {audio.shape}, sr={SR}")

    # Features sur la totalité
    feats = compute_features_mid_only(audio, sr=SR)         # (N_frames, 11)
    feats_t = torch.from_numpy(feats).permute(1, 0).unsqueeze(0).float()
    print(f"  features shape: {feats_t.shape}")

    with torch.no_grad():
        params_norm = model(feats_t)[0]                     # (62,)
    params = denormalize_params(params_norm)

    # Force exciter drive max + balance neutre (validés en écoute PC)
    params['exciter']['drive'] = torch.tensor(float(force_drive))
    params['stereo']['balance'] = torch.tensor(float(force_balance))

    print()
    print("Params prédits (drive forcé, balance forcée) :")
    print(f"  Exciter   : amount={params['exciter']['amount'].item():.3f}  drive={params['exciter']['drive'].item():.3f}  freq={params['exciter']['freq_hz'].item():.0f} Hz")
    print(f"  Stereo    : bal={params['stereo']['balance'].item():+.3f}  mid={params['stereo']['mid_gain'].item():.3f}  side={params['stereo']['side_gain'].item():.3f}")
    print(f"  Limiter   : th={params['limiter']['threshold_db'].item():.1f}dB  in={params['limiter']['input_db'].item():.1f}dB  at={params['limiter']['attack_ms'].item():.1f}ms")
    print(f"  EQ        : 16 bands ; gain range [{params['eq']['gain_db'].min().item():+.1f}, {params['eq']['gain_db'].max().item():+.1f}] dB")
    return params


def build_bulk_payload(params) -> str:
    """Construit le JSON {"op":"set_insert_params_bulk","params":[[slot,name,val],...]}.

    Mapping slot/name identique à apply_params_to_real_lv2 (eval_v4.py).
    """
    items = []
    # Slot 0 : Para EQ x16
    eq = params['eq']
    for b in range(16):
        items.append([0, f"ft_{b}", 1.0])                          # 1 = Bell
        items.append([0, f"f_{b}",  float(eq['freq'][b].item())])
        g_lin = float(10.0 ** (eq['gain_db'][b].item() / 20.0))
        items.append([0, f"g_{b}",  g_lin])
        items.append([0, f"q_{b}",  float(eq['q'][b].item())])
    # Slot 1 : Calf Exciter
    ex = params['exciter']
    items.append([1, "amount", float(ex['amount'].item())])
    items.append([1, "drive",  float(ex['drive'].item())])
    items.append([1, "freq",   float(ex['freq_hz'].item())])
    items.append([1, "ceil",   float(ex['ceiling'].item())])
    # Slot 2 : Calf StereoTools
    st = params['stereo']
    items.append([2, "balance_in", float(st['balance'].item())])
    items.append([2, "mlev",       float(st['mid_gain'].item())])
    items.append([2, "slev",       float(st['side_gain'].item())])
    # Slot 3 : LSP Limiter Stereo
    lm = params['limiter']
    items.append([3, "th",    float(10.0 ** (lm['threshold_db'].item() / 20.0))])
    items.append([3, "g_in",  float(10.0 ** (lm['input_db'].item()    / 20.0))])
    items.append([3, "g_out", float(10.0 ** (lm['output_db'].item()   / 20.0))])
    items.append([3, "at",    float(lm['attack_ms'].item())])
    items.append([3, "rt",    float(lm['release_ms'].item())])

    return json.dumps({"op": "set_insert_params_bulk", "params": items})


def ssh_send(host: str, payload: str) -> str:
    """Envoie payload via SSH → socat → /run/mixer-pro.sock."""
    cmd = ["ssh", f"root@{host}",
           f"echo '{payload}' | socat - UNIX-CONNECT:/run/mixer-pro.sock"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
    return r.stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ckpt', default='v5_12_full_epoch029')
    ap.add_argument('--wav',  default=str(WORKSPACE / 'outputs' /
                                          'cambridge-bigmeansoundmachine-contraband_raw.wav'))
    ap.add_argument('--host', default='192.168.0.9')
    ap.add_argument('--force_drive',   type=float, default=6.0)
    ap.add_argument('--force_balance', type=float, default=0.0)
    args = ap.parse_args()

    params = predict_params_mean(args.ckpt, Path(args.wav),
                                  force_drive=args.force_drive,
                                  force_balance=args.force_balance)

    print()
    print("=== Step 1 : Configure insert chain ===")
    set_insert = json.dumps({
        "op": "set_insert",
        "plugins": [{"engine": "lv2", "uri": uri} for uri in CHAIN_URIS],
    })
    r = ssh_send(args.host, set_insert)
    print(f"  → {r}")

    print()
    print("=== Step 2 : Push 62 params via bulk ===")
    bulk = build_bulk_payload(params)
    print(f"  payload size: {len(bulk)} chars, ~{bulk.count('[')-2} params")
    r = ssh_send(args.host, bulk)
    print(f"  → {r}")

    print()
    print("=== Step 3 : Verify insert active ===")
    r = ssh_send(args.host, '{"op":"get_insert"}')
    print(f"  → {r[:200]}{'...' if len(r) > 200 else ''}")

    print()
    print("✓ Insert chain v5.12 active sur board.")
    print("  Maintenant play un wav via UAC2 stems pour entendre la chaîne LV2.")


if __name__ == '__main__':
    main()
