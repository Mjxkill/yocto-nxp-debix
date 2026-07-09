#!/usr/bin/env python3
# Génère la compo du marathon : SMF format 0, ~3 min, 100 BPM.
# Am - F - C - G, piano arpèges (ch1), nappes cordes (ch2), basse (ch3),
# batterie GM (ch10), mélodie celesta (ch4) sur la section B.
import struct, random

TPQ = 480          # ticks par noire
BPM = 100

def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))

ev = []            # (tick_abs, bytes)
def note(ch, key, vel, t0, dur):
    ev.append((t0, bytes([0x90 | ch, key, vel])))
    ev.append((t0 + dur, bytes([0x80 | ch, key, 0])))
def prog(ch, p, t0=0):
    ev.append((t0, bytes([0xC0 | ch, p])))
def cc(ch, num, val, t0=0):
    ev.append((t0, bytes([0xB0 | ch, num, val])))

# programmes GM : 0 piano, 48 strings, 33 basse fingered, 8 celesta
prog(0, 0); prog(1, 48); prog(2, 33); prog(3, 8)
cc(0, 7, 95); cc(1, 7, 70); cc(2, 7, 100); cc(3, 7, 85); cc(9, 7, 100)

# grille : Am F C G (2 tours = section) — root, tierce, quinte
CH = [(57, 60, 64), (53, 57, 60), (48, 52, 55), (55, 59, 62)]  # A2m F2 C2 G2
BAR = 4 * TPQ
random.seed(20260708)

def drums(bar_t, fill=False):
    for b in range(4):
        t = bar_t + b * TPQ
        note(9, 42, 70, t, 60)                 # HH
        note(9, 42, 55, t + TPQ // 2, 60)
        if b in (0, 2): note(9, 36, 100, t, 60)      # kick
        if b in (1, 3): note(9, 38, 95, t, 60)       # snare
    if fill:
        for k in range(4):
            note(9, 45 + k, 80, bar_t + 3 * TPQ + k * (TPQ // 4), 50)

def bar_piano(bar_t, chord, pattern):
    r, t3, q = chord
    if pattern == 0:      # arpège montée-descente en doubles
        seq = [r, t3, q, t3 + 12, q, t3, r + 12, q]
        for k, key in enumerate(seq):
            note(0, key + 12, 72 + random.randint(-8, 8), bar_t + k * (TPQ // 2), TPQ // 2 - 20)
    else:                 # accords plaqués syncopés
        for beat in (0, 1.5, 3):
            t = bar_t + int(beat * TPQ)
            for key in chord:
                note(0, key + 12, 80, t, TPQ - 40)

def bar_bass(bar_t, chord):
    r = chord[0] - 12
    for b in range(4):
        note(2, r if b != 3 else r + 7, 92, bar_t + b * TPQ, TPQ - 30)

def bar_strings(bar_t, chord):
    for key in chord:
        note(1, key + 24, 52, bar_t, BAR - 40)

MELO = [76, 74, 72, 74, 76, 79, 77, 76, 74, 72, 71, 72, 74, 72, 71, 69]
def bar_melody(bar_t, i):
    for k in range(4):
        key = MELO[(i * 4 + k) % len(MELO)]
        note(3, key, 88, bar_t + k * TPQ, TPQ - 60)

t = 0
NB_SECTIONS = 6            # ~3 min
for sec in range(NB_SECTIONS):
    B = sec % 2 == 1       # sections B : mélodie + plaqués
    for tour in range(2):
        for ci, chord in enumerate(CH):
            drums(t, fill=(ci == 3))
            bar_piano(t, chord, 1 if B else 0)
            bar_bass(t, chord)
            bar_strings(t, chord)
            if B: bar_melody(t, ci + tour * 4)
            t += BAR

# fin propre
ev.append((t + TPQ, bytes([0xB0, 123, 0])))

ev.sort(key=lambda e: e[0])
trk = bytearray()
# tempo
trk += vlq(0) + bytes([0xFF, 0x51, 0x03]) + struct.pack(">I", 60000000 // BPM)[1:]
last = 0
for tick, data in ev:
    trk += vlq(tick - last) + data
    last = tick
trk += vlq(0) + bytes([0xFF, 0x2F, 0x00])

out = b"MThd" + struct.pack(">IHHH", 6, 0, 1, TPQ)
out += b"MTrk" + struct.pack(">I", len(trk)) + trk
open(__file__.replace("compose.py", "compo.mid"), "wb").write(out)
dur_s = t / TPQ * 60 / BPM
print(f"compo.mid : {len(ev)} événements, {dur_s:.0f} s")
