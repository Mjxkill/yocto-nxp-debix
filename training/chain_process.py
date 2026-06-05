#!/usr/bin/env python3
"""
V9.5 — POC : applique la chaîne mastering V10 à un WAV stéréo.

Mêmes plugins, mêmes URIs, mêmes ranges que mixer-pro côté board.

Usage :
    python3 chain_process.py input.wav output.wav

Plugins de la chaîne (validés board V9.4) :
    1. LSP Para EQ x16 stereo  — tonal + compression spectrale dynamique
    2. Calf Exciter            — harmoniques (saturation)
    3. Calf StereoTools        — image stéréo
    4. LSP Limiter Stereo      — limiter true-peak

Params actuels = defaults plugin. Pour les modifier (future training) :
    chain.set_param(slot=0, name_or_idx='g_0', value=0.5)
"""

import sys
import numpy as np
import soundfile as sf

from lv2_chain import Chain


CHAIN_URIS = [
    'http://lsp-plug.in/plugins/lv2/para_equalizer_x16_stereo',
    'http://calf.sourceforge.net/plugins/Exciter',
    'http://calf.sourceforge.net/plugins/StereoTools',
    'http://lsp-plug.in/plugins/lv2/limiter_stereo',
]


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} input.wav output.wav")
        sys.exit(1)
    in_path, out_path = sys.argv[1], sys.argv[2]

    # Load wav stéréo (force 48 kHz pour cohérence avec board)
    audio, sr = sf.read(in_path, dtype='float32', always_2d=True)
    print(f"Loaded {in_path}: {audio.shape} @ {sr} Hz")

    if sr != 48000:
        print(f"⚠ resample needed: {sr} Hz → 48 kHz (utilise librosa)")
        import librosa
        audio = librosa.resample(audio.T, orig_sr=sr, target_sr=48000).T
        sr = 48000
        print(f"Resampled to {audio.shape} @ {sr} Hz")

    # Mono → stereo dupliqué
    if audio.shape[1] == 1:
        audio = np.repeat(audio, 2, axis=1)

    audio_l = audio[:, 0].copy()
    audio_r = audio[:, 1].copy()

    # Build chain
    print("\nBuilding chain...")
    chain = Chain(sr=48000.0, block=96)
    for uri in CHAIN_URIS:
        slot = chain.add(uri)
        print(f"  slot {slot} : {uri.split('/')[-1]}")

    # Inspect params (premiers 8 du slot 0 = Para EQ)
    print(f"\nParam ranges slot 0 (Para EQ x16, first 8) :")
    for i, (p, name, mn, mx, df) in enumerate(chain.params(0)):
        if i >= 8: break
        print(f"  {name:10s} min={mn:>9.3f} max={mx:>9.3f} def={df:>9.3f}")

    # Process
    print(f"\nProcessing {len(audio_l)} samples ({len(audio_l)/sr:.1f}s)...")
    out_l, out_r = chain.process_wav(audio_l, audio_r)

    # Write output
    out_audio = np.column_stack([out_l, out_r])
    sf.write(out_path, out_audio, sr)
    print(f"Wrote {out_path}: peak_l={np.abs(out_l).max():.4f} "
          f"peak_r={np.abs(out_r).max():.4f}")


if __name__ == '__main__':
    main()
