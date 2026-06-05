"""
V9.5.3 phase 5 — Modèle ML prédicteur des params de chaîne mastering.

Architecture initiale : MLP 256×2 (cohérent avec MATLAB existant).
Input  : 17 features audio (cf features.py)
Output : 62 params chain (48 EQ + 4 exciter + 4 stereo + 6 limiter)

Le modèle prédit des params **normalisés [0, 1]** que `denormalize_params`
remappe vers les ranges réels des plugins LV2 (pour write côté board).

Compatibilité quantization INT8 : MLP avec ReLU + Linear sans branche
exotique → trivial pour TFLite quantizer.
"""

import math
import numpy as np
import torch
import torch.nn as nn


N_FEATURES_IN  = 17
N_PARAMS_OUT   = 62

# Param indices dans le vecteur 62-D :
PARAM_LAYOUT = {
    'eq.freq':       slice( 0, 16),    # 16 floats (Hz)
    'eq.gain_db':    slice(16, 32),    # 16 floats (dB)
    'eq.q':          slice(32, 48),    # 16 floats
    'exciter.amount': 48,
    'exciter.drive':  49,
    'exciter.freq_hz':50,
    'exciter.ceiling':51,
    'stereo.balance':    52,
    'stereo.mid_gain':   53,
    'stereo.side_gain':  54,
    'stereo.sm_swap':    55,
    'limiter.threshold_db': 56,
    'limiter.ceiling_lin':  57,
    'limiter.attack_ms':    58,
    'limiter.release_ms':   59,
    'limiter.input_db':     60,
    'limiter.output_db':    61,
}

# Ranges réels [min, max] des params utilisables par le NPU.
# Ce sont des valeurs raisonnables pour mastering, pas les limites absolues
# des plugins (NPU ne va pas écrire 1000.0 sur g_in).
PARAM_RANGES = {
    'eq.freq':              (20.0,     20000.0),
    'eq.gain_db':           (-12.0,    +12.0),
    'eq.q':                 (0.3,      4.0),
    'exciter.amount':       (0.0,      1.0),
    'exciter.drive':        (1.0,      6.0),
    'exciter.freq_hz':      (1000.0,   12000.0),
    'exciter.ceiling':      (0.5,      1.0),
    'stereo.balance':       (-0.5,     +0.5),
    'stereo.mid_gain':      (0.5,      1.5),
    'stereo.side_gain':     (0.3,      2.0),
    'stereo.sm_swap':       (0.0,      0.5),
    'limiter.threshold_db': (-12.0,    0.0),
    'limiter.ceiling_lin':  (0.85,     0.99),
    'limiter.attack_ms':    (0.3,      10.0),
    'limiter.release_ms':   (10.0,     200.0),
    'limiter.input_db':     (0.0,      12.0),
    'limiter.output_db':    (-6.0,     0.0),
}


def normalize_params(params_dict: dict) -> torch.Tensor:
    """Convertit un dict default_param_dict() de chain → tensor (62,) ∈ [0,1]."""
    out = torch.zeros(N_PARAMS_OUT)
    for key, idx in PARAM_LAYOUT.items():
        path = key.split('.')
        v = params_dict[path[0]][path[1]]
        mn, mx = PARAM_RANGES[key]
        v_norm = (v - mn) / (mx - mn)
        out[idx] = v_norm.clamp(0.0, 1.0) if isinstance(v, torch.Tensor) \
                                          else max(0.0, min(1.0, float(v_norm)))
    return out


