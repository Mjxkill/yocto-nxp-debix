#!/usr/bin/env python3
"""
npu_master_loop.py — V3.2.2 J5 stub : NPU mastering loop skeleton.

Reads post-effets audio from /dev/imx-audio-tap (mmap shared memory written
by the SOF DSP firmware), computes FFT + master-bus features at ~10 Hz,
prints them to console.

This is the SKELETON of the J5/Phase 4 NPU mastering loop. The pieces
present :

  ✓ mmap /dev/imx-audio-tap, validate magic, read header (R6 ring_size)
  ✓ R4 seqcount-style read (epoch1 → write_idx → data → epoch2 → retry)
  ✓ R1 epoch transition handling (DSP reset → resync read_idx)
  ✓ Window buffering (2048 samples = 42.67 ms @ 48kHz)
  ✓ FFT (numpy.fft) + log-spaced bands
  ✓ Features : per-band loudness (8 bands), L/R balance, crest, stereo corr
  ✓ amixer-based ALSA control read (proves output side works)

Pieces NOT yet present (future work) :

  ✗ TFLite model + dataset training (Phase 4 dataset gen)
  ✗ NPU inference via eIQ runtime
  ✗ Closed-loop controls : write multiband_drc + volume + limiter via amixer

Usage :
    npu_master_loop.py [--device /dev/imx-audio-tap]
                       [--window 2048]
                       [--rate 10]
                       [--bands 8]
                       [--time 0]   # 0 = until SIGINT
"""
from __future__ import annotations

import argparse
import ctypes
import errno
import mmap
import os
import signal
import struct
import subprocess
import sys
import time
from dataclasses import dataclass

import numpy as np

NPU_TAP_DEV          = "/dev/imx-audio-tap"
NPU_TAP_MAGIC        = 0x5441504E   # "NPAT"
NPU_TAP_RING_SIZE    = 0x40000      # 256 KB total
NPU_TAP_HDR_SIZE     = 128
NPU_TAP_HDR_FMT      = "<11I84x"    # 11 uint32 (44 B) + 84 padding = 128 B
NPU_TAP_HDR_FIELDS   = (
    "magic", "version", "ring_size", "hdr_size",
    "epoch", "write_idx", "read_idx",
    "period_bytes", "sample_rate", "channels", "frame_fmt",
)


@dataclass
class TapHeader:
    magic: int
    version: int
    ring_size: int
    hdr_size: int
    epoch: int
    write_idx: int
    read_idx: int
    period_bytes: int
    sample_rate: int
    channels: int
    frame_fmt: int


