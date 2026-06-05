"""
V9.5.3 phase 3 — Pipeline complet : chaîne mastering surrogate PyTorch.

Cascade :
    Para EQ x16 → Calf Exciter → Calf StereoTools → LSP Limiter

Tout différentiable PyTorch. Forward pass complet sur audio stéréo →
output traité. Gradients exploitables pour training NPU.

Comparaison validation surrogate vs vraie chaîne LV2 (lv2_chain.Chain) :
    on applique les mêmes params à chaque, on mesure MSE entre les
    outputs. Si MSE faible → surrogate fiable pour training.

Usage :
    chain = MasteringChainSurrogate(sr=48000.0)
    out = chain(audio)               # defaults
    out = chain(audio, params=dict)  # avec params NPU
"""

import math
import numpy as np
import torch
import torch.nn as nn

from surrogate_eq import ParaEQx16Surrogate
from surrogate_exciter import CalfExciterSurrogate
from surrogate_stereo import CalfStereoToolsSurrogate
from surrogate_limiter import LSPLimiterSurrogate


class MasteringChainSurrogate(nn.Module):
    """Chaîne complète différentiable, identique au mixer-pro insert V10.

    Si learnable=True, tous les params sont des nn.Parameter directement
    optimisables (pour POC). Le vrai NPU les prédira au runtime via
    `forward(x, params)`.
    """

    def __init__(self, sr: float = 48000.0, learnable: bool = False):
        super().__init__()
        self.sr = sr
        self.eq      = ParaEQx16Surrogate(sr=sr, learnable=learnable)
        self.exciter = CalfExciterSurrogate(sr=sr, learnable=learnable)
        self.stereo  = CalfStereoToolsSurrogate(learnable=learnable)
        self.limiter = LSPLimiterSurrogate(sr=sr, learnable=learnable)

    def forward(self, x: torch.Tensor, params: dict = None) -> torch.Tensor:
        """x : (..., 2, N). Returns same shape.

        params : optional dict avec sous-clés 'eq', 'exciter', 'stereo',
        'limiter', chacun contenant les overrides nommés (cf forward de
        chaque surrogate).
        """
        p = params or {}
        y = self.eq(x,     **p.get('eq',      {}))
        y = self.exciter(y, **p.get('exciter', {}))
        y = self.stereo(y,  **p.get('stereo',  {}))
        y = self.limiter(y, **p.get('limiter', {}))
        return y

    def n_params_total(self) -> int:
        """Nombre total de params dynamiques (= ce que le NPU doit prédire).

        EQ : 16 × 3 (freq + gain + Q) = 48
        Exciter : 4 (amount, drive, freq_hz, ceiling)
        Stereo : 4 (balance, mid_gain, side_gain, sm_swap)
        Limiter : 6 (threshold, ceiling, attack, release, input, output)
        Total : 62
        """
        return 48 + 4 + 4 + 6

    def default_param_dict(self):
        """Retourne un dict avec les params dynamiques aux valeurs default.

        Utile pour valider et seed initial."""
        return {
            'eq': {
                'freq':    self.eq.freq.detach().clone(),
                'gain_db': self.eq.gain_db.detach().clone(),
                'q':       self.eq.q.detach().clone(),
            },
            'exciter': {
                'amount':  self.exciter.amount.detach().clone(),
                'drive':   self.exciter.drive.detach().clone(),
                'freq_hz': self.exciter.freq_hz.detach().clone(),
                'ceiling': self.exciter.ceiling.detach().clone(),
            },
            'stereo': {
                'balance':   self.stereo.balance.detach().clone(),
                'mid_gain':  self.stereo.mid_gain.detach().clone(),
                'side_gain': self.stereo.side_gain.detach().clone(),
                'sm_swap':   self.stereo.sm_swap.detach().clone(),
            },
            'limiter': {
                'threshold_db': self.limiter.threshold_db.detach().clone(),
                'ceiling_lin':  self.limiter.ceiling_lin.detach().clone(),
                'attack_ms':    self.limiter.attack_ms.detach().clone(),
                'release_ms':   self.limiter.release_ms.detach().clone(),
                'input_db':     self.limiter.input_db.detach().clone(),
                'output_db':    self.limiter.output_db.detach().clone(),
            },
        }


def quick_validation():
    """Sanity : forward chaîne sur 1s noise, mesure RMS in/out (defaults)."""
    print("[1/3] Building surrogate chain...")
    chain = MasteringChainSurrogate(sr=48000.0, learnable=False)
    print(f"    n_params_total: {chain.n_params_total()}")

    print("[2/3] Forward 1s noise...")
    np.random.seed(42)
    noise = np.random.randn(2, 48000).astype(np.float32) * 0.1
    x = torch.from_numpy(noise)
    with torch.no_grad():
        y = chain(x).numpy()

    print(f"[3/3] Stats :")
    print(f"    input  RMS = {20 * np.log10(np.sqrt(np.mean(noise**2)) + 1e-12):.2f} dB")
    print(f"    output RMS = {20 * np.log10(np.sqrt(np.mean(y**2)) + 1e-12):.2f} dB")
    print(f"    output peak = {np.abs(y).max():.4f} (limiter cap)")

    # Gradient check : run backward sur loss simple
    print()
    print("[Gradient check] backward via MSE loss...")
    chain_l = MasteringChainSurrogate(sr=48000.0, learnable=True)
    x = torch.randn(2, 48000) * 0.1
    target = torch.randn(2, 48000) * 0.1
    y = chain_l(x)
    loss = torch.mean((y - target)**2)
    loss.backward()
    grads_ok = []
    for n, p in chain_l.named_parameters():
        if p.grad is not None:
            grads_ok.append(f"  {n:30s} grad_norm={p.grad.norm().item():.4f}")
    print('\n'.join(grads_ok))


if __name__ == '__main__':
    quick_validation()
