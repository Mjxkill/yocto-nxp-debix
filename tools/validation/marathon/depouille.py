#!/usr/bin/env python3
# Dépouillement du marathon : sépare les CLICK alignés au tempo (batterie
# de la compo, 100 BPM → grille 600 ms) des vrais suspects, corrèle les
# suspects avec le journal des changements (±1,2 s), agrège les métriques.
import csv, math, pathlib, re, sys
from collections import defaultdict

HERE = pathlib.Path(__file__).parent
LOG = HERE / "marathon_log.csv"

clicks, changes, larsen, taps, mons, xruns, loopers = [], [], [], [], [], [], []
for line in open(LOG):
    parts = line.rstrip("\n").split(",", 3)
    if len(parts) < 4: continue
    ts, hms, kind, msg = float(parts[0]), parts[1], parts[2], parts[3]
    if kind == "CLICK":
        m = re.search(r"(\d+:\d+:\d+)\.(\d+) ch(\d) ratio=(\d+) fichier=cap_(\d+)", msg)
        if m:
            h, mn, s = map(int, m.group(1).split(":"))
            base = int(m.group(5))
            day = base - (base % 86400)  # approx, on reconstruit via chunk
            # ts absolu du clic : reconstruit à partir du chunk + hms local
            clicks.append({"hms": m.group(1) + "." + m.group(2),
                           "sec_in_day": h*3600 + mn*60 + s + int(m.group(2))/1000.0,
                           "ch": int(m.group(3)), "ratio": int(m.group(4)),
                           "file": "cap_" + m.group(5) + ".wav"})
    elif kind == "chg":
        changes.append({"ts": ts, "hms": hms, "msg": msg})
    elif kind == "larsen":
        larsen.append((hms, msg))
    elif kind == "tap":
        taps.append((hms, msg))
    elif kind == "XRUN":
        xruns.append((hms, msg))
    elif kind == "looper":
        loopers.append((ts, hms, msg))
    elif kind == "mon":
        mons.append(msg)

def sec_in_day(hms):
    h, mn, s = hms.split(":")
    return int(h)*3600 + int(mn)*60 + float(s)

# --- grille de tempo : 100 BPM = 0.6 s. On estime la phase par histogramme
# des clics modulo 0.6 : le pic = transitoires batterie.
GRID = 0.6
bins = defaultdict(int)
for c in clicks:
    bins[round((c["sec_in_day"] % GRID) / 0.02)] += 1
phase = max(bins, key=bins.get) * 0.02 if bins else 0

on_beat, suspects = [], []
for c in clicks:
    d = (c["sec_in_day"] - phase) % GRID
    d = min(d, GRID - d)
    (on_beat if d < 0.045 else suspects).append(c)

print(f"CLICKS total={len(clicks)}  alignés-tempo={len(on_beat)} "
      f"({100*len(on_beat)/max(1,len(clicks)):.0f}%)  suspects={len(suspects)}")

# --- classification onset/glitch des suspects par la forme d'onde ---
# vrai glitch = marche/chute d'énergie ; onset musical = l'énergie HF MONTE
import wave, numpy as np
CAPD = pathlib.Path("/home/michael/marathon_capture")
def classify(c):
    fn = CAPD / c["file"]
    if not fn.exists(): return "fichier-absent"
    try:
        base = int(re.search(r"cap_(\d+)", c["file"]).group(1))
        w = wave.open(str(fn), "rb")
        fs, nch = w.getframerate(), w.getnchannels()
        # offset : sec_in_day du clic - sec_in_day du début de chunk
        t0 = base % 86400
        off = (c["sec_in_day"] - (t0 + 7200) % 86400) % 86400  # UTC+2
        if off < 0.05 or off > w.getnframes()/fs - 0.05: return "hors-fenêtre"
        w.setpos(int((off - 0.03) * fs))
        d = np.frombuffer(w.readframes(int(0.06*fs)), dtype=np.int32).reshape(-1, nch)
        x = d[:, c["ch"]].astype(np.float64)
        dx = np.abs(np.diff(x)); i = int(np.argmax(dx))
        import itertools
        run = max(len(list(g)) for _, g in itertools.groupby(x[max(0,i-480):i+480]))
        pre = np.sqrt(np.mean(np.diff(x[:max(2,i-48)])**2)) + 1
        post = np.sqrt(np.mean(np.diff(x[i+48:])**2)) + 1
        if run > 24: return "GLITCH(trou)"
        if post < pre * 0.25: return "GLITCH(chute)"
        if post > pre * 1.2: return "onset-musical"
        return "ambigu"
    except Exception as e:
        return f"err:{e}"

verdicts = defaultdict(int)
for c in suspects:
    v = classify(c); c["verdict"] = v; verdicts[v] += 1
print("verdicts suspects :", dict(verdicts))

# --- corrélation suspects ↔ changements (fenêtre ±1.2 s) ---
chg_sec = [(sec_in_day(ch["hms"]), ch["msg"]) for ch in changes]
corr = defaultdict(list)
orphans = []
for c in suspects:
    best, bd = None, 1.2
    for cs, msg in chg_sec:
        d = abs(cs - c["sec_in_day"])
        if d < bd: bd, best = d, msg
    if best:
        typ = best.split(" ")[0]
        corr[typ].append((c["hms"], c["ratio"], f"{bd:+.2f}s", best))
    else:
        orphans.append(c)

print("\n--- suspects corrélés à un changement (±1.2 s) ---")
for typ, lst in sorted(corr.items(), key=lambda kv: -len(kv[1])):
    print(f"  {typ:12s} : {len(lst)}")
    for hms, ratio, dt, msg in lst[:4]:
        print(f"      {hms} ratio={ratio} dt={dt} [{msg}]")
print(f"\n--- suspects orphelins (aucun changement proche) : {len(orphans)}")
for c in orphans[:10]:
    print(f"      {c['hms']} ch{c['ch']} ratio={c['ratio']} {c['file']}")

# --- coutures looper : clics dans les fenêtres play des cycles looper ---
print("\n--- larsen ---")
for hms, msg in larsen: print(f"  {hms} {msg}")
print("\n--- taps FX ---")
for hms, msg in taps[-8:]: print(f"  {hms} {msg}")
print("\n--- xruns ---")
if xruns:
    for hms, msg in xruns: print(f"  {hms} {msg}")
else:
    print("  aucun")
iters = [int(re.search(r"iter=(\d+)us", m).group(1)) for m in mons
         if re.search(r"iter=(\d+)us", m)]
if iters:
    print(f"\nprof_iter : min={min(iters)} moy={sum(iters)//len(iters)} "
          f"max={max(iters)} µs sur {len(iters)} points")
print(f"changements totaux : {len(changes)}")
