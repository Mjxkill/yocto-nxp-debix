#!/usr/bin/env python3
# V11-AL E0(b)(c) — pose/retire un notch RBJ sur un biquad DAC du TAC.
# Port bit-exact du rbjBlob de stripfx.js (type 4 = notch, Q1.31 BE).
# Usage :
#   al_notch.py set  <tac 0-3> <bq 1-12> <freq_hz> [q=30] [--halved]
#   al_notch.py clear <tac 0-3> <bq 1-12>
# --halved : variante coefficients N1/D1 divisés par 2 (test format E0c)
import math, subprocess, sys

FS = 48000

def q31be(x):
    v = round(max(-1.0, min(0.9999999995, x)) * 0x80000000)
    v = max(-0x80000000, min(0x7FFFFFFF, v))
    if v < 0:
        v += 0x100000000
    return [(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255]

def notch_blob(f_hz, q, halved):
    w0 = 2 * math.pi * f_hz / FS
    cw, sw = math.cos(w0), math.sin(w0)
    al = sw / (2 * q)
    b0, b1, b2 = 1.0, -2 * cw, 1.0
    a0, a1, a2 = 1 + al, -2 * cw, 1 - al
    N0, N1, N2 = b0 / a0, b1 / a0, b2 / a0
    D1, D2 = -a1 / a0, -a2 / a0
    if halved:
        N1 /= 2.0
        D1 /= 2.0
    out = []
    for c in (N0, N1, N2, D1, D2):
        out += q31be(c)
    return out

FLAT = [0x7F, 0xFF, 0xFF, 0xFF] + [0] * 16

def find_card():
    for c in range(8):
        r = subprocess.run(["amixer", "-c", str(c), "controls"],
                           capture_output=True, text=True)
        if "TAC0 DAC BQ1 Coefs" in r.stdout:
            return c
    sys.exit("carte TAC introuvable")

def cset(card, name, blob):
    val = ",".join(f"{b}" for b in blob)
    r = subprocess.run(["amixer", "-c", str(card), "cset",
                        f"name='{name}'", val],
                       capture_output=True, text=True)
    if r.returncode != 0:
        # amixer veut parfois la syntaxe sans quotes internes
        r = subprocess.run(["amixer", "-c", str(card), "cset",
                            f"name={name}", val],
                           capture_output=True, text=True)
    print(r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr.strip())
    return r.returncode

if len(sys.argv) < 4:
    sys.exit(__doc__)
mode, tac, bq = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
name = f"TAC{tac} DAC BQ{bq} Coefs"
card = find_card()
if mode == "clear":
    sys.exit(cset(card, name, FLAT))
freq = float(sys.argv[4])
q = float(sys.argv[5]) if len(sys.argv) > 5 and not sys.argv[5].startswith("--") else 30.0
halved = "--halved" in sys.argv
blob = notch_blob(freq, q, halved)
print(f"notch {freq} Hz Q={q} {'(coefs /2)' if halved else '(pleins)'} → carte {card}, {name}")
sys.exit(cset(card, name, blob))
