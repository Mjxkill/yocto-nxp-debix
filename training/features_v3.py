"""V5.18 — Encoder v3 : court terme + long terme BF + deltas + carto normalisation.

Corrige les 3 biais fréquentiels identifiés (diag_freq_bias.py 2026-06-10) :
  1. Bandes mel mortes < 120 Hz (plus étroites que la résolution FFT 46.9 Hz)
     → court terme : 44 bandes Mel GARANTIES ≥ 2 bins FFT (200 Hz - 20 kHz)
     → long terme : FFT 8192 sur fenêtre 100 ms (résolution 5.9 Hz)
       → 16 bandes BF précises 20-630 Hz
  2. Asymétrie de fiabilité par bande → carto de normalisation par bande
     (std mesurée sur bruit rose stationnaire, égalise le "poids" des features)
  3. Dynamique temporelle → delta-Mel (dérivée par bande entre trames)

Layout d'une trame (10 ms de hop) :
  [0..43]    : log Mel 44 bandes court terme (200 Hz - 20 kHz, FFT 1024)
  [44..63]   : 20 MFCC (DCT du log Mel court terme)
  [64..79]   : 16 bandes BF log (20 - 630 Hz, FFT 8192 sur les 100 ms passées)
  [80..123]  : 44 delta-Mel (mel[t] - mel[t-1])
  [124]      : RMS global dB de la trame
  Total : 125 features

Board (ml_features.c) : 1 FFT 1024 + 1 FFT 8192 par cycle de 10 ms par canal
(bench FFTW NEON : 17.6 + 217.5 µs → 2 canaux ≈ 5% CPU, validé 2026-06-10).
"""
import numpy as np

SR_DEFAULT     = 48000
FRAME_SIZE     = 480           # 10 ms
HOP_SIZE       = 480
FFT_SHORT      = 1024          # résolution 46.9 Hz
FFT_LONG       = 8192          # fenêtre 100 ms (4800) zero-paddée → résolution 5.86 Hz
WIN_LONG       = 4800          # 100 ms

N_MEL_SHORT    = 44            # 200 Hz - 20 kHz log
N_MFCC         = 20
N_BF           = 16            # 20 - 630 Hz log (depuis FFT longue)
N_FEATURES_V3  = N_MEL_SHORT + N_MFCC + N_BF + N_MEL_SHORT + 1   # 125

_BIN_HZ_SHORT  = SR_DEFAULT / FFT_SHORT     # 46.875 Hz
_BIN_HZ_LONG   = SR_DEFAULT / FFT_LONG      # 5.859 Hz


def _mel_filterbank(n_bands, fmin, fmax, n_fft, sr, min_bins=2):
    """Filterbank rectangulaire log-spaced avec garantie ≥ min_bins par bande.
    Si une bande a moins de bins, elle est fusionnée avec la suivante (les
    edges sont recalculés en élargissant). Retourne (fb, edges_effectifs)."""
    edges = np.geomspace(fmin, fmax, n_bands + 1)
    bins = np.fft.rfftfreq(n_fft, 1.0 / sr)
    fb = np.zeros((n_bands, len(bins)), dtype=np.float32)
    for m in range(n_bands):
        lo, hi = edges[m], edges[m + 1]
        mask = (bins >= lo) & (bins < hi)
        # garantie : élargit la bande jusqu'à avoir min_bins
        while mask.sum() < min_bins and hi < sr / 2:
            hi *= 1.06
            mask = (bins >= lo) & (bins < hi)
        if mask.sum() > 0:
            fb[m, mask] = 1.0 / mask.sum()
    return fb


_MEL_SHORT = _mel_filterbank(N_MEL_SHORT, 200.0, 20000.0, FFT_SHORT, SR_DEFAULT, min_bins=2)
_MEL_BF    = _mel_filterbank(N_BF, 20.0, 630.0, FFT_LONG, SR_DEFAULT, min_bins=2)


def _dct_matrix(n_in, n_out):
    M = np.zeros((n_out, n_in), dtype=np.float32)
    for k in range(n_out):
        for n in range(n_in):
            M[k, n] = np.cos(np.pi * (n + 0.5) * k / n_in)
    M *= np.sqrt(2.0 / n_in)
    M[0] *= 1.0 / np.sqrt(2.0)
    return M

_DCT = _dct_matrix(N_MEL_SHORT, N_MFCC)

_WIN_SHORT = np.hanning(FRAME_SIZE).astype(np.float32)
_WIN_LONG_W = np.hanning(WIN_LONG).astype(np.float32)

# ---------------------------------------------------------------------------
# Carto de normalisation par bande (idée utilisateur #6).
# std de chaque feature mesurée sur bruit rose stationnaire 10 s → toute
# variation résiduelle = bruit de mesure. On normalise pour égaliser.
# Calculée une fois (lazy) et figée. Sur le board : constantes compilées.
# ---------------------------------------------------------------------------
_NORM = None

