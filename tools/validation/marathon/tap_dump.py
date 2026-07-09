#!/usr/bin/env python3
# Dump N secondes de frames brutes du tap FX vers un fichier int32
# (8 canaux entrelacés). Usage: tap_dump.py <out.raw> [secondes]
import mmap, struct, sys, time

DEV = "/dev/imx-audio-tap-out"
TOTAL, HDR, MAGIC, NCHAN = 0x40000, 128, 0x5441504E, 8
out = open(sys.argv[1], "wb")
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0

with open(DEV, "rb") as f:
    m = mmap.mmap(f.fileno(), TOTAL, prot=mmap.PROT_READ)
assert struct.unpack_from("<I", m, 0)[0] == MAGIC
ring = struct.unpack_from("<I", m, 8)[0]
fsz = NCHAN * 4
rd = struct.unpack_from("<I", m, 20)[0]
t0 = time.time()
n = 0
while time.time() - t0 < dur:
    wr = struct.unpack_from("<I", m, 20)[0]
    avail = (wr - rd) % ring
    k = avail // fsz
    for i in range(k):
        off = HDR + (rd + i * fsz) % ring
        out.write(m[off:off + fsz])
        n += 1
    rd = (rd + k * fsz) % ring
    time.sleep(0.02)
out.close()
print(f"dump {n} frames")