def denormalize_params(params_norm: torch.Tensor) -> dict:
    """Inverse : tensor (62,) ∈ [0,1] → dict utilisable par chain forward.

    params_norm : shape (62,) ou (B, 62). Returns dict de tensors compatibles
    avec chain(x, params=...).
    """
    has_batch = params_norm.dim() == 2
    if not has_batch:
        params_norm = params_norm.unsqueeze(0)
    B = params_norm.shape[0]

    def lookup(key, slot):
        mn, mx = PARAM_RANGES[key]
        v = params_norm[..., slot] * (mx - mn) + mn
        return v.squeeze(0) if not has_batch else v

    out = {
        'eq': {
            'freq':    lookup('eq.freq',    PARAM_LAYOUT['eq.freq']),
            'gain_db': lookup('eq.gain_db', PARAM_LAYOUT['eq.gain_db']),
            'q':       lookup('eq.q',       PARAM_LAYOUT['eq.q']),
        },
        'exciter': {
            'amount':  lookup('exciter.amount',  PARAM_LAYOUT['exciter.amount']),
            'drive':   lookup('exciter.drive',   PARAM_LAYOUT['exciter.drive']),
            'freq_hz': lookup('exciter.freq_hz', PARAM_LAYOUT['exciter.freq_hz']),
            'ceiling': lookup('exciter.ceiling', PARAM_LAYOUT['exciter.ceiling']),
        },
        'stereo': {
            'balance':   lookup('stereo.balance',   PARAM_LAYOUT['stereo.balance']),
            'mid_gain':  lookup('stereo.mid_gain',  PARAM_LAYOUT['stereo.mid_gain']),
            'side_gain': lookup('stereo.side_gain', PARAM_LAYOUT['stereo.side_gain']),
            'sm_swap':   lookup('stereo.sm_swap',   PARAM_LAYOUT['stereo.sm_swap']),
        },
        'limiter': {
            'threshold_db': lookup('limiter.threshold_db', PARAM_LAYOUT['limiter.threshold_db']),
            'ceiling_lin':  lookup('limiter.ceiling_lin',  PARAM_LAYOUT['limiter.ceiling_lin']),
            'attack_ms':    lookup('limiter.attack_ms',    PARAM_LAYOUT['limiter.attack_ms']),
            'release_ms':   lookup('limiter.release_ms',   PARAM_LAYOUT['limiter.release_ms']),
            'input_db':     lookup('limiter.input_db',     PARAM_LAYOUT['limiter.input_db']),
            'output_db':    lookup('limiter.output_db',    PARAM_LAYOUT['limiter.output_db']),
        },
    }
    return out


class MasteringMLP(nn.Module):
    """MLP 256×2 ReLU, output 62 params normalisés.

    Sortie sigmoidée → ∈ [0,1], denormalize ensuite.

    Cohérent quantization INT8 TFLite : nn.Linear + nn.ReLU + sigmoid.
    """

    def __init__(self, hidden: int = 256, n_layers: int = 2):
        super().__init__()
        layers = []
        in_dim = N_FEATURES_IN
        for _ in range(n_layers):
            layers.append(nn.Linear(in_dim, hidden))
            layers.append(nn.ReLU())
            in_dim = hidden
        layers.append(nn.Linear(in_dim, N_PARAMS_OUT))
        layers.append(nn.Sigmoid())
        self.net = nn.Sequential(*layers)

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        """features : (B, N_FEATURES_IN). Returns (B, N_PARAMS_OUT) ∈ [0,1]."""
        return self.net(features)


def n_parameters(model: nn.Module) -> int:
    return sum(p.numel() for p in model.parameters())


if __name__ == '__main__':
    m = MasteringMLP()
    print(f"MasteringMLP : {n_parameters(m):,} trainable params")
    x = torch.randn(4, N_FEATURES_IN)
    y = m(x)
    print(f"forward (4, {N_FEATURES_IN}) → {y.shape}")
    # Roundtrip norm/denorm
    from surrogate_chain import MasteringChainSurrogate
    chain = MasteringChainSurrogate()
    d = chain.default_param_dict()
    norm = normalize_params(d)
    print(f"normalize default params → tensor min={norm.min():.3f} max={norm.max():.3f}")
    back = denormalize_params(norm)
    print(f"denorm OK: eq.gain_db = {back['eq']['gain_db'][:4].tolist()}")
