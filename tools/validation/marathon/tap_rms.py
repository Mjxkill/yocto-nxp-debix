#!/usr/bin/env python3
# Sonde TAP FX (/dev/imx-audio-tap-out) : mmap du ring (magic 'NPAT'),
# lit ~0.2 s de frames 8ch S32 et imprime le RMS dBFS par canal 0/1.
import mmap, struct, time, math, sys

import sys as _s; DEV = _s.argv[1] if len(_s.argv)>1 else "/dev/imx-audio-tap-out"
TOTAL, HDR, MAGIC, NCHAN = 0x40000, 128, 0x5441504E, 8

with open(DEV, "rb") as f:
    m = mmap.mmap(f.fileno(), TOTAL, prot=mmap.PROT_READ)

if struct.unpack_from("<I", m, 0)[0] != MAGIC:
    print("no-magic"); sys.exit(0)
ring = struct.unpack_from("<I", m, 8)[0]
fsz = NCHAN * 4
rd = struct.unpack_from("<I", m, 20)[0]     # départ = position actuelle
acc = [0.0]*8; pk = [0]*8; n = 0
t0 = time.time()
while time.time() - t0 < 0.25:
    wr = struct.unpack_from("<I", m, 20)[0]
    avail = (wr - rd) % ring
    k = avail // fsz
    for i in range(k):
        off = HDR + (rd + i * fsz) % ring
        fr = struct.unpack_from("<8i", m, off)
        for c in range(8):
            acc[c] += (fr[c] / 2147483648.0) ** 2
            if abs(fr[c]) > pk[c]: pk[c] = abs(fr[c])
        n += 1
    rd = (rd + k * fsz) % ring
    time.sleep(0.02)
if n == 0:
    print("rms=silence n=0")
else:
    db = ["%.0f" % (10 * math.log10(a / n + 1e-24)) for a in acc]
    pkdb = ["%.0f" % (20 * math.log10(p / 2147483648.0 + 1e-12)) for p in pk]
    print("rms:", " ".join(db))
    print("pk :", " ".join(pkdb), f"frames={n} dev={DEV}")
