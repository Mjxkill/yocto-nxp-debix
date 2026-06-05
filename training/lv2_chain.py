"""
V9.5 — Python wrapper ctypes pour lv2_chain_host.so.

Reproduit l'API mixer-pro côté board pour le training PC.

Usage minimal :

    chain = Chain(sr=48000, block=96)
    chain.add('http://lsp-plug.in/plugins/lv2/para_equalizer_x16_stereo')
    chain.add('http://calf.sourceforge.net/plugins/Exciter')
    chain.add('http://calf.sourceforge.net/plugins/StereoTools')
    chain.add('http://lsp-plug.in/plugins/lv2/limiter_stereo')

    # Process stéréo (in_l, in_r) → (out_l, out_r), block_size = 96
    out_l, out_r = chain.process(in_l, in_r)

    # Inspect params slot 0 (Para EQ x16)
    for p, name, mn, mx, df in chain.params(0):
        print(name, mn, mx, df)

    chain.set_param(0, 'g_0', 0.25)   # gain band 0 = -12 dB
"""

import ctypes
import os
import numpy as np

_LIB_PATH = os.path.join(os.path.dirname(__file__), 'lv2_chain_host.so')


class _Lib:
    def __init__(self, path):
        if not os.path.exists(path):
            raise RuntimeError(
                f"{path} introuvable. Compile-le :\n"
                "  gcc -O2 -fPIC -shared -o lv2_chain_host.so lv2_chain_host.c "
                "$(pkg-config --cflags --libs lilv-0)"
            )
        lib = ctypes.CDLL(path)
        lib.chain_create.restype = ctypes.c_void_p
        lib.chain_create.argtypes = [ctypes.c_float, ctypes.c_uint32]

        lib.chain_add_plugin.restype = ctypes.c_int
        lib.chain_add_plugin.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

        lib.chain_slot_n_params.restype = ctypes.c_int
        lib.chain_slot_n_params.argtypes = [ctypes.c_void_p, ctypes.c_int]

        lib.chain_slot_param_name.restype = ctypes.c_int
        lib.chain_slot_param_name.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
            ctypes.c_char_p, ctypes.c_int,
        ]

        lib.chain_slot_param_range.restype = ctypes.c_int
        lib.chain_slot_param_range.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
            ctypes.POINTER(ctypes.c_float),
        ]

        lib.chain_set_param.restype = ctypes.c_int
        lib.chain_set_param.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_float
        ]

        lib.chain_set_param_by_name.restype = ctypes.c_int
        lib.chain_set_param_by_name.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_float
        ]

        lib.chain_process.restype = ctypes.c_int
        lib.chain_process.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
            ctypes.c_uint32,
        ]

        lib.chain_free.restype = None
        lib.chain_free.argtypes = [ctypes.c_void_p]
        self.lib = lib


_L = None


def _lib():
    global _L
    if _L is None:
        _L = _Lib(_LIB_PATH)
    return _L.lib


class Chain:
    def __init__(self, sr: float = 48000.0, block: int = 96):
        self.sr = sr
        self.block = block
        self._h = _lib().chain_create(ctypes.c_float(sr), ctypes.c_uint32(block))
        if not self._h:
            raise RuntimeError("chain_create failed")
        self._n_slots = 0

    def __del__(self):
        if getattr(self, '_h', None):
            _lib().chain_free(self._h)
            self._h = None

    def add(self, uri: str) -> int:
        slot = _lib().chain_add_plugin(self._h, uri.encode('utf-8'))
        if slot < 0:
            raise RuntimeError(f"add_plugin failed for {uri}")
        self._n_slots = slot + 1
        return slot

    @property
    def n_slots(self) -> int:
        return self._n_slots

    def params(self, slot: int):
        """Yield (idx, name, min, max, default) for each param of a slot."""
        n = _lib().chain_slot_n_params(self._h, slot)
        if n < 0:
            raise IndexError(slot)
        name_buf = ctypes.create_string_buffer(64)
        mmd = (ctypes.c_float * 3)()
        for p in range(n):
            _lib().chain_slot_param_name(self._h, slot, p, name_buf, 64)
            _lib().chain_slot_param_range(self._h, slot, p, mmd)
            yield p, name_buf.value.decode('utf-8'), mmd[0], mmd[1], mmd[2]

    def set_param(self, slot: int, name_or_idx, value: float):
        if isinstance(name_or_idx, int):
            rc = _lib().chain_set_param(self._h, slot, name_or_idx,
                                        ctypes.c_float(value))
        else:
            rc = _lib().chain_set_param_by_name(self._h, slot,
                                                name_or_idx.encode('utf-8'),
                                                ctypes.c_float(value))
        if rc < 0:
            raise RuntimeError(f"set_param failed slot={slot} param={name_or_idx}")

    def process(self, in_l: np.ndarray, in_r: np.ndarray):
        """Process float32 mono arrays in_l, in_r (length self.block).
        Returns out_l, out_r as np.ndarray float32."""
        N = self.block
        if len(in_l) != N or len(in_r) != N:
            raise ValueError(f"expected length {N}, got {len(in_l)},{len(in_r)}")
        in_l = np.ascontiguousarray(in_l, dtype=np.float32)
        in_r = np.ascontiguousarray(in_r, dtype=np.float32)
        out_l = np.zeros(N, dtype=np.float32)
        out_r = np.zeros(N, dtype=np.float32)
        rc = _lib().chain_process(
            self._h,
            in_l.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
            in_r.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
            out_l.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
            out_r.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
            ctypes.c_uint32(N),
        )
        if rc < 0:
            raise RuntimeError("chain_process failed")
        return out_l, out_r

    def process_wav(self, audio_l: np.ndarray, audio_r: np.ndarray):
        """Process an entire stereo audio array by chunking into blocks."""
        N = self.block
        n_blocks = (len(audio_l) + N - 1) // N
        out_l = np.zeros_like(audio_l)
        out_r = np.zeros_like(audio_r)
        for b in range(n_blocks):
            i0 = b * N
            i1 = min(i0 + N, len(audio_l))
            chunk_l = np.zeros(N, dtype=np.float32)
            chunk_r = np.zeros(N, dtype=np.float32)
            chunk_l[: i1 - i0] = audio_l[i0:i1]
            chunk_r[: i1 - i0] = audio_r[i0:i1]
            o_l, o_r = self.process(chunk_l, chunk_r)
            out_l[i0:i1] = o_l[: i1 - i0]
            out_r[i0:i1] = o_r[: i1 - i0]
        return out_l, out_r
