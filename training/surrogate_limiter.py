"""
V9.5.3 phase 2c — Surrogate PyTorch différentiable du LSP Limiter Stereo.

LSP Limiter est un brick-wall limiter avec lookahead et attack/release.
On approxime par un soft-clip + envelope follower différentiable.

Architecture :
    1. Input gain (preamp dB → linear)
    2. Envelope detector : peak follower avec attack (ms) et release (ms)
    3. Gain reduction : ratio entre envelope et threshold
    4. Soft saturation autour de ceiling
    5. Output gain (post-amp)

Params différentiables :
    threshold : -60..0 dB  (gain reduction starts above this)
    ceiling   :   0..-20 dB ou linéaire 0..1
    attack    :   0.1..50 ms
    release   :   1..1000 ms
    input_db  : -24..+24 dB
    output_db : -24..+24 dB

Limites :
    - lookahead omis (offline, on suppose audio non-causal OK)
    - true-peak detector omis (NPU verra ce détail au déploiement LV2 réel)
"""

import math
import torch
import torch.nn as nn
import torchaudio.functional as TAF


def db_to_lin(x_db: torch.Tensor) -> torch.Tensor:
    return 10.0 ** (x_db / 20.0)   # device-safe


def lin_to_db(x_lin: torch.Tensor) -> torch.Tensor:
    return 20.0 * torch.log10(x_lin.clamp(min=1e-12))


class LSPLimiterSurrogate(nn.Module):
    """Soft-knee peak limiter différentiable. Approximation rapide.

    Le vrai LSP utilise un detector true-peak + lookahead 5 ms ; ici
    on utilise un peak-follower exponentiel one-pole.
    """

    DEFAULTS = {
        'threshold_db': -1.0,
        'ceiling_lin':   0.99,
        'attack_ms':     2.0,
        'release_ms':   30.0,
        'input_db':      0.0,
        'output_db':     0.0,
    }

    def __init__(self, sr: float = 48000.0, learnable: bool = False):
        super().__init__()
        self.sr = sr
        f = lambda v: torch.tensor(v, dtype=torch.float32)
        names = ['threshold_db', 'ceiling_lin', 'attack_ms',
                 'release_ms', 'input_db', 'output_db']
        for n in names:
            t = f(self.DEFAULTS[n])
            if learnable:
                setattr(self, n, nn.Parameter(t))
            else:
                self.register_buffer(n, t)

    def _env_follower(self, x: torch.Tensor, attack_a: torch.Tensor,
                      release_a: torch.Tensor) -> torch.Tensor:
        """Peak envelope follower one-pole, IIR vectorisé via lfilter.

        x : (..., N) absolute amplitude (rectified).

        V9.5.3 phase 6 : approximation symétrique avec alpha = attack_a
        (le path "transient down" via release n'est pas pris en compte).
        Trade-off : ×1000 plus rapide que sample-par-sample, suffit pour
        training POC. Pour mastering final, le NPU set les vrais params
        attack/release sur la chaîne LV2 (LSP Limiter réel).
        """
        alpha = attack_a.clamp(min=1e-4, max=1.0)
        # IIR : y[n] = alpha*x[n] + (1-alpha)*y[n-1]
        # → numerateur b = [alpha], dénominateur a = [1, -(1-alpha)]
        b = torch.stack([alpha, torch.zeros_like(alpha)])
        a = torch.stack([torch.ones_like(alpha), -(1.0 - alpha)])
        return TAF.lfilter(x, a, b, clamp=False)

    def forward(self, x: torch.Tensor,
                threshold_db: torch.Tensor = None,
                ceiling_lin: torch.Tensor = None,
                attack_ms: torch.Tensor = None,
                release_ms: torch.Tensor = None,
                input_db: torch.Tensor = None,
                output_db: torch.Tensor = None) -> torch.Tensor:
        th = self.threshold_db if threshold_db is None else threshold_db
        cl = self.ceiling_lin  if ceiling_lin  is None else ceiling_lin
        at = self.attack_ms    if attack_ms    is None else attack_ms
        rl = self.release_ms   if release_ms   is None else release_ms
        ig = self.input_db     if input_db     is None else input_db
        og = self.output_db    if output_db    is None else output_db

        # 1) Input preamp
        x = x * db_to_lin(ig)

        # 2) Peak envelope sur valeur absolue (max L,R pour stereo)
        # Pour x shape (C, N), prendre max sur C
        if x.dim() >= 2 and x.shape[-2] >= 2:
            abs_max = torch.max(x.abs(), dim=-2, keepdim=False).values   # (..., N)
        else:
            abs_max = x.abs()

        # 3) one-pole coefs (attack/release) : alpha = 1 - exp(-1/(tau·sr/1000))
        a_a = 1.0 - torch.exp(-1.0 / (at.clamp(min=0.1) * self.sr / 1000.0))
        a_r = 1.0 - torch.exp(-1.0 / (rl.clamp(min=0.1) * self.sr / 1000.0))
        env = self._env_follower(abs_max, a_a, a_r)

        # 4) Gain reduction : si env > threshold, on réduit
        th_lin = db_to_lin(th)
        # Soft-knee : ratio limiteur (∞:1 brick-wall) → x → min(x, threshold)
        # Différentiable : gain = min(1, threshold / env)
        gain = torch.where(env > th_lin, th_lin / env.clamp(min=1e-12),
                           torch.ones_like(env))
        # Broadcast (..., N) → (..., 1, N) pour multiplier x (..., C, N)
        if x.dim() >= 2 and x.shape[-2] >= 2:
            gain = gain.unsqueeze(-2)
        y = x * gain

        # 5) Soft ceiling final (sécurité numérique)
        y = cl * torch.tanh(y / cl.clamp(min=1e-6))

        # 6) Output postamp
        return y * db_to_lin(og)
