"""
V9.5.3 phase 2a — Surrogate PyTorch différentiable du Calf Exciter.

Calf Exciter applique une saturation harmonique sur les hautes fréquences
pour ajouter "présence" et "air" au son.

Architecture (simplifiée du LADSPA Calf Exciter) :
    1. HPF à f_cutoff sur l'input → bande haute
    2. Saturation tanh(drive · x) sur la bande haute
    3. Mix wet × amount + dry × (1 - amount)

Params différentiables :
    amount   : 0..1   (mix dry/wet)
    drive    : 1..10  (gain pré-saturation)
    freq     : 1000..20000 Hz (HPF cutoff)
    ceiling  : 0..1   (post-clip soft)
"""

import math
import torch
import torch.nn as nn
import torchaudio.functional as TAF


def highpass_biquad(freq_hz: torch.Tensor, q: torch.Tensor, sr: float):
    """RBJ HPF biquad coefs. Returns (b, a) shape (3,)."""
    omega = 2.0 * math.pi * freq_hz / sr
    sin_w = torch.sin(omega)
    cos_w = torch.cos(omega)
    alpha = sin_w / (2.0 * q.clamp(min=0.1))

    b0 = (1.0 + cos_w) / 2.0
    b1 = -(1.0 + cos_w)
    b2 = (1.0 + cos_w) / 2.0
    a0 = 1.0 + alpha
    a1 = -2.0 * cos_w
    a2 = 1.0 - alpha
    b = torch.stack([b0, b1, b2], dim=-1)
    a = torch.stack([a0, a1, a2], dim=-1)
    return b, a


class CalfExciterSurrogate(nn.Module):
    """Reproduction simplifiée du Calf Exciter en PyTorch différentiable.

    Mapping params Calf → notre surrogate :
        amount  → 'amount'   (mix wet/dry)
        drive   → 'drive'    (saturation depth)
        freq    → 'freq_hz'  (HPF cutoff)
        ceiling → 'ceiling'  (clip cap)
    """

    DEFAULTS = {
        'amount':   1.0,
        'drive':    3.0,
        'freq_hz': 5000.0,
        'ceiling':  1.0,
        'q':        0.707,
    }

    def __init__(self, sr: float = 48000.0, learnable: bool = False):
        super().__init__()
        self.sr = sr
        f = lambda v: torch.tensor(v, dtype=torch.float32)
        if learnable:
            self.amount  = nn.Parameter(f(self.DEFAULTS['amount']))
            self.drive   = nn.Parameter(f(self.DEFAULTS['drive']))
            self.freq_hz = nn.Parameter(f(self.DEFAULTS['freq_hz']))
            self.ceiling = nn.Parameter(f(self.DEFAULTS['ceiling']))
        else:
            self.register_buffer('amount',  f(self.DEFAULTS['amount']))
            self.register_buffer('drive',   f(self.DEFAULTS['drive']))
            self.register_buffer('freq_hz', f(self.DEFAULTS['freq_hz']))
            self.register_buffer('ceiling', f(self.DEFAULTS['ceiling']))
        self.register_buffer('q', f(self.DEFAULTS['q']))

    def forward(self, x: torch.Tensor,
                amount: torch.Tensor = None,
                drive: torch.Tensor = None,
                freq_hz: torch.Tensor = None,
                ceiling: torch.Tensor = None) -> torch.Tensor:
        a    = self.amount  if amount  is None else amount
        d    = self.drive   if drive   is None else drive
        f    = self.freq_hz if freq_hz is None else freq_hz
        clip = self.ceiling if ceiling is None else ceiling

        # 1) HPF pour isoler la bande haute
        b, a_coefs = highpass_biquad(f, self.q, self.sr)
        high = TAF.lfilter(x, a_coefs, b, clamp=False)

        # 2) Saturation tanh sur la bande haute
        wet = torch.tanh(d * high)

        # 3) Mix wet × amount + dry × 1
        mix = x + a * wet

        # 4) Soft ceiling
        return clip * torch.tanh(mix / clip.clamp(min=1e-6))
