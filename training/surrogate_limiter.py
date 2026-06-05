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


def db_to_lin(x_db: torch.Tensor) -> torch.Tensor:
    return torch.pow(torch.tensor(10.0, dtype=x_db.dtype, device=x_db.device), x_db / 20.0)


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
        """Peak envelope follower différentiable, one-pole asymétrique.

        x : (..., N) absolute amplitude (rectified). N samples.
        attack_a, release_a : scalaires entre 0 et 1.

        Implémentation : boucle sample-par-sample (différentiable mais lent).
        Pour N=48000 c'est ~1 sec audio, prend ~50 ms en Python. Acceptable
        pour training.
        """
        # Flatten leading dims
        orig_shape = x.shape
        x_flat = x.reshape(-1, orig_shape[-1])
        env = torch.zeros_like(x_flat)
        prev = torch.zeros(x_flat.shape[0], device=x.device, dtype=x.dtype)
        for n in range(x_flat.shape[-1]):
            xn = x_flat[:, n]
            up = xn > prev
            coef = torch.where(up, attack_a, release_a)
            prev = prev + (xn - prev) * coef
            env[:, n] = prev
        return env.reshape(orig_shape)

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
