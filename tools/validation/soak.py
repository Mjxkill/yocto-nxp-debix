#!/usr/bin/env python3
# Soak test A.L.A. — échantillonne santé/perf en continu + burst de stress
# périodique. Log CSV : /root/tests/soak_20260708.csv
import json, time, urllib.request, subprocess, threading

API = "http://127.0.0.1:8080/api/cmd"
LOG = "/root/tests/soak_20260708.csv"
STRESS_EVERY = 600          # burst toutes les 10 min
SAMPLE_EVERY = 60

def api(op, timeout=6):
    rq = urllib.request.Request(API, data=json.dumps(op).encode(),
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(rq, timeout=timeout) as f:
            return json.loads(f.read())
    except Exception:
        return {}

def rss(name):
    try:
        out = subprocess.run(["pgrep", "-x", name], capture_output=True, text=True).stdout.split()
        if not out: return 0
        with open(f"/proc/{out[0]}/status") as f:
            for l in f:
                if l.startswith("VmRSS"):
                    return int(l.split()[1])
    except Exception:
        return 0
    return 0

def cpu_pct(dur=2.0):
    def snap():
        d = {}
        for l in open("/proc/stat"):
            if l.startswith("cpu") and l[3].isdigit():
                f = l.split()
                d[f[0]] = (int(f[4]) + int(f[5]), sum(map(int, f[1:9])))
        return d
    a = snap(); time.sleep(dur); b = snap()
    return [round(100 * (1 - (b[c][0]-a[c][0]) / max(1, b[c][1]-a[c][1])), 1)
            for c in sorted(a)]

def stress_burst():
    """~30 s de sollicitation légère : pads + taps + polling."""
    for t, (k, a) in enumerate([(1, 0), (1, 8), (2, 4)]):
        api({"op": "set_tap", "tap": t, "kind": k, "a": a, "b": -1})
    for i in range(20):
        api({"op": "sampler_trigger", "slot": 0})
        api({"op": "get_meters"})
        time.sleep(1.5)
    api({"op": "sampler_stop", "slot": 0})
    for t in range(3):
        api({"op": "set_tap", "tap": t, "kind": 0, "a": 0, "b": -1})

with open(LOG, "a") as f:
    f.write("time,xrun,ring_drops,iter_us,cpu0,cpu1,cpu2,cpu3,"
            "rss_mixer,rss_gui,rss_console,rss_midix,stress\n")

last_stress = 0
while True:
    t0 = time.time()
    stress = 0
    if t0 - last_stress >= STRESS_EVERY:
        last_stress = t0
        stress = 1
        threading.Thread(target=stress_burst, daemon=True).start()
    s = api({"op": "get_state"})
    c = cpu_pct()
    row = [time.strftime("%H:%M:%S"),
           s.get("xrun", -1), s.get("ring_drops", -1), s.get("prof_iter_us", -1),
           c[0], c[1], c[2], c[3],
           rss("mixer-pro"), rss("mixer-gui-http"),
           rss("mixer-console"), rss("midi-expander"), stress]
    with open(LOG, "a") as f:
        f.write(",".join(map(str, row)) + "\n")
    time.sleep(max(1, SAMPLE_EVERY - (time.time() - t0)))
