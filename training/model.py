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


N_FEATURES_IN     = 17               # v1 legacy
N_FEATURES_IN_V2  = 11               # v2 : mid-only features
N_FEATURES_IN_V3  = 85               # v3 (V5.16) : Mel 64 + MFCC 20 + RMS = 85
N_PARAMS_OUT      = 62
N_PARAMS_OUT_V5_14 = 30               # v5.14 : 16 EQ gains + 14 plugin params (freq+Q fixés)
N_PARAMS_OUT_V5_17 = 26               # v5.17 MONO : 16 EQ + 4 exciter + 6 limiter (stereo retiré)

# V5.14 — bandes EQ fixes (16 ISO 1/3-octave équivalent), Q fixe.
# Couverture 31.5 Hz → 16 kHz = range mastering audio standard.
FIXED_EQ_FREQS_V5_14 = (
    31.5,  50.0,  80.0,  125.0,  200.0,  315.0,  500.0,  800.0,
    1250.0, 2000.0, 3150.0, 5000.0, 8000.0, 10000.0, 12500.0, 16000.0,
)
FIXED_EQ_QS_V5_14 = tuple([1.0] * 16)   # bandes larges, mastering standard

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
    'exciter.freq_hz':      (5000.0,   12000.0),    # V5.9 : min 5 kHz pour vrai "air"
    'exciter.ceiling':      (0.5,      1.0),
    # V9.5.3-v5 : ranges plus serrées sur stéréo pour éviter loudness war
    'stereo.balance':       (-0.2,     +0.2),    # centré (vs -0.5..+0.5 v4)
    'stereo.mid_gain':      (0.7,      1.4),    # ~ -3 à +3 dB (vs 0.5..1.5 = -6..+3.5)
    'stereo.side_gain':     (0.5,      1.7),    # ~ -6 à +4.6 dB (vs 0.3..2.0 = -10..+6)
    'stereo.sm_swap':       (0.0,      0.3),    # moins de swap (vs 0..0.5)
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