class NpuTapReader:
    """R4 seqcount-style reader of the SOF DSP NPU tap shared ring."""

    def __init__(self, dev: str = NPU_TAP_DEV) -> None:
        self.fd = os.open(dev, os.O_RDONLY)
        self.region = mmap.mmap(
            self.fd, NPU_TAP_RING_SIZE,
            mmap.MAP_SHARED, mmap.PROT_READ,
        )
        # Wait for firmware to publish a valid header (M5 magic handshake).
        for attempt in range(50):  # 5 s max
            hdr = self._read_header_locked()
            if hdr.magic == NPU_TAP_MAGIC:
                break
            if attempt == 0:
                print(f"npu_master: waiting for firmware "
                      f"(magic=0x{hdr.magic:x}), aplay if needed...",
                      file=sys.stderr)
            time.sleep(0.1)
        else:
            raise RuntimeError(
                f"timed out waiting for NPU_TAP_MAGIC (got 0x{hdr.magic:x})"
            )
        self.hdr = hdr
        self.local_read = hdr.write_idx
        self.last_epoch = hdr.epoch
        print(f"npu_master: header OK — version={hdr.version} "
              f"ring_size={hdr.ring_size} period={hdr.period_bytes} "
              f"rate={hdr.sample_rate} ch={hdr.channels}", file=sys.stderr)

    def _read_header_locked(self) -> TapHeader:
        raw = bytes(self.region[:NPU_TAP_HDR_SIZE])
        vals = struct.unpack(NPU_TAP_HDR_FMT, raw)
        return TapHeader(*vals)

    def _atomic_u32(self, offset: int) -> int:
        # Python's mmap reads aren't atomic at the C level, but a single 32-bit
        # word read from an aligned mmap is effectively atomic on AArch64
        # because the underlying load is a single instruction. For ring buffer
        # producer/consumer indices we just need eventual visibility, which the
        # write-combine mapping + DSP memw on the producer side provide.
        b = bytes(self.region[offset:offset + 4])
        return struct.unpack("<I", b)[0]

    def _read_data(self, src_off: int, length: int) -> bytes:
        ring_size = self.hdr.ring_size
        data_base = NPU_TAP_HDR_SIZE
        end_off = data_base + src_off + length
        ring_end = data_base + ring_size
        if end_off <= ring_end:
            return bytes(self.region[data_base + src_off:data_base + src_off + length])
        first = ring_end - (data_base + src_off)
        return (
            bytes(self.region[data_base + src_off:ring_end])
            + bytes(self.region[data_base:data_base + (length - first)])
        )

    def read_frames(self, n_frames: int) -> tuple[np.ndarray, int, int]:
        """
        Block until n_frames available, return (array (channels, n_frames) int32,
        epoch_resets_observed, race_retries_observed).
        Numpy returns S32 normalized to [-1, 1] as float32.
        """
        ch = self.hdr.channels
        bytes_needed = n_frames * ch * 4
        ring_size = self.hdr.ring_size
        epoch_resets = 0
        race_retries = 0
        # Offset where epoch and write_idx live in the header
        epoch_off = struct.calcsize("<4I")          # offset = 16
        write_idx_off = epoch_off + 4               # offset = 20

        while True:
            # R4 seqcount read
            e1 = self._atomic_u32(epoch_off)
            if e1 != self.last_epoch:
                # DSP reset — discard local state and resync
                self.last_epoch = e1
                self.local_read = self._atomic_u32(write_idx_off)
                epoch_resets += 1
                print(f"npu_master: epoch reset → {e1}, resync read_idx",
                      file=sys.stderr)
                continue

            w = self._atomic_u32(write_idx_off)
            avail = (w - self.local_read) % ring_size
            if avail < bytes_needed:
                time.sleep(0.002)   # one period
                continue

            # Read the requested chunk
            raw = self._read_data(self.local_read, bytes_needed)

            # Re-check epoch
            e2 = self._atomic_u32(epoch_off)
            if e2 != e1:
                race_retries += 1
                self.last_epoch = e2
                self.local_read = self._atomic_u32(write_idx_off)
                continue

            # Commit
            self.local_read = (self.local_read + bytes_needed) % ring_size
            arr = np.frombuffer(raw, dtype="<i4").reshape(n_frames, ch).T
            # int32 → float32 in [-1, 1]
            audio = arr.astype(np.float32) / 2_147_483_648.0
            return audio, epoch_resets, race_retries

    def close(self) -> None:
        self.region.close()
        os.close(self.fd)


class FeatureExtractor:
    """Compute master-bus features from a stereo audio window."""

    def __init__(self, sample_rate: int, n_bands: int = 8) -> None:
        self.sr = sample_rate
        self.n_bands = n_bands
        # Log-spaced band edges from 60 Hz to 16 kHz
        self.band_edges = np.geomspace(60, 16000, n_bands + 1)

    def compute(self, audio: np.ndarray) -> dict:
        """audio shape : (channels, n_frames). Uses ch0=L, ch1=R for master."""
        n_frames = audio.shape[1]
        L = audio[0]
        R = audio[1] if audio.shape[0] > 1 else audio[0]

        # Per-channel loudness (RMS dBFS)
        rms_l = np.sqrt(np.mean(L ** 2) + 1e-12)
        rms_r = np.sqrt(np.mean(R ** 2) + 1e-12)
        rms_m = np.sqrt(np.mean(((L + R) * 0.5) ** 2) + 1e-12)

        # Balance : ratio L/(L+R) energy
        bal = rms_l ** 2 / (rms_l ** 2 + rms_r ** 2 + 1e-12)

        # Crest factor (peak/RMS of mono mix)
        peak_m = max(np.max(np.abs(L)), np.max(np.abs(R)), 1e-12)
        crest_db = 20 * np.log10(peak_m / max(rms_m, 1e-12))

        # Stereo correlation (Pearson)
        if rms_l > 1e-9 and rms_r > 1e-9:
            stereo_corr = float(np.corrcoef(L, R)[0, 1])
        else:
            stereo_corr = 0.0

        # FFT band loudness (mono)
        mono = (L + R) * 0.5
        win = np.hanning(n_frames)
        spec = np.fft.rfft(mono * win)
        mag = np.abs(spec) ** 2
        freqs = np.fft.rfftfreq(n_frames, 1.0 / self.sr)
        bands_db = np.empty(self.n_bands, dtype=np.float32)
        for i in range(self.n_bands):
            lo, hi = self.band_edges[i], self.band_edges[i + 1]
            mask = (freqs >= lo) & (freqs < hi)
            e = float(mag[mask].sum())
            bands_db[i] = 10 * np.log10(e + 1e-12)

        return {
            "rms_l_db": 20 * np.log10(rms_l + 1e-12),
            "rms_r_db": 20 * np.log10(rms_r + 1e-12),
            "rms_m_db": 20 * np.log10(rms_m + 1e-12),
            "balance": float(bal),
            "crest_db": float(crest_db),
            "stereo_corr": stereo_corr,
            "bands_db": bands_db,
            "band_edges": self.band_edges,
        }


