"""
V9.5.3 phase 4 — Extracteur features audio (port du MATLAB dataset_creator.m).

Reproduit en NumPy/PyTorch les 17 features par frame utilisées par le
modèle MATLAB existant :
    - 5 énergies bandes Mid
    - 5 énergies bandes Side
    - 5 centroïdes spectraux par bande (mid)
    - 1 niveau global Mid
    - 1 niveau global Side

Total : 17 features par frame.

Frame : 1024 samples Hann window, hop 512 → frames @ 48 kHz / 512 = 93.75 Hz
        ≈ ~11 ms par frame (cohérent avec MATLAB existant).

Bandes (5) :
    low      :   20 Hz –   200 Hz
    lowmid   :  200 Hz –   800 Hz
    mid      :  800 Hz –  2500 Hz
    highmid  : 2500 Hz –  8000 Hz
    high     : 8000 Hz – 20000 Hz
"""

import numpy as np
import torch


SR_DEFAULT     = 48000
FRAME_SIZE     = 1024
HOP_SIZE       = 512
N_BANDS        = 5
N_FEATURES     = N_BANDS * 3 + 2          # 5 mid + 5 side + 5 centroïdes + 2 niveaux globaux = 17 (v1 legacy)
# V9.5.3-v2 : drop side features (artifact dataset raw mono → master stéréo).
# Mid-only = 5 mid_rms + 5 mid_centroid + 1 mid_global = 11 features.
N_FEATURES_MID_ONLY = N_BANDS * 2 + 1     # 11

BAND_HZ = np.array([
    (   20.0,   200.0),
    (  200.0,   800.0),
    (  800.0,  2500.0),
    ( 2500.0,  8000.0),
    ( 8000.0, 20000.0),
], dtype=np.float32)


def _hann(N: int) -> np.ndarray:
    n = np.arange(N, dtype=np.float32)
    return 0.5 * (1.0 - np.cos(2.0 * np.pi * n / (N - 1)))


