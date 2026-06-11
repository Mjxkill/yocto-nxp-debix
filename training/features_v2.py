"""V5.16 — Encoder Mel 64 + MFCC 20 + RMS global = 85 features par trame.

Beaucoup plus riche que features.py (11 features) pour permettre au modèle
de discriminer les chunks (causes du mode collapse v5.13/14/15).

Layout d'une trame :
  [0..63]    : log Mel-spectrogram 64 bins (20 Hz - 20 kHz, log-spaced)
  [64..83]   : 20 MFCC (DCT du log Mel, indexes 0..19)
  [84]       : RMS global dB

Chunk 100ms @ 48kHz = 4800 samples. Frame size 480 (10ms) hop 480 → 10 trames.
"""
import numpy as np

SR_DEFAULT       = 48000
FRAME_SIZE       = 480           # 10 ms @ 48 kHz
HOP_SIZE         = 480           # 10 ms hop (non-overlap entre trames intra-chunk)
N_MELS           = 64
N_MFCC           = 20
N_FEATURES_V2    = N_MELS + N_MFCC + 1   # 85
FFT_SIZE         = 1024          # zero-pad de 480 → 1024 pour résolution fréquentielle

# Pré-calcul filterbank Mel (log-spaced 20-20000 Hz).
def _mel_filterbank(n_mels=N_MELS, n_fft=FFT_SIZE, sr=SR_DEFAULT,
                     fmin=20.0, fmax=20000.0):
    fmax = min(fmax, sr / 2)
    edges = np.geomspace(fmin, fmax, n_mels + 1)
    bins = np.linspace(0, sr / 2, n_fft // 2 + 1)
    fb = np.zeros((n_mels, n_fft // 2 + 1), dtype=np.float32)
    for m in range(n_mels):
        lo, hi = edges[m], edges[m + 1]
        mask = (bins >= lo) & (bins < hi)
        if mask.sum() > 0:
            fb[m, mask] = 1.0 / mask.sum()
    return fb

_MEL_FB = _mel_filterbank()

# DCT matrix (type-II orthonormal) pour 20 MFCC depuis 64 Mel
def _dct_matrix(n_in=N_MELS, n_out=N_MFCC):
    M = np.zeros((n_out, n_in), dtype=np.float32)
    for k in range(n_out):
        for n in range(n_in):
            M[k, n] = np.cos(np.pi * (n + 0.5) * k / n_in)
    M *= np.sqrt(2.0 / n_in)
    M[0] *= 1.0 / np.sqrt(2.0)
    return M

_DCT = _dct_matrix()


def compute_features_v2(audio: np.ndarray, sr: int = SR_DEFAULT) -> np.ndarray:
    """Compute Mel + MFCC + global features pour un chunk audio.

    audio : (N,) ou (N, C) float32. Si stéréo on prend le mid (L+R)/2.
    sr    : sample rate
    Returns : (n_frames, 85) float32
    """
    if audio.ndim == 2:
        # Mid only
        audio = 0.5 * (audio[:, 0] + audio[:, 1])
    audio = audio.astype(np.float32)
    N = len(audio)
    n_frames = max(0, (N - FRAME_SIZE) // HOP_SIZE + 1)
    if n_frames == 0:
        return np.zeros((0, N_FEATURES_V2), dtype=np.float32)

    out = np.zeros((n_frames, N_FEATURES_V2), dtype=np.float32)
    win = np.hanning(FRAME_SIZE).astype(np.float32)

    for f in range(n_frames):
        i0 = f * HOP_SIZE
        frame = audio[i0:i0 + FRAME_SIZE] * win
        # Zero-pad to FFT_SIZE
        spec = np.fft.rfft(frame, n=FFT_SIZE)
        pwr  = np.abs(spec) ** 2
        # Mel
        mel  = _MEL_FB @ pwr                          # (64,)
        log_mel = np.log10(mel + 1e-10)
        out[f, 0:N_MELS] = log_mel
        # MFCC
        mfcc = _DCT @ log_mel                         # (20,)
        out[f, N_MELS:N_MELS + N_MFCC] = mfcc
        # RMS global dB
        rms = np.sqrt((frame * frame).mean() + 1e-12)
        out[f, -1] = 20 * np.log10(rms + 1e-12)

    return out


if __name__ == '__main__':
    sr = SR_DEFAULT
    chunk = 0.1   # 100 ms
    n = int(sr * chunk)
    # Sinus 1 kHz
    t = np.arange(n) / sr
    x = (0.3 * np.sin(2 * np.pi * 1000 * t)).astype(np.float32)
    feats = compute_features_v2(x, sr=sr)
    print(f'Input    : {n} samples, sinus 1kHz')
    print(f'Features : {feats.shape}  (= {feats.shape[0]} trames × {N_FEATURES_V2} dims)')
    print(f'Mel peak bin : argmax = {feats[0, :N_MELS].argmax()}  (should be ~ band of 1kHz)')
    print(f'MFCC[0..5]   : {feats[0, N_MELS:N_MELS+6].round(3)}')
    print(f'RMS global   : {feats[0, -1]:.2f} dB')

    # Bruit blanc
    np.random.seed(0)
    x2 = (0.3 * np.random.randn(n)).astype(np.float32)
    feats2 = compute_features_v2(x2, sr=sr)
    print()
    print(f'Bruit blanc :')
    print(f'  Mel mean  : {feats2[0, :N_MELS].mean():.2f}  (flat spectrum)')
    print(f'  MFCC[0]   : {feats2[0, N_MELS]:.2f}  (energy)')

    # Vérif que sinus ≠ bruit
    diff = np.abs(feats - feats2).mean()
    print(f'Mean abs diff (sinus vs bruit) : {diff:.2f}  (should be high)')