def find_master_volume_control() -> str | None:
    """Probe SOF amixer for a Master Volume control on softac5212tdm."""
    try:
        out = subprocess.run(
            ["amixer", "-c", "softac5212tdm", "controls"],
            capture_output=True, check=True, text=True,
        ).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None
    for line in out.splitlines():
        if "Master Playback Volume" in line:
            # Format: numid=N,iface=MIXER,name='...'
            for part in line.split(","):
                if part.startswith("name="):
                    return part[5:].strip("'")
    return None


def read_alsa_volume(name: str) -> str:
    try:
        out = subprocess.run(
            ["amixer", "-c", "softac5212tdm", "cget", f"name={name}"],
            capture_output=True, text=True, check=True,
        ).stdout
    except subprocess.CalledProcessError as e:
        return f"<error: {e}>"
    for line in out.splitlines():
        if line.strip().startswith(": values="):
            return line.strip().split("=", 1)[1]
    return "<not found>"


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--device", default=NPU_TAP_DEV)
    p.add_argument("--window", type=int, default=2048,
                   help="FFT window in samples (default 2048 = 42.67 ms @ 48k)")
    p.add_argument("--rate", type=float, default=10.0,
                   help="Print rate in Hz (default 10)")
    p.add_argument("--bands", type=int, default=8)
    p.add_argument("--time", type=float, default=0.0,
                   help="Run duration in seconds (0 = until SIGINT)")
    p.add_argument("--show-controls", action="store_true",
                   help="Print SOF ALSA controls available and exit")
    args = p.parse_args()

    if args.show_controls:
        try:
            out = subprocess.run(
                ["amixer", "-c", "softac5212tdm", "controls"],
                capture_output=True, text=True, check=True,
            ).stdout
            print(out)
        except subprocess.CalledProcessError as e:
            print(f"amixer error: {e}", file=sys.stderr)
            return 1
        return 0

    running = [True]
    def _stop(_sig, _frm): running[0] = False
    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    reader = NpuTapReader(args.device)
    extractor = FeatureExtractor(reader.hdr.sample_rate, args.bands)
    master_ctl = find_master_volume_control()
    if master_ctl:
        print(f"npu_master: ALSA Master Playback Volume = '{master_ctl}'",
              file=sys.stderr)
    else:
        print("npu_master: no Master Playback Volume control found", file=sys.stderr)

    period = 1.0 / args.rate
    t_start = time.monotonic()
    next_print = t_start
    total_resets = 0
    total_races = 0
    n_iter = 0

    while running[0]:
        if args.time > 0 and (time.monotonic() - t_start) >= args.time:
            break

        audio, resets, races = reader.read_frames(args.window)
        total_resets += resets
        total_races += races

        feats = extractor.compute(audio)
        n_iter += 1

        now = time.monotonic()
        if now >= next_print:
            bands_str = " ".join(f"{b:+5.1f}" for b in feats["bands_db"])
            mvol = read_alsa_volume(master_ctl) if master_ctl else "n/a"
            print(
                f"[{now - t_start:6.1f}s] "
                f"L={feats['rms_l_db']:+6.2f}dB R={feats['rms_r_db']:+6.2f}dB "
                f"M={feats['rms_m_db']:+6.2f}dB "
                f"bal={feats['balance']:.3f} crest={feats['crest_db']:5.1f}dB "
                f"corr={feats['stereo_corr']:+.3f}  "
                f"bands[{bands_str}]  "
                f"vol={mvol}",
                flush=True,
            )
            next_print = now + period

    duration = time.monotonic() - t_start
    print(
        f"\nnpu_master: stopped after {duration:.2f}s, {n_iter} windows, "
        f"epoch_resets={total_resets} race_retries={total_races}",
        file=sys.stderr,
    )
    reader.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