def _band_indices(sr: int, n_fft: int) -> list:
    """Indices STFT pour chaque bande [(lo, hi), ...]."""
    freqs = np.linspace(0, sr / 2, n_fft // 2 + 1)
    result = []
    for lo_hz, hi_hz in BAND_HZ:
        lo = int(np.searchsorted(freqs, lo_hz, side='left'))
        hi = int(np.searchsorted(freqs, hi_hz, side='right'))
        result.append((lo, hi))
    return result


def compute_features(audio: np.ndarray, sr: int = SR_DEFAULT) -> np.ndarray:
    """Extrait les features frame-wise d'un audio stéréo.

    audio : (N, 2) ou (2, N)  float32 — stéréo (mono dup en amont)
    sr    : sample rate (default 48000)

    Returns : (n_frames, N_FEATURES)  float32
    """
    # Normalise à shape (2, N)
    if audio.ndim == 2 and audio.shape[1] == 2 and audio.shape[0] != 2:
        audio = audio.T
    assert audio.shape[0] == 2, f"expect stereo, got shape {audio.shape}"

    L, R = audio[0], audio[1]
    mid  = 0.5 * (L + R).astype(np.float32)
    side = 0.5 * (L - R).astype(np.float32)

    n_total = len(mid)
    n_frames = max(0, (n_total - FRAME_SIZE) // HOP_SIZE + 1)
    if n_frames == 0:
        return np.zeros((0, N_FEATURES), dtype=np.float32)

    win = _hann(FRAME_SIZE)
    bands_idx = _band_indices(sr, FRAME_SIZE)
    freqs = np.linspace(0, sr / 2, FRAME_SIZE // 2 + 1, dtype=np.float32)

    feats = np.zeros((n_frames, N_FEATURES), dtype=np.float32)
    for f in range(n_frames):
        i0 = f * HOP_SIZE
        i1 = i0 + FRAME_SIZE
        frame_m = mid[i0:i1] * win
        frame_s = side[i0:i1] * win

        # rFFT — spectres mid et side
        sp_m = np.abs(np.fft.rfft(frame_m)).astype(np.float32)
        sp_s = np.abs(np.fft.rfft(frame_s)).astype(np.float32)

        # Features per band
        for b, (lo, hi) in enumerate(bands_idx):
            energy_m = np.sqrt(np.mean(sp_m[lo:hi]**2) + 1e-12)
            energy_s = np.sqrt(np.mean(sp_s[lo:hi]**2) + 1e-12)
            # log énergies (cohérent avec MATLAB log-spectrum)
            feats[f, b]              = 20.0 * np.log10(energy_m + 1e-12)
            feats[f, N_BANDS + b]    = 20.0 * np.log10(energy_s + 1e-12)
            # Centroïde spectral (Hz pondéré par magnitude)
            mag_sum = sp_m[lo:hi].sum() + 1e-12
            centroid = (freqs[lo:hi] * sp_m[lo:hi]).sum() / mag_sum
            feats[f, 2 * N_BANDS + b] = centroid

        # Niveaux globaux Mid/Side
        feats[f, 3 * N_BANDS    ] = 20.0 * np.log10(np.sqrt(np.mean(frame_m**2)) + 1e-12)
        feats[f, 3 * N_BANDS + 1] = 20.0 * np.log10(np.sqrt(np.mean(frame_s**2)) + 1e-12)

    return feats


def normalize_features(feats: np.ndarray, stats: dict = None) -> np.ndarray:
    """Normalise features (z-score). Si stats=None, recalcule mean/std."""
    if stats is None:
        mu = feats.mean(axis=0)
        sigma = feats.std(axis=0) + 1e-9
    else:
        mu, sigma = stats['mean'], stats['std']
    return (feats - mu) / sigma


def feature_names() -> list:
    """Pour debug / introspection."""
    names = []
    for b in range(N_BANDS):
        names.append(f"mid_b{b}_rms_db")
    for b in range(N_BANDS):
        names.append(f"side_b{b}_rms_db")
    for b in range(N_BANDS):
        names.append(f"mid_b{b}_centroid_hz")
    names.append("mid_global_db")
    names.append("side_global_db")
    return names


def compute_features_mid_only(audio: np.ndarray, sr: int = SR_DEFAULT) -> np.ndarray:
    """V9.5.3-v2 : extrait features mid-only (drop side, qui sont artifact
    dataset raw mono → master stéréo).

    Returns : (n_frames, N_FEATURES_MID_ONLY=11) float32
    Layout : [mid_b0..b4_rms_db, mid_b0..b4_centroid_hz, mid_global_db]
    """
    full = compute_features(audio, sr=sr)
    if len(full) == 0:
        return np.zeros((0, N_FEATURES_MID_ONLY), dtype=np.float32)
    # Indices : mid_b0..b4 = [0..4], mid_centroid_b0..b4 = [10..14], mid_global = [15]
    out = np.zeros((len(full), N_FEATURES_MID_ONLY), dtype=np.float32)
    out[:, 0:5]   = full[:, 0:5]      # mid_b0..b4_rms_db
    out[:, 5:10]  = full[:, 10:15]    # mid_b0..b4_centroid_hz
    out[:, 10]    = full[:, 15]       # mid_global_db
    return out


def feature_names_mid_only() -> list:
    names = []
    for b in range(N_BANDS):
        names.append(f"mid_b{b}_rms_db")
    for b in range(N_BANDS):
        names.append(f"mid_b{b}_centroid_hz")
    names.append("mid_global_db")
    return names


def quick_test():
    """Smoke test sur paire raw/master du dataset."""
    import soundfile as sf
    from dataset_loader import iter_pairs, load_audio

    pair = next(iter_pairs())
    print(f"Pair: {pair.slug} ({pair.genre})")

    raw, sr = load_audio(pair.raw_path)
    tgt, _  = load_audio(pair.master_path)
    print(f"  raw  shape: {raw.shape}  @ {sr} Hz")
    print(f"  tgt  shape: {tgt.shape}")

    # Truncate à 5s pour test rapide
    n5 = 5 * sr
    raw5 = raw[:n5]
    tgt5 = tgt[:n5]

    feats_raw = compute_features(raw5, sr=sr)
    feats_tgt = compute_features(tgt5, sr=sr)

    print(f"  features raw : {feats_raw.shape}  ({N_FEATURES} per frame)")
    print(f"  features tgt : {feats_tgt.shape}")
    print()
    print("Mean delta target - raw (= ce que la chaîne doit fournir) :")
    delta = (feats_tgt - feats_raw).mean(axis=0)
    names = feature_names()
    for n, d in zip(names, delta):
        print(f"  {n:25s} : {d:+8.2f}")


if __name__ == '__main__':
    quick_test()
