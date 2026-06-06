"""
V9.5.3 Phase 1 — Surrogate PyTorch différentiable du LSP Para EQ x16 stereo.

Reproduit en PyTorch les 16 biquads peak en série, avec params (freq, gain, Q)
différentiables. Output gradient-flow exploitable pour training NPU.

Référence formules biquad peak (RBJ Audio EQ Cookbook) :
    omega = 2π · f / sr
    alpha = sin(omega) / (2Q)
    A     = 10^(gain_db / 40)

    b0 = 1 + alpha·A
    b1 = -2·cos(omega)
    b2 = 1 - alpha·A
    a0 = 1 + alpha/A
    a1 = -2·cos(omega)
    a2 = 1 - alpha/A

Normalisation : divise b0/b1/b2/a1/a2 par a0.

Usage :
    eq = ParaEQx16Surrogate(sr=48000)
    x  = torch.randn(2, 48000)             # stéréo (2, N)
    f  = torch.tensor([100, 200, 400, ...] * 1)  # 16 freqs Hz
    g  = torch.zeros(16)                   # 16 gains dB
    q  = torch.ones(16) * 0.7              # 16 Q factors
    y  = eq(x, f, g, q)                    # (2, N) processed

Différentiable wrt f, g, q : gradient descent direct.
"""

import math
import numpy as np
import torch
import torch.nn as nn
import torchaudio.functional as TAF


def lfilter_batched(x: torch.Tensor, a: torch.Tensor, b: torch.Tensor) -> torch.Tensor:
    """V9.5.3-v4 : applique des coefs IIR différents par batch item.

    x : (B, C, N) audio multi-batch multi-channel
    a, b : (B, num_taps) coefs IIR par batch
    Returns : (B, C, N)

    torchaudio.lfilter avec batching=True attend
    waveform (..., M, N) + coefs (M, taps) → applique filter[i] à waveform[..., i, :].
    On reshape (B, C, N) → (B*C, N) et duplique coefs C fois.
    """
    B, C, N = x.shape
    x_flat = x.reshape(B * C, N)
    a_flat = a.repeat_interleave(C, dim=0)
    b_flat = b.repeat_interleave(C, dim=0)
    out_flat = TAF.lfilter(x_flat, a_flat, b_flat, clamp=False, batching=True)
    return out_flat.reshape(B, C, N)


def biquad_peak_coefs(freq_hz: torch.Tensor,
                      gain_db: torch.Tensor,
                      q: torch.Tensor,
                      sr: float):
    """Compute peaking biquad coefs (b0..b2, a0..a2) pour chaque bande.

    freq_hz, gain_db, q : shape (...,) — broadcasts.
    Returns : b (..., 3), a (..., 3)
    """
    omega = 2.0 * math.pi * freq_hz / sr
    sin_w = torch.sin(omega)
    cos_w = torch.cos(omega)
    alpha = sin_w / (2.0 * q.clamp(min=0.1))
    A = 10.0 ** (gain_db / 40.0)   # device-safe (utilise device de gain_db)

    b0 = 1.0 + alpha * A
    b1 = -2.0 * cos_w
    b2 = 1.0 - alpha * A
    a0 = 1.0 + alpha / A
    a1 = -2.0 * cos_w
    a2 = 1.0 - alpha / A

    # Normalise par a0 (lfilter PyTorch attend a[0] != 0 — on garde a0 réel)
    b = torch.stack([b0, b1, b2], dim=-1)
    a = torch.stack([a0, a1, a2], dim=-1)
    return b, a


