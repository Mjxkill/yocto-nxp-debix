#!/usr/bin/env python3
"""
V9.5.1 — Validation POC : applique la chaîne LV2 (defaults) sur N paires
du dataset utilisateur, mesure les écarts vs target masterisé.

But : confirmer que la chaîne LV2 est utilisable et identifier ce que
le NPU devra apprendre à corriger.

Usage :
    python3 validate_chain.py [N_pairs] [first_seconds]

Defaults :
    N_pairs = 10  (premières paires du manifest)
    first_seconds = 30  (premières secondes pour rapidité)
"""

import sys
import numpy as np

from dataset_loader import iter_pairs, load_audio, audio_stats
from lv2_chain import Chain


CHAIN_URIS = [
    'http://lsp-plug.in/plugins/lv2/para_equalizer_x16_stereo',
    'http://calf.sourceforge.net/plugins/Exciter',
    'http://calf.sourceforge.net/plugins/StereoTools',
    'http://lsp-plug.in/plugins/lv2/limiter_stereo',
]


def main():
    n_pairs = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    first_s = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

    print(f"Building chain (defaults)...")
    chain = Chain(sr=48000.0, block=96)
    for uri in CHAIN_URIS:
        chain.add(uri)
    print(f"  {chain.n_slots} plugins loaded")
    print()

    print(f"{'slug':50s} {'raw_rms':>8s} {'our_rms':>8s} {'tgt_rms':>8s} "
          f"{'raw_cf':>6s} {'our_cf':>6s} {'tgt_cf':>6s}")
    print("-" * 100)

    raw_rms_diffs = []
    our_rms_diffs = []
    for i, pair in enumerate(iter_pairs()):
        if i >= n_pairs:
            break
        # Load raw + target
        raw, sr_r = load_audio(pair.raw_path)
        tgt, sr_t = load_audio(pair.master_path)
        # Truncate to first_s
        n_lim = int(first_s * 48000)
        raw = raw[:n_lim]
        tgt = tgt[:n_lim]
        # Process raw via chain LV2 defaults
        our_l, our_r = chain.process_wav(raw[:, 0], raw[:, 1])
        our = np.column_stack([our_l, our_r])
        # Stats
        sr_raw = audio_stats(raw)
        sr_our = audio_stats(our)
        sr_tgt = audio_stats(tgt)
        print(f"{pair.slug[:50]:50s} "
              f"{sr_raw['rms_db']:+8.1f} {sr_our['rms_db']:+8.1f} {sr_tgt['rms_db']:+8.1f} "
              f"{sr_raw['crest']:6.2f} {sr_our['crest']:6.2f} {sr_tgt['crest']:6.2f}")
        raw_rms_diffs.append(sr_tgt['rms_db'] - sr_raw['rms_db'])
        our_rms_diffs.append(sr_tgt['rms_db'] - sr_our['rms_db'])

    print()
    print("=== verdict (delta RMS = target - source) ===")
    print(f"  delta RMS (target - raw)  : moyen {np.mean(raw_rms_diffs):+.1f} dB  "
          f"(le master gagne ce loudness)")
    print(f"  delta RMS (target - our)  : moyen {np.mean(our_rms_diffs):+.1f} dB  "
          f"(NPU doit fournir ça)")


if __name__ == '__main__':
    main()