def _compute_norm():
    rng = np.random.default_rng(42)
    N = SR_DEFAULT * 10
    white = rng.standard_normal(N + 1)
    spec = np.fft.rfft(white)
    fr = np.fft.rfftfreq(len(white), 1 / SR_DEFAULT)
    spec[1:] /= np.sqrt(fr[1:])
    pink = np.fft.irfft(spec)[:N].astype(np.float32)
    pink *= 0.1 / np.sqrt((pink ** 2).mean())
    feats = compute_features_v3(pink, sr=SR_DEFAULT, _normalize=False)
    std = feats.std(axis=0)
    # std=0 (features constantes) → 1.0 (pas de normalisation)
    std[std < 1e-6] = 1.0
    # Cible : std ~0.3 (ordre de grandeur des log-mel HF sur bruit) → facteur
    return (0.3 / std).astype(np.float32)


def compute_features_v3(audio: np.ndarray, sr: int = SR_DEFAULT,
                         _normalize: bool = True) -> np.ndarray:
    """audio : (N,) mono ou (N, C) → mid. Returns (n_frames, 125) float32."""
    global _NORM
    if audio.ndim == 2:
        audio = 0.5 * (audio[:, 0] + audio[:, 1])
    audio = audio.astype(np.float32)
    N = len(audio)
    n_frames = max(0, (N - FRAME_SIZE) // HOP_SIZE + 1)
    if n_frames == 0:
        return np.zeros((0, N_FEATURES_V3), dtype=np.float32)

    out = np.zeros((n_frames, N_FEATURES_V3), dtype=np.float32)
    buf_long = np.zeros(FFT_LONG, dtype=np.float32)
    prev_mel = None

    for fidx in range(n_frames):
        i0 = fidx * HOP_SIZE
        frame = audio[i0:i0 + FRAME_SIZE] * _WIN_SHORT

        # --- court terme : FFT 1024 ---
        sp = np.fft.rfft(frame, n=FFT_SHORT)
        pwr = (sp.real ** 2 + sp.imag ** 2)
        mel = np.log10(_MEL_SHORT @ pwr + 1e-10)
        out[fidx, 0:N_MEL_SHORT] = mel
        out[fidx, N_MEL_SHORT:N_MEL_SHORT + N_MFCC] = _DCT @ mel

        # --- long terme : FFT 8192 sur les 100 ms finissant à cette trame ---
        lo = max(0, i0 + FRAME_SIZE - WIN_LONG)
        seg = audio[lo:i0 + FRAME_SIZE]
        buf_long[:] = 0.0
        buf_long[:len(seg)] = seg * _WIN_LONG_W[-len(seg):]
        sp2 = np.fft.rfft(buf_long)
        pwr2 = (sp2.real ** 2 + sp2.imag ** 2)
        out[fidx, 64:64 + N_BF] = np.log10(_MEL_BF @ pwr2 + 1e-10)

        # --- delta-Mel ---
        if prev_mel is not None:
            out[fidx, 80:80 + N_MEL_SHORT] = mel - prev_mel
        prev_mel = mel

        # --- RMS global ---
        rms = np.sqrt((frame * frame).mean() + 1e-12)
        out[fidx, -1] = 20 * np.log10(rms + 1e-12)

    if _normalize:
        if _NORM is None:
            _NORM = _compute_norm()
        out *= _NORM[None, :]
    return out


if __name__ == '__main__':
    sr = SR_DEFAULT
    # Validation T1 : bruit rose stationnaire → std par bande
    rng = np.random.default_rng(0)
    N = sr * 10
    white = rng.standard_normal(N + 1)
    spec = np.fft.rfft(white)
    fr = np.fft.rfftfreq(len(white), 1 / sr)
    spec[1:] /= np.sqrt(fr[1:])
    pink = np.fft.irfft(spec)[:N].astype(np.float32)
    pink *= 0.1 / np.sqrt((pink ** 2).mean())

    feats = compute_features_v3(pink, sr=sr, _normalize=False)
    print(f'features shape : {feats.shape} (attendu (~1000, {N_FEATURES_V3}))')
    print('\nT1 check — std par bande sur bruit rose (AVANT normalisation) :')
    print('Mel court terme (200 Hz - 20 kHz) :')
    edges_s = np.geomspace(200, 20000, N_MEL_SHORT + 1)
    for b in [0, 4, 8, 16, 24, 32, 43]:
        c = np.sqrt(edges_s[b] * edges_s[b + 1])
        print(f'  band {b:2d} ({c:7.0f} Hz) : std {10*feats[:, b].std():.2f} dB '
              + ('⚠ MORTE' if feats[:, b].std() < 1e-6 else 'OK'))
    print('Bandes BF longues (20 - 630 Hz, FFT 8192) :')
    edges_b = np.geomspace(20, 630, N_BF + 1)
    for b in [0, 3, 6, 9, 12, 15]:
        c = np.sqrt(edges_b[b] * edges_b[b + 1])
        v = feats[:, 64 + b]
        print(f'  band {b:2d} ({c:7.1f} Hz) : std {10*v.std():.2f} dB '
              + ('⚠ MORTE' if v.std() < 1e-6 else 'OK'))

    # Sinus 50 Hz : la BF doit le voir
    t = np.arange(sr) / sr
    s50 = (0.3 * np.sin(2 * np.pi * 50 * t)).astype(np.float32)
    f50 = compute_features_v3(s50, sr=sr, _normalize=False)
    bf = f50[20, 64:80]
    print(f'\nSinus 50 Hz → bande BF argmax = {bf.argmax()} '
          f'(attendu ~{int(np.log(50/20)/np.log(630/20)*16)})')
    print('valeurs BF :', np.round(bf, 1))