def denormalize_params_batched(params_norm: torch.Tensor) -> dict:
    """V9.5.3-v4 : version batchée de denormalize_params.

    params_norm : (B, 62) tensor → dict de tensors batchés (B, ...) ou (B,).
    Tous les sub-engines surrogate acceptent ces tensors batched directement.
    """
    assert params_norm.dim() == 2 and params_norm.shape[1] == N_PARAMS_OUT, \
        f"expected (B, {N_PARAMS_OUT}), got {params_norm.shape}"

    def lookup(key, slot):
        mn, mx = PARAM_RANGES[key]
        return params_norm[:, slot] * (mx - mn) + mn   # (B,) ou (B, k)

    return {
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


class MasteringConv1D(nn.Module):
    """V9.5.3-v2 : modèle Conv1D temporel sur features par frame.

    Input  : (B, N_FEATURES_IN_V2=11, N_frames) — features par frame
    Output : (B, N_PARAMS_OUT=62)                — params par chunk (global)

    Architecture :
      Conv1D 11 → 64  (kernel 5)
      ReLU
      Conv1D 64 → 128 (kernel 5)
      ReLU
      AdaptiveAvgPool1D → (B, 128, 1)
      Linear 128 → 62
      Sigmoid

    ~25K params, quantizable INT8.
    """

    def __init__(self, n_input: int = N_FEATURES_IN_V2):
        super().__init__()
        self.net = nn.Sequential(
            nn.Conv1d(n_input, 64, kernel_size=5, padding=2),
            nn.ReLU(),
            nn.Conv1d(64, 128, kernel_size=5, padding=2),
            nn.ReLU(),
            nn.AdaptiveAvgPool1d(1),
            nn.Flatten(),
            nn.Linear(128, N_PARAMS_OUT),
            nn.Sigmoid(),
        )

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        """features : (B, N_FEATURES_IN_V2, N_frames). Returns (B, 62)."""
        return self.net(features)


PARAM_LAYOUT_V5_17 = {
    # V5.17 MONO — sortie 26 valeurs : 16 EQ gains + exciter + limiter.
    # PAS de params stereo (modèle mono appliqué indépendamment par canal).
    'eq.gain_db':           slice(0, 16),
    'exciter.amount':       16,
    'exciter.drive':        17,
    'exciter.freq_hz':      18,
    'exciter.ceiling':      19,
    'limiter.threshold_db': 20,
    'limiter.ceiling_lin':  21,
    'limiter.attack_ms':    22,
    'limiter.release_ms':   23,
    'limiter.input_db':     24,
    'limiter.output_db':    25,
}

# V5.17 : ranges modifiés — input_db élargi 0..+18 dB (diag v5.16 : lim_ig saturé
# à 0.77, le modèle réclamait plus de headroom pour matcher la RMS du master).
PARAM_RANGES_V5_17 = dict(PARAM_RANGES)
PARAM_RANGES_V5_17['limiter.input_db'] = (0.0, 18.0)


def denormalize_params_v5_17(params_norm: torch.Tensor) -> dict:
    """V5.17 MONO — décodage 26 outputs vers dict complet chaîne.

    Stereo fixé neutre (balance=0, mid=1, side=1, swap=0) : le modèle est mono,
    appliqué indépendamment sur chaque canal — StereoTools traverse sans effet.
    EQ freq+Q fixés (FIXED_EQ_FREQS_V5_14 / Q=1).
    """
    has_batch = params_norm.dim() == 2
    if not has_batch:
        params_norm = params_norm.unsqueeze(0)
    B = params_norm.shape[0]
    dev = params_norm.device

    def denorm_key(key, idx):
        mn, mx = PARAM_RANGES_V5_17[key]
        return params_norm[:, idx] * (mx - mn) + mn

    gain_db = denorm_key('eq.gain_db', PARAM_LAYOUT_V5_17['eq.gain_db'])
    freq = torch.tensor(FIXED_EQ_FREQS_V5_14, device=dev).unsqueeze(0).expand(B, -1)
    q    = torch.tensor(FIXED_EQ_QS_V5_14,    device=dev).unsqueeze(0).expand(B, -1)

    def const(v):
        return torch.full((B,), float(v), device=dev)

    out = {
        'eq': {'freq': freq, 'gain_db': gain_db, 'q': q},
        'exciter': {
            'amount':  denorm_key('exciter.amount',  PARAM_LAYOUT_V5_17['exciter.amount']),
            'drive':   denorm_key('exciter.drive',   PARAM_LAYOUT_V5_17['exciter.drive']),
            'freq_hz': denorm_key('exciter.freq_hz', PARAM_LAYOUT_V5_17['exciter.freq_hz']),
            'ceiling': denorm_key('exciter.ceiling', PARAM_LAYOUT_V5_17['exciter.ceiling']),
        },
        'stereo': {   # neutre — mono par canal
            'balance':   const(0.0),
            'mid_gain':  const(1.0),
            'side_gain': const(1.0),
            'sm_swap':   const(0.0),
        },
        'limiter': {
            'threshold_db': denorm_key('limiter.threshold_db', PARAM_LAYOUT_V5_17['limiter.threshold_db']),
            'ceiling_lin':  denorm_key('limiter.ceiling_lin',  PARAM_LAYOUT_V5_17['limiter.ceiling_lin']),
            'attack_ms':    denorm_key('limiter.attack_ms',    PARAM_LAYOUT_V5_17['limiter.attack_ms']),
            'release_ms':   denorm_key('limiter.release_ms',   PARAM_LAYOUT_V5_17['limiter.release_ms']),
            'input_db':     denorm_key('limiter.input_db',     PARAM_LAYOUT_V5_17['limiter.input_db']),
            'output_db':    denorm_key('limiter.output_db',    PARAM_LAYOUT_V5_17['limiter.output_db']),
        },
    }
    if not has_batch:
        for k1, v1 in out.items():
            for k2, v2 in v1.items():
                out[k1][k2] = v2.squeeze(0)
    return out


PARAM_LAYOUT_V5_14 = {
    # V5.14 — sortie 30 valeurs : 16 EQ gains + plugins (sans freq/Q EQ).
    'eq.gain_db':           slice(0, 16),
    'exciter.amount':       16,
    'exciter.drive':        17,
    'exciter.freq_hz':      18,
    'exciter.ceiling':      19,
    'stereo.balance':       20,
    'stereo.mid_gain':      21,
    'stereo.side_gain':     22,
    'stereo.sm_swap':       23,
    'limiter.threshold_db': 24,
    'limiter.ceiling_lin':  25,
    'limiter.attack_ms':    26,
    'limiter.release_ms':   27,
    'limiter.input_db':     28,
    'limiter.output_db':    29,
}


def denormalize_params_v5_14(params_norm: torch.Tensor) -> dict:
    """V5.14 — décodage 30 outputs vers dict 62 params (freq+Q EQ fixés).

    params_norm : (B, 30) ou (30,). Returns dict compat surrogate_chain
    avec eq.freq = FIXED_EQ_FREQS_V5_14 et eq.q = FIXED_EQ_QS_V5_14.
    """
    has_batch = params_norm.dim() == 2
    if not has_batch:
        params_norm = params_norm.unsqueeze(0)
    B = params_norm.shape[0]
    dev = params_norm.device

    def denorm_key(key, idx):
        mn, mx = PARAM_RANGES[key]
        v_norm = params_norm[:, idx] if isinstance(idx, int) else params_norm[:, idx]
        return v_norm * (mx - mn) + mn

    # 16 EQ gains_db
    gain_db = denorm_key('eq.gain_db', PARAM_LAYOUT_V5_14['eq.gain_db'])
    # 16 fréquences fixes broadcast batch
    freq = torch.tensor(FIXED_EQ_FREQS_V5_14, device=dev).unsqueeze(0).expand(B, -1)
    q    = torch.tensor(FIXED_EQ_QS_V5_14,    device=dev).unsqueeze(0).expand(B, -1)

    out = {
        'eq': {
            'freq':    freq,
            'gain_db': gain_db,
            'q':       q,
        },
        'exciter': {
            'amount':  denorm_key('exciter.amount',  PARAM_LAYOUT_V5_14['exciter.amount']),
            'drive':   denorm_key('exciter.drive',   PARAM_LAYOUT_V5_14['exciter.drive']),
            'freq_hz': denorm_key('exciter.freq_hz', PARAM_LAYOUT_V5_14['exciter.freq_hz']),
            'ceiling': denorm_key('exciter.ceiling', PARAM_LAYOUT_V5_14['exciter.ceiling']),
        },
        'stereo': {
            'balance':   denorm_key('stereo.balance',   PARAM_LAYOUT_V5_14['stereo.balance']),
            'mid_gain':  denorm_key('stereo.mid_gain',  PARAM_LAYOUT_V5_14['stereo.mid_gain']),
            'side_gain': denorm_key('stereo.side_gain', PARAM_LAYOUT_V5_14['stereo.side_gain']),
            'sm_swap':   denorm_key('stereo.sm_swap',   PARAM_LAYOUT_V5_14['stereo.sm_swap']),
        },
        'limiter': {
            'threshold_db': denorm_key('limiter.threshold_db', PARAM_LAYOUT_V5_14['limiter.threshold_db']),
            'ceiling_lin':  denorm_key('limiter.ceiling_lin',  PARAM_LAYOUT_V5_14['limiter.ceiling_lin']),
            'attack_ms':    denorm_key('limiter.attack_ms',    PARAM_LAYOUT_V5_14['limiter.attack_ms']),
            'release_ms':   denorm_key('limiter.release_ms',   PARAM_LAYOUT_V5_14['limiter.release_ms']),
            'input_db':     denorm_key('limiter.input_db',     PARAM_LAYOUT_V5_14['limiter.input_db']),
            'output_db':    denorm_key('limiter.output_db',    PARAM_LAYOUT_V5_14['limiter.output_db']),
        },
    }
    if not has_batch:
        # squeeze
        for k1, v1 in out.items():
            for k2, v2 in v1.items():
                out[k1][k2] = v2.squeeze(0)
    return out


class _Reshape2D(nn.Module):
    """Convert (B, C, T) → (B, C, 1, T) pour utiliser Conv2D mappé NPU."""
    def forward(self, x): return x.unsqueeze(2)


class _ResidualBlock2D(nn.Module):
    """V5.16 — Block résiduel 2D : Conv → ReLU → Conv → Add (skip) → ReLU.

    Si in/out channels diffèrent, skip via Conv 1×1 (projection).
    Kaiming init explicite. NPU-compatible (Add op).
    """
    def __init__(self, c_in, c_out, kernel=(1, 3)):
        super().__init__()
        pad = (kernel[0] // 2, kernel[1] // 2)
        self.conv1 = nn.Conv2d(c_in,  c_out, kernel, padding=pad)
        self.conv2 = nn.Conv2d(c_out, c_out, kernel, padding=pad)
        self.relu  = nn.ReLU(inplace=True)
        self.skip  = (nn.Identity() if c_in == c_out
                       else nn.Conv2d(c_in, c_out, (1, 1)))
        # Kaiming init explicite
        for m in self.modules():
            if isinstance(m, nn.Conv2d):
                nn.init.kaiming_normal_(m.weight, nonlinearity='relu')
                if m.bias is not None:
                    nn.init.zeros_(m.bias)

    def forward(self, x):
        identity = self.skip(x)
        y = self.relu(self.conv1(x))
        y = self.conv2(y)
        y = y + identity
        return self.relu(y)


class MasteringXXL_conv2d(nn.Module):
    """V5.16 — XXL Conv2D (bench NPU INT8 = 2.77 ms, 6.3M params).

    Architecture validée par le bench sweep (cf bench_sweep_archs.py) :
      Reshape2D
      Conv2D n_in → 512   (1,5) ReLU
      Conv2D 512 → 1024   (1,5) ReLU
      Conv2D 1024 → 1024  (1,3) ReLU
      AvgPool + Flatten
      Linear 1024 → 512   ReLU
      Linear 512 → 30     Sigmoid

    AVEC skip connection autour du conv 1024→1024 pour éviter mode collapse.
    Kaiming init explicite.
    """

    def __init__(self, n_input: int = N_FEATURES_IN_V3,
                 n_out: int = N_PARAMS_OUT_V5_14):
        super().__init__()
        self.reshape = _Reshape2D()
        self.conv1 = nn.Conv2d(n_input, 512, kernel_size=(1, 5), padding=(0, 2))
        self.conv2 = nn.Conv2d(512, 1024, kernel_size=(1, 5), padding=(0, 2))
        self.conv3 = nn.Conv2d(1024, 1024, kernel_size=(1, 3), padding=(0, 1))
        self.relu  = nn.ReLU(inplace=True)
        self.pool  = nn.AdaptiveAvgPool2d((1, 1))
        self.flat  = nn.Flatten()
        self.fc1   = nn.Linear(1024, 512)
        self.fc2   = nn.Linear(512, n_out)
        self.sig   = nn.Sigmoid()
        # Kaiming init explicite (anti mode-collapse)
        for m in self.modules():
            if isinstance(m, (nn.Conv2d, nn.Linear)):
                nn.init.kaiming_normal_(m.weight, nonlinearity='relu')
                if m.bias is not None:
                    nn.init.zeros_(m.bias)

    def forward(self, x):
        x = self.reshape(x)
        x = self.relu(self.conv1(x))
        x = self.relu(self.conv2(x))
        # Skip autour du dernier conv (résiduel 1024 → 1024)
        skip = x
        x = self.conv3(x)
        x = self.relu(x + skip)
        x = self.flat(self.pool(x))
        x = self.relu(self.fc1(x))
        return self.sig(self.fc2(x))


class MasteringResNet_conv2d(nn.Module):
    """V5.16 — ResNet Conv2D pour input Mel 85 features × 10 trames.

    Skip connections + Kaiming init → évite mode collapse de v5.15.
    Conv2D mapping NPU efficace.

    Input  : (B, 85, 10)  →  Reshape (B, 85, 1, 10)
    Output : (B, 30)      (V5.14 layout, bandes EQ fixes)
    """

    def __init__(self, n_input: int = N_FEATURES_IN_V3,
                 n_out: int = N_PARAMS_OUT_V5_14):
        super().__init__()
        self.net = nn.Sequential(
            _Reshape2D(),
            # Initial conv
            nn.Conv2d(n_input, 256, kernel_size=(1, 3), padding=(0, 1)),
            nn.ReLU(inplace=True),
            # 4 blocs résiduels 256 channels
            _ResidualBlock2D(256, 256),
            _ResidualBlock2D(256, 256),
            _ResidualBlock2D(256, 256),
            _ResidualBlock2D(256, 256),
            # Pool + tête FC
            nn.AdaptiveAvgPool2d((1, 1)),
            nn.Flatten(),
            nn.Linear(256, 256),
            nn.ReLU(inplace=True),
            nn.Linear(256, n_out),
            nn.Sigmoid(),
        )
        # Kaiming init pour Linear
        for m in self.net.modules():
            if isinstance(m, nn.Linear):
                nn.init.kaiming_normal_(m.weight, nonlinearity='relu')
                if m.bias is not None:
                    nn.init.zeros_(m.bias)

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        return self.net(features)


class MasteringDEEP_conv2d(nn.Module):
    """V9.5.15 — Conv2D DEEP (8 couches × 512 channels, ~5M params).

    Bench NPU i.MX8MP : 1.66 ms/invoke (15× plus rapide que CPU).
    Conv2D mieux mappé que Conv1D (1.6× speedup à params égaux).
    8 couches profondes = meilleur pour patterns temporels mastering.

    Sortie : 30 floats (16 EQ gains + 4 exciter + 4 stereo + 6 limiter).
    Freq+Q EQ fixés (cf FIXED_EQ_FREQS_V5_14).
    """

    def __init__(self, n_input: int = N_FEATURES_IN_V2):
        super().__init__()
        self.net = nn.Sequential(
            _Reshape2D(),
            nn.Conv2d(n_input, 512, kernel_size=(1, 5), padding=(0, 2)), nn.ReLU(),
            nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
            nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
            nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
            nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
            nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
            nn.Conv2d(512, 512, kernel_size=(1, 3), padding=(0, 1)), nn.ReLU(),
            nn.AdaptiveAvgPool2d((1, 1)), nn.Flatten(),
            nn.Linear(512, 512), nn.ReLU(),
            nn.Linear(512, N_PARAMS_OUT_V5_14), nn.Sigmoid(),
        )

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        return self.net(features)


class MasteringConv1DLargeV5_14(nn.Module):
    """V9.5.14 — comme MasteringConv1DLarge mais sortie 30 (bandes EQ fixes).

    16 EQ gains + 4 exciter + 4 stereo + 6 limiter = 30 params.
    Freq + Q de l'EQ sont fixés au boot (cf FIXED_EQ_FREQS_V5_14).
    """

    def __init__(self, n_input: int = N_FEATURES_IN_V2):
        super().__init__()
        self.net = nn.Sequential(
            nn.Conv1d(n_input, 128, kernel_size=5, padding=2),
            nn.BatchNorm1d(128),
            nn.GELU(),
            nn.Conv1d(128, 256, kernel_size=5, padding=2),
            nn.BatchNorm1d(256),
            nn.GELU(),
            nn.Conv1d(256, 256, kernel_size=3, padding=1),
            nn.BatchNorm1d(256),
            nn.GELU(),
            nn.Conv1d(256, 256, kernel_size=3, padding=1),
            nn.BatchNorm1d(256),
            nn.GELU(),
            nn.AdaptiveAvgPool1d(1),
            nn.Flatten(),
            nn.Linear(256, 256),
            nn.GELU(),
            nn.Dropout(0.1),
            nn.Linear(256, N_PARAMS_OUT_V5_14),
            nn.Sigmoid(),
        )

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        return self.net(features)


class MasteringConv1DLarge(nn.Module):
    """V9.5.13 — modèle Conv1D large pour Phase 3 (capacité ×10 vs v5.12).

    Architecture :
      Conv1D 11  → 128 (kernel 5, padding=2)
      BatchNorm1d 128 + GELU
      Conv1D 128 → 256 (kernel 5, padding=2)
      BatchNorm1d 256 + GELU
      Conv1D 256 → 256 (kernel 3, padding=1)
      BatchNorm1d 256 + GELU
      Conv1D 256 → 256 (kernel 3, padding=1)
      BatchNorm1d 256 + GELU
      AdaptiveAvgPool1d → (B, 256, 1)
      Flatten
      Linear 256 → 256 + GELU + Dropout 0.1
      Linear 256 → 62
      Sigmoid

    ~500K params. Quantizable INT8 TFLite (BatchNorm fold + GELU approximé).
    """

    def __init__(self, n_input: int = N_FEATURES_IN_V2):
        super().__init__()
        self.net = nn.Sequential(
            nn.Conv1d(n_input, 128, kernel_size=5, padding=2),
            nn.BatchNorm1d(128),
            nn.GELU(),
            nn.Conv1d(128, 256, kernel_size=5, padding=2),
            nn.BatchNorm1d(256),
            nn.GELU(),
            nn.Conv1d(256, 256, kernel_size=3, padding=1),
            nn.BatchNorm1d(256),
            nn.GELU(),
            nn.Conv1d(256, 256, kernel_size=3, padding=1),
            nn.BatchNorm1d(256),
            nn.GELU(),
            nn.AdaptiveAvgPool1d(1),
            nn.Flatten(),
            nn.Linear(256, 256),
            nn.GELU(),
            nn.Dropout(0.1),
            nn.Linear(256, N_PARAMS_OUT),
            nn.Sigmoid(),
        )

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        """features : (B, N_FEATURES_IN_V2, N_frames). Returns (B, 62)."""
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


# ============================================================================
# V5.19 — enveloppe spectrale fine (remplace l'EQ paramétrique 16 bandes)
# ============================================================================
N_PARAMS_OUT_V5_19 = 74     # 64 enveloppe + 4 exciter + 6 limiter

PARAM_LAYOUT_V5_19 = {
    'env.gains_db':         slice(0, 64),    # 64 bandes log 20 Hz - 20 kHz
    'exciter.amount':       64,
    'exciter.drive':        65,
    'exciter.freq_hz':      66,
    'exciter.ceiling':      67,
    'limiter.threshold_db': 68,
    'limiter.ceiling_lin':  69,
    'limiter.attack_ms':    70,
    'limiter.release_ms':   71,
    'limiter.input_db':     72,
    'limiter.output_db':    73,
}

ENV_RANGE_DB_V5_19 = 12.0    # enveloppe ∈ [-12, +12] dB


def denormalize_params_v5_19(params_norm: torch.Tensor) -> dict:
    """74 outputs [0,1] → dict {env, exciter, limiter}. Mono par canal.
    env.gains_db : (B, 64) ∈ [-12, +12] dB.
    Ranges exciter/limiter : PARAM_RANGES_V5_17 (input_db 0..18)."""
    has_batch = params_norm.dim() == 2
    if not has_batch:
        params_norm = params_norm.unsqueeze(0)

    def dk(key, idx):
        mn, mx = PARAM_RANGES_V5_17[key]
        return params_norm[:, idx] * (mx - mn) + mn

    out = {
        'env': {
            'gains_db': (params_norm[:, PARAM_LAYOUT_V5_19['env.gains_db']] * 2.0
                          - 1.0) * ENV_RANGE_DB_V5_19,
        },
        'exciter': {
            'amount':  dk('exciter.amount',  PARAM_LAYOUT_V5_19['exciter.amount']),
            'drive':   dk('exciter.drive',   PARAM_LAYOUT_V5_19['exciter.drive']),
            'freq_hz': dk('exciter.freq_hz', PARAM_LAYOUT_V5_19['exciter.freq_hz']),
            'ceiling': dk('exciter.ceiling', PARAM_LAYOUT_V5_19['exciter.ceiling']),
        },
        'limiter': {
            'threshold_db': dk('limiter.threshold_db', PARAM_LAYOUT_V5_19['limiter.threshold_db']),
            'ceiling_lin':  dk('limiter.ceiling_lin',  PARAM_LAYOUT_V5_19['limiter.ceiling_lin']),
            'attack_ms':    dk('limiter.attack_ms',    PARAM_LAYOUT_V5_19['limiter.attack_ms']),
            'release_ms':   dk('limiter.release_ms',   PARAM_LAYOUT_V5_19['limiter.release_ms']),
            'input_db':     dk('limiter.input_db',     PARAM_LAYOUT_V5_19['limiter.input_db']),
            'output_db':    dk('limiter.output_db',    PARAM_LAYOUT_V5_19['limiter.output_db']),
        },
    }
    if not has_batch:
        for k1, v1 in out.items():
            for k2, v2 in v1.items():
                out[k1][k2] = v2.squeeze(0)
    return out