class ParaEQx16Surrogate(nn.Module):
    """16 biquads peak en série, params différentiables.

    Reproduit LSP Para EQ x16 stereo (mode=0 = peak par défaut sur toutes
    les bandes). Les params freq[i], gain_db[i], q[i] sont des tensors
    learnable (ou passés en forward pour pilotage NPU).

    Par défaut, freqs réparties logarithmiquement de 20 Hz à 20 kHz.
    """

    DEFAULT_FREQS = np.logspace(np.log10(20.0), np.log10(20000.0), 16)
    DEFAULT_Q = 0.707  # ≈ Butterworth

    def __init__(self, sr: float = 48000.0, learnable: bool = False):
        super().__init__()
        self.sr = sr
        f_init = torch.tensor(self.DEFAULT_FREQS, dtype=torch.float32)
        g_init = torch.zeros(16, dtype=torch.float32)
        q_init = torch.ones(16, dtype=torch.float32) * self.DEFAULT_Q
        if learnable:
            self.freq    = nn.Parameter(f_init)
            self.gain_db = nn.Parameter(g_init)
            self.q       = nn.Parameter(q_init)
        else:
            self.register_buffer('freq',    f_init)
            self.register_buffer('gain_db', g_init)
            self.register_buffer('q',       q_init)

    def forward(self, x: torch.Tensor,
                freq: torch.Tensor = None,
                gain_db: torch.Tensor = None,
                q: torch.Tensor = None) -> torch.Tensor:
        """Apply 16 biquads à x.

        Modes :
          - Unbatched : x (C, N), params (16,) → identique original
          - Batched   : x (B, C, N), params (B, 16) → V9.5.3-v4
        """
        f = self.freq    if freq    is None else freq
        g = self.gain_db if gain_db is None else gain_db
        Q = self.q       if q       is None else q

        batched = (f.dim() == 2)   # (B, 16) batched / (16,) unbatched

        out = x
        if batched:
            for i in range(16):
                b, a = biquad_peak_coefs(f[:, i], g[:, i], Q[:, i], self.sr)  # (B, 3)
                out = lfilter_batched(out, a, b)
        else:
            for i in range(16):
                b, a = biquad_peak_coefs(f[i], g[i], Q[i], self.sr)            # (3,)
                out = TAF.lfilter(out, a, b, clamp=False)
        return out


def quick_sanity():
    """Smoke test : load chaîne LV2 réelle pour Para EQ x16, génère bruit
    blanc, compare output PyTorch surrogate vs LV2 (defaults g=0 dB partout).

    Avec gain 0 dB partout : output devrait être ≈ input (au sign près +
    phase response des biquads cascade, qui doit être ≈ unité).
    """
    from lv2_chain import Chain

    print("[1/3] Loading PyTorch surrogate...")
    eq = ParaEQx16Surrogate(sr=48000)
    print(f"    default freqs (Hz): {eq.freq.numpy().round(1).tolist()}")
    print(f"    default gains (dB): {eq.gain_db.tolist()}")

    print("[2/3] Load LV2 real chain (Para EQ x16 seul)...")
    chain = Chain(sr=48000.0, block=96)
    chain.add('http://lsp-plug.in/plugins/lv2/para_equalizer_x16_stereo')

    print("[3/3] Forward 1s noise → compare RMS surrogate vs LV2...")
    np.random.seed(42)
    noise = np.random.randn(48000).astype(np.float32) * 0.1
    x = torch.from_numpy(np.stack([noise, noise]))           # (2, N)
    with torch.no_grad():
        y_torch = eq(x).numpy()
    y_lv2_l, y_lv2_r = chain.process_wav(noise, noise)
    y_lv2 = np.stack([y_lv2_l, y_lv2_r])

    rms_in     = float(np.sqrt(np.mean(noise**2)))
    rms_torch  = float(np.sqrt(np.mean(y_torch**2)))
    rms_lv2    = float(np.sqrt(np.mean(y_lv2**2)))
    err_torch  = 20 * np.log10(rms_torch / rms_in)
    err_lv2    = 20 * np.log10(rms_lv2 / rms_in)
    print(f"    input  RMS: {20*np.log10(rms_in):.2f} dB")
    print(f"    surrog RMS: {20*np.log10(rms_torch):.2f} dB (delta {err_torch:+.2f} dB)")
    print(f"    lv2    RMS: {20*np.log10(rms_lv2):.2f} dB (delta {err_lv2:+.2f} dB)")
    print(f"    inter-system delta: {err_torch - err_lv2:+.3f} dB")


if __name__ == '__main__':
    quick_sanity()
