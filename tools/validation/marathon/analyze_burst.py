#!/usr/bin/env python3
# Rejoue EXACTEMENT l'algorithme d'anti-larsen.c sur un dump du tap et
# imprime, pour la bande 2300-2500 Hz, quel critère passe/échoue.
import numpy as np, sys

raw = np.fromfile(sys.argv[1], dtype=np.int32).reshape(-1, 8)
FS, NFFT = 48000, 8192
NBINS = NFFT // 2 + 1
HZPB = FS / NFFT
THRESH, PNR, F0 = -45.0, 25.0, 2400.0
win = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(NFFT) / (NFFT - 1))

nseg = len(raw) // NFFT
print(f"{len(raw)} frames -> {nseg} segments FFT")
for seg in range(nseg):
    x = raw[seg * NFFT:(seg + 1) * NFFT, 0].astype(np.float64) / 2**31
    rms_db = 10 * np.log10(np.mean(x[::16] ** 2) + 1e-24)
    X = np.fft.rfft(x * win)
    sp = 10 * np.log10((np.abs(X) ** 2) / (NFFT * NFFT / 16.0) + 1e-24)
    b0 = int(round(F0 / HZPB))
    zone = range(b0 - 4, b0 + 5)
    bmax = max(zone, key=lambda b: sp[b])
    # critères du daemon
    gate = rms_db > THRESH - 10
    c_thresh = sp[bmax] > THRESH
    c_locmax = sp[bmax] >= sp[bmax - 1] and sp[bmax] >= sp[bmax + 1]
    d = np.concatenate([sp[bmax - 10:bmax - 1], sp[bmax + 2:bmax + 11]])
    pnr = sp[bmax] - d.mean()
    c_pnr = pnr >= PNR
    musical = False
    for h in (2, 3):
        hb = bmax * h
        for dd in range(-2, 3):
            if hb + dd < NBINS and sp[hb + dd] > sp[bmax] - 12.0:
                musical = True
    print(f"seg{seg}: rms={rms_db:5.1f} gate={int(gate)} "
          f"bin={bmax} ({bmax*HZPB:.0f}Hz) sp={sp[bmax]:6.1f} "
          f"thresh={int(c_thresh)} locmax={int(c_locmax)} "
          f"pnr={pnr:5.1f} ({int(c_pnr)}) musical={int(musical)}")
    # top 5 raies candidates toutes bandes (pour voir la concurrence)
    if seg == nseg - 1:
        hits = []
        for b in range(8, NBINS - 8):
            if sp[b] < THRESH or sp[b] < sp[b-1] or sp[b] < sp[b+1]:
                continue
            dd = np.concatenate([sp[b-10:b-1], sp[b+2:b+11]])
            if sp[b] - dd.mean() < PNR: continue
            hits.append((b, sp[b]))
        print("hits qualifiés (bin, dB):",
              [(b, round(s,1), f"{b*HZPB:.0f}Hz") for b, s in hits[:16]])
