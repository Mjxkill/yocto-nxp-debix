#!/usr/bin/env python3
# V11-AL E0(a) — lit le tap FX (/dev/imx-audio-tap-out, play post-effets
# 8ch S32) et affiche les pics spectraux par canal.
# Usage : al_tap_fft.py [duree_max_s]
import mmap, os, struct, sys, time
import numpy as np

DEV = "/dev/imx-audio-tap-out"
RING_TOTAL = 0x40000        # 256 KB (header + data)
HDR = 128
MAGIC = 0x5441504E
FS = 48000
NCH = 8
NFFT = 8192

fd = os.open(DEV, os.O_RDONLY)
mm = mmap.mmap(fd, RING_TOTAL, mmap.MAP_SHARED, mmap.PROT_READ)
magic, version, ring_size, hdr_size = struct.unpack_from("<IIII", mm, 0)
epoch, wr = struct.unpack_from("<II", mm, 16)
print(f"magic={magic:#x} v{version} ring={ring_size} hdr={hdr_size} epoch={epoch}")
if magic != MAGIC:
    sys.exit("magic invalide — DSP pas en streaming ?")

dur = float(sys.argv[1]) if len(sys.argv) > 1 else 4.0
fsz = NCH * 4
buf = np.zeros((NFFT, NCH), dtype=np.float64)
got = 0
rd = struct.unpack_from("<I", mm, 20)[0]   # partir du write_idx courant
t0 = time.time()
while got < NFFT and time.time() - t0 < dur:
    wr = struct.unpack_from("<I", mm, 20)[0]
    avail = (wr - rd) % ring_size
    n = min(avail // fsz, NFFT - got)
    for i in range(n):
        off = HDR + (rd + i * fsz) % ring_size
        buf[got + i] = np.array(struct.unpack_from("<8i", mm, off)) / 2147483648.0
    rd = (rd + n * fsz) % ring_size
    got += n
    if n == 0:
        time.sleep(0.01)
print(f"frames lues : {got}")
if got < NFFT:
    sys.exit("pas assez de données (audio joue ?)")

win = np.hanning(NFFT)
for ch in range(NCH):
    x = buf[:, ch]
    rms = np.sqrt(np.mean(x * x))
    if rms < 1e-6:
        print(f"ch{ch}: silence")
        continue
    sp = np.abs(np.fft.rfft(x * win))
    sp_db = 20 * np.log10(sp / NFFT * 4 + 1e-12)
    order = np.argsort(sp_db)[::-1]
    picks, taken = [], []
    for p in order:
        if any(abs(p - t) < 6 for t in taken):
            continue
        taken.append(p); picks.append(p)
        if len(picks) == 5:
            break
    desc = ", ".join(f"{p*FS/NFFT:.0f}Hz {sp_db[p]:.0f}dB" for p in picks)
    print(f"ch{ch}: rms={20*np.log10(rms+1e-12):.1f}dBFS  pics: {desc}")
