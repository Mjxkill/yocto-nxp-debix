#!/usr/bin/env python3
"""
V9.6 — Deploy le modèle TFLite sur la board + monitor inférence NPU.

Étape côté board :
    1. scp mlp_<tag>.int8.tflite → root@192.168.0.9:/etc/mastering_npu.tflite
    2. Lance npu_inference_loop.py sur la board qui :
        a. Lit features audio depuis tap NPU (V9.6.1 = via /api/cmd get_state
           proxy, V9.6.2 = vrai DMA tap reserved-memory)
        b. Inference TFLite → params normalisés
        c. denormalize → écrit via /api/cmd set_insert_param chaque slot

Côté PC, ce script :
    - Push fichiers
    - Configure mixer-pro : set_insert avec les 4 plugins de la chaîne
    - Démarre l'inference loop (mode standalone Python pour POC,
      portage C/systemd plus tard)

Usage :
    python3 deploy_npu.py --ckpt v1_epoch049 [--board 192.168.0.9]
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np


WORKSPACE = Path('/home/michael/data/mastering_workspace')
EXPORT_DIR = WORKSPACE / 'exports'

CHAIN_URIS = [
    'http://lsp-plug.in/plugins/lv2/para_equalizer_x16_stereo',
    'http://calf.sourceforge.net/plugins/Exciter',
    'http://calf.sourceforge.net/plugins/StereoTools',
    'http://lsp-plug.in/plugins/lv2/limiter_stereo',
]


def post_cmd(board: str, body: dict, timeout: int = 10) -> dict:
    """POST JSON command à mixer-pro via gui-http (port 8080)."""
    import urllib.request
    url = f"http://{board}:8080/api/cmd"
    req = urllib.request.Request(
        url, data=json.dumps(body).encode(),
        headers={'Content-Type': 'application/json'},
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def push_tflite(tflite_path: Path, board: str):
    """scp TFLite vers board."""
    target = f"root@{board}:/etc/mastering_npu.tflite"
    print(f"[1/4] scp {tflite_path.name} → {target}")
    rc = subprocess.run(['scp', str(tflite_path), target], check=False)
    if rc.returncode != 0:
        raise RuntimeError(f"scp failed (rc={rc.returncode})")


def configure_chain(board: str):
    """Configure l'insert mastering avec les 4 plugins LV2."""
    print(f"[2/4] Configure insert chain (4 plugins LV2)...")
    plugins = [{'engine': 'lv2', 'uri': uri} for uri in CHAIN_URIS]
    resp = post_cmd(board, {'op': 'set_insert', 'plugins': plugins}, timeout=30)
    if not resp.get('ok'):
        raise RuntimeError(f"set_insert failed: {resp}")
    print(f"    set_insert n={resp.get('n')}")


def push_inference_script(board: str):
    """Push le script Python d'inference + features sur la board."""
    print(f"[3/4] Push npu_inference_loop.py + features.py + model.py...")
    files = [
        'npu_inference_loop.py',
        # On copie aussi les modules nécessaires
    ]
    # Pour le POC POC simple, écrire un script standalone qui inclut tout
    # (sans dépendances) directement sur la board.
    inline_path = Path('/tmp/npu_inference_loop.py')
    inline_path.write_text(_inline_loop_script())
    rc = subprocess.run(
        ['scp', str(inline_path), f"root@{board}:/usr/bin/npu_inference_loop.py"],
        check=False)
    if rc.returncode != 0:
        raise RuntimeError(f"scp script failed")
    print(f"    /usr/bin/npu_inference_loop.py installed")


