"""
V9.5.3 phase 2b — Surrogate PyTorch différentiable du Calf StereoTools.

Manipule l'image stéréo via Mid/Side encoding :
    mid  = (L + R) / 2     side = (L - R) / 2
    mid' = mid * mid_gain  side' = side * side_gain
    L'   = mid' + side'    R'    = mid' - side'

Params différentiables :
    balance  : -1..+1   (pan L/R)
    mid_gain : 0..2     (mono content)
    side_gain: 0..2     (stereo width, >1 = wider)
    sm_swap  : 0..1     (mid/side swap fraction, 0 = normal)
"""

import torch
import torch.nn as nn


class CalfStereoToolsSurrogate(nn.Module):
    DEFAULTS = {
        'balance':   0.0,
        'mid_gain':  1.0,
        'side_gain': 1.0,
        'sm_swap':   0.0,
    }

    def __init__(self, learnable: bool = False):
        super().__init__()
        f = lambda v: torch.tensor(v, dtype=torch.float32)
        if learnable:
            self.balance   = nn.Parameter(f(self.DEFAULTS['balance']))
            self.mid_gain  = nn.Parameter(f(self.DEFAULTS['mid_gain']))
            self.side_gain = nn.Parameter(f(self.DEFAULTS['side_gain']))
            self.sm_swap   = nn.Parameter(f(self.DEFAULTS['sm_swap']))
        else:
            self.register_buffer('balance',   f(self.DEFAULTS['balance']))
            self.register_buffer('mid_gain',  f(self.DEFAULTS['mid_gain']))
            self.register_buffer('side_gain', f(self.DEFAULTS['side_gain']))
            self.register_buffer('sm_swap',   f(self.DEFAULTS['sm_swap']))

    def forward(self, x: torch.Tensor,
                balance: torch.Tensor = None,
                mid_gain: torch.Tensor = None,
                side_gain: torch.Tensor = None,
                sm_swap: torch.Tensor = None) -> torch.Tensor:
        """x : (..., 2, N) stereo. Supports unbatched (2, N) ou batched (B, 2, N)."""
        bal = self.balance   if balance   is None else balance
        mg  = self.mid_gain  if mid_gain  is None else mid_gain
        sg  = self.side_gain if side_gain is None else side_gain
        sw  = self.sm_swap   if sm_swap   is None else sm_swap

        batched = (x.dim() >= 3 and bal.dim() >= 1 and bal.shape[0] > 1)

        L = x[..., 0, :]
        R = x[..., 1, :]

        if batched:
            B = bal.shape[0]
            bal_c = bal.clamp(-1.0, 1.0).view(B, 1)
            gL = (1.0 - bal_c).clamp(min=0.0)
            gR = (1.0 + bal_c).clamp(min=0.0)
            mg_b = mg.view(B, 1)
            sg_b = sg.view(B, 1)
            sw_b = sw.view(B, 1)
        else:
            gL = (1.0 - bal.clamp(-1.0, 1.0)).clamp(min=0.0)
            gR = (1.0 + bal.clamp(-1.0, 1.0)).clamp(min=0.0)
            mg_b, sg_b, sw_b = mg, sg, sw

        L = L * gL
        R = R * gR
        mid  = 0.5 * (L + R)
        side = 0.5 * (L - R)
        mid_p  = mid  * mg_b
        side_p = side * sg_b
        mid_out  = (1.0 - sw_b) * mid_p  + sw_b * side_p
        side_out = (1.0 - sw_b) * side_p + sw_b * mid_p
        L_out = mid_out + side_out
        R_out = mid_out - side_out
        return torch.stack([L_out, R_out], dim=-2)
