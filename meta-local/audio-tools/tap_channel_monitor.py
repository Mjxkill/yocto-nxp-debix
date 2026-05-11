#!/usr/bin/env python3
"""
tap_channel_monitor.py — V7.0 diag : qui chante côté DSP ?

Lit /dev/imx-audio-tap-out (le buffer post-pga écrit par le SOF firmware
juste avant l'envoi vers le SAI TX) et affiche en temps réel l'énergie
RMS par canal + le ou les canaux actifs.

But : isoler une désync TDM playback. Si mixer-pro envoie sur ch_i et
ce script affiche "active ch=[i]", le DSP routing est OK ; un éventuel
décalage entendu sur les speakers physiques est alors aval (SAI/DMA),
pas DSP.

Usage :
    tap_channel_monitor.py [--device /dev/imx-audio-tap-out]
                           [--window 4800]   # frames par fenêtre (100 ms @ 48k)
                           [--rate 5]        # prints par seconde
                           [--thr-db -50]    # seuil d'activité (par défaut -50 dBFS)
                           [--time 0]
"""
from __future__ import annotations

import argparse
import mmap
import os
import signal
import struct
import sys
import time
from dataclasses import dataclass

import numpy as np

NPU_TAP_DEFAULT_DEV  = "/dev/imx-audio-tap-out"
NPU_TAP_MAGIC        = 0x5441504E   # "NPAT"
NPU_TAP_RING_SIZE    = 0x40000      # 256 KB
NPU_TAP_HDR_SIZE     = 128
NPU_TAP_HDR_FMT      = "<11I84x"
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
    """R4 seqcount-style reader (copie minimale de npu_master_loop.py)."""

    def __init__(self, dev: str) -> None:
        self.fd = os.open(dev, os.O_RDONLY)
        self.region = mmap.mmap(
            self.fd, NPU_TAP_RING_SIZE,
            mmap.MAP_SHARED, mmap.PROT_READ,
        )
        for attempt in range(50):
            hdr = self._read_header_locked()
            if hdr.magic == NPU_TAP_MAGIC:
                break
            if attempt == 0:
                print(f"tap_channel_monitor: waiting for firmware "
                      f"(magic=0x{hdr.magic:x}) — start mixer-pro / aplay first",
                      file=sys.stderr)
            time.sleep(0.1)
        else:
            raise RuntimeError(
                f"timed out waiting for NPU_TAP_MAGIC (got 0x{hdr.magic:x})"
            )
        self.hdr = hdr
        self.local_read = hdr.write_idx
        self.last_epoch = hdr.epoch
        print(f"tap_channel_monitor: header OK — version={hdr.version} "
              f"ring_size={hdr.ring_size} period_bytes={hdr.period_bytes} "
              f"rate={hdr.sample_rate} ch={hdr.channels}",
              file=sys.stderr)

    def _read_header_locked(self) -> TapHeader:
        raw = bytes(self.region[:NPU_TAP_HDR_SIZE])
        return TapHeader(*struct.unpack(NPU_TAP_HDR_FMT, raw))

    def _atomic_u32(self, offset: int) -> int:
        return struct.unpack("<I", bytes(self.region[offset:offset + 4]))[0]

    def _read_data(self, src_off: int, length: int) -> bytes:
        ring_size = self.hdr.ring_size
        base = NPU_TAP_HDR_SIZE
        end_off = base + src_off + length
        ring_end = base + ring_size
        if end_off <= ring_end:
            return bytes(self.region[base + src_off:base + src_off + length])
        first = ring_end - (base + src_off)
        return (
            bytes(self.region[base + src_off:ring_end])
            + bytes(self.region[base:base + (length - first)])
        )

    def read_frames(self, n_frames: int) -> tuple[np.ndarray, int, int]:
        ch = self.hdr.channels
        bytes_needed = n_frames * ch * 4
        ring_size = self.hdr.ring_size
        epoch_resets = 0
        race_retries = 0
        epoch_off = struct.calcsize("<4I")   # 16
        write_idx_off = epoch_off + 4         # 20

        while True:
            e1 = self._atomic_u32(epoch_off)
            if e1 != self.last_epoch:
                self.last_epoch = e1
                self.local_read = self._atomic_u32(write_idx_off)
                epoch_resets += 1
                print(f"tap_channel_monitor: epoch reset → {e1}",
                      file=sys.stderr)
                continue

            w = self._atomic_u32(write_idx_off)
            avail = (w - self.local_read) % ring_size
            if avail < bytes_needed:
                time.sleep(0.002)
                continue

            raw = self._read_data(self.local_read, bytes_needed)

            e2 = self._atomic_u32(epoch_off)
            if e2 != e1:
                race_retries += 1
                self.last_epoch = e2
                self.local_read = self._atomic_u32(write_idx_off)
                continue

            self.local_read = (self.local_read + bytes_needed) % ring_size
            arr = np.frombuffer(raw, dtype="<i4").reshape(n_frames, ch).T
            audio = arr.astype(np.float32) / 2_147_483_648.0
            return audio, epoch_resets, race_retries

    def close(self) -> None:
        self.region.close()
        os.close(self.fd)


def per_channel_rms_db(audio: np.ndarray) -> np.ndarray:
    """audio shape (channels, n_frames) → RMS dBFS par canal."""
    rms = np.sqrt(np.mean(audio.astype(np.float64) ** 2, axis=1) + 1e-24)
    return 20.0 * np.log10(rms + 1e-12)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--device", default=NPU_TAP_DEFAULT_DEV)
    p.add_argument("--window", type=int, default=4800,
                   help="frames par fenêtre (4800 = 100 ms @ 48k)")
    p.add_argument("--rate", type=float, default=5.0,
                   help="prints par seconde")
    p.add_argument("--thr-db", type=float, default=-50.0,
                   help="seuil dBFS pour considérer un canal actif")
    p.add_argument("--time", type=float, default=0.0,
                   help="durée totale en secondes (0 = jusqu'à SIGINT)")
    args = p.parse_args()

    running = [True]
    def _stop(_sig, _frm): running[0] = False
    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    reader = NpuTapReader(args.device)
    nch = reader.hdr.channels
    if nch <= 0 or nch > 16:
        print(f"tap_channel_monitor: refusing nch={nch}", file=sys.stderr)
        return 1

    period = 1.0 / args.rate
    t_start = time.monotonic()
    next_print = t_start
    total_resets = 0
    total_races = 0

    print(f"# t(s)  | per-channel RMS dBFS (ch0..ch{nch-1})"
          f"                                              | active",
          flush=True)

    while running[0]:
        if args.time > 0 and (time.monotonic() - t_start) >= args.time:
            break

        audio, resets, races = reader.read_frames(args.window)
        total_resets += resets
        total_races += races

        db = per_channel_rms_db(audio)
        active = [i for i, v in enumerate(db) if v >= args.thr_db]

        now = time.monotonic()
        if now >= next_print:
            db_str = " ".join(f"{v:+6.1f}" for v in db)
            print(f"{now - t_start:7.2f} | {db_str} | {active}", flush=True)
            next_print = now + period

    duration = time.monotonic() - t_start
    print(
        f"\ntap_channel_monitor: stopped after {duration:.2f}s, "
        f"epoch_resets={total_resets} race_retries={total_races}",
        file=sys.stderr,
    )
    reader.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