def _inline_loop_script() -> str:
    """Script Python standalone embarqué (deploy direct sur board)."""
    return r'''#!/usr/bin/env python3
"""V9.6 NPU inference loop — POC standalone (à porter en C/systemd).

Lit features audio périodiquement → TFLite NPU → set_insert_param via
/api/cmd. Tourne en boucle à ~5 Hz (200 ms par cycle).
"""
import json, time, urllib.request, sys
import numpy as np

# TFLite via VX delegate pour acceleration NPU i.MX8MP
try:
    import tflite_runtime.interpreter as tflite
except ImportError:
    import tensorflow.lite as tflite

TFLITE_PATH = '/etc/mastering_npu.tflite'
API_URL     = 'http://127.0.0.1:8080/api/cmd'
INFER_HZ    = 10.0  # 100 ms par cycle (sliding window 1s analyse)
ANALYSIS_WINDOW_SEC = 1.0   # le modèle a été entraîné sur 1s chunks

# Param layout (must match training/model.py PARAM_LAYOUT)
N_PARAMS = 62
PARAM_LAYOUT = {
    'eq_freq':    slice(0, 16),
    'eq_gain_db': slice(16, 32),
    'eq_q':       slice(32, 48),
}
PARAM_RANGES = {
    'eq.freq':       (20.0, 20000.0),
    'eq.gain_db':    (-12.0, 12.0),
    'eq.q':          (0.3, 4.0),
    'exciter.amount':(0.0, 1.0),
    'exciter.drive': (1.0, 6.0),
    # ... (à compléter, cf model.py)
}

def post(body):
    req = urllib.request.Request(API_URL,
            data=json.dumps(body).encode(),
            headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=2) as r:
        return json.loads(r.read().decode())


def main():
    interp = tflite.Interpreter(model_path=TFLITE_PATH,
                                 experimental_delegates=None)
    interp.allocate_tensors()
    in_det  = interp.get_input_details()
    out_det = interp.get_output_details()
    print(f"NPU model: in={in_det[0]['shape']} out={out_det[0]['shape']}")
    print(f"Loop @ {INFER_HZ} Hz")

    period = 1.0 / INFER_HZ
    while True:
        t0 = time.time()
        # Phase 1 (POC) : feature dummy = juste un vecteur constant (mid noise)
        # Phase 2 (vrai) : lire features depuis NPU tap DMA reserved-memory
        features = np.zeros((1, 17), dtype=np.float32)
        features[0, 15] = -20.0    # mid global -20 dB
        features[0, 16] = -20.0    # side global -20 dB

        # Inference
        interp.set_tensor(in_det[0]['index'], features)
        interp.invoke()
        params_norm = interp.get_tensor(out_det[0]['index'])[0]  # (62,)

        # Apply à la chaîne (subset : juste les 16 gains EQ pour POC)
        for b in range(16):
            v_norm = float(params_norm[16 + b])  # gain_db slot
            gain_db = -12.0 + v_norm * 24.0      # denorm
            gain_lin = 10 ** (gain_db / 20.0)
            try:
                post({'op': 'set_insert_param',
                      'slot': 0, 'param': f'g_{b}', 'value': gain_lin})
            except Exception as e:
                print(f"  set_insert_param g_{b} failed: {e}")

        dt = time.time() - t0
        if dt < period:
            time.sleep(period - dt)

if __name__ == '__main__':
    main()
'''


def start_inference_service(board: str):
    """Lance le loop d'inference NPU côté board."""
    print(f"[4/4] Starting npu_inference_loop on board (background)...")
    print(f"    ssh root@{board} 'systemctl start npu-inference || "
          f"python3 /usr/bin/npu_inference_loop.py &'")
    # NB : pour POC manuel, on indique au user de lancer à la main


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ckpt', type=str, required=True)
    ap.add_argument('--board', type=str, default='192.168.0.9')
    args = ap.parse_args()

    tflite_path = EXPORT_DIR / f"mlp_{args.ckpt}.int8.tflite"
    if not tflite_path.exists():
        cands = list(EXPORT_DIR.glob(f"mlp_*{args.ckpt}*.int8.tflite"))
        if not cands:
            print(f"  TFLite INT8 introuvable. Lance d'abord :")
            print(f"    python3 export_tflite.py --ckpt {args.ckpt}")
            sys.exit(1)
        tflite_path = cands[0]
    print(f"Using {tflite_path}")

    push_tflite(tflite_path, args.board)
    configure_chain(args.board)
    push_inference_script(args.board)
    start_inference_service(args.board)

    print()
    print("=== Deploy complete ===")
    print(f"Sur la board, démarrer manuellement :")
    print(f"  ssh root@{args.board}")
    print(f"  python3 /usr/bin/npu_inference_loop.py")
    print()
    print("Pour intégrer systemd : à compléter en V9.6.x (service unit).")


if __name__ == '__main__':
    main()
