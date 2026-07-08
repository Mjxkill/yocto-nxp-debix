#!/usr/bin/env python3
# P5 — CPU par core + delta xrun + profil audio (prof_iter_us) par action.
# Chaque scénario : activer -> mesurer 6 s -> désactiver -> retour idle.
import json, subprocess, time, urllib.request

API = "http://127.0.0.1:8080/api/cmd"

def api(op, timeout=8):
    rq = urllib.request.Request(API, data=json.dumps(op).encode(),
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(rq, timeout=timeout) as f:
            return json.loads(f.read())
    except Exception as e:
        return {"_exc": str(e)}

def cpu_snap():
    d = {}
    for l in open("/proc/stat"):
        if l.startswith("cpu") and l[3].isdigit():
            f = l.split()
            d[f[0]] = (int(f[4]) + int(f[5]), sum(map(int, f[1:9])))
    return d

def measure(dur=6.0):
    a = cpu_snap()
    s0 = api({"op": "get_state"})
    time.sleep(dur)
    b = cpu_snap()
    s1 = api({"op": "get_state"})
    cores = []
    for c in sorted(a):
        di = b[c][0] - a[c][0]
        dt = b[c][1] - a[c][1]
        cores.append(round(100 * (1 - di / dt), 1))
    return {
        "cores": cores,
        "xrun_d": s1["xrun"] - s0["xrun"],
        "iter_us": s1["prof_iter_us"],
        "ring_drops_d": s1.get("ring_drops", 0) - s0.get("ring_drops", 0),
    }

def report(name, m):
    print(f"{name:34s} cores={m['cores']} iter={m['iter_us']}us "
          f"xrunΔ={m['xrun_d']} ringΔ={m['ring_drops_d']}")

# ---------- scénarios ----------
print("scénario                           CPU par core / prof audio")
report("idle", measure())

# 4 taps analyzer actifs
for t, (k, a) in enumerate([(1, 0), (1, 8), (2, 4), (3, 0)]):
    api({"op": "set_tap", "tap": t, "kind": k, "a": a, "b": -1})
report("4 taps analyzer", measure())
for t in range(3):
    api({"op": "set_tap", "tap": t, "kind": 0, "a": 0, "b": -1})
api({"op": "set_tap", "tap": 3, "kind": 3, "a": 0, "b": 1})

# sampler en boucle (retrigger toutes les 400 ms pendant la mesure)
import threading
stop = False
def spam_pad():
    while not stop:
        api({"op": "sampler_trigger", "slot": 0})
        time.sleep(0.4)
th = threading.Thread(target=spam_pad); th.start()
report("sampler retrigger 2.5 Hz", measure())
stop = True; th.join()
api({"op": "sampler_stop", "slot": 0})

# looper : 3 pistes en lecture (silence)
for t in (3, 4, 5):
    api({"op": "looper_track_ctl", "track": t, "action": "rec"})
    time.sleep(1.0)
    api({"op": "looper_track_ctl", "track": t, "action": "play"})
report("looper 3 pistes play", measure())
api({"op": "looper_ctl", "action": "stop_all"})
for t in (3, 4, 5):
    api({"op": "looper_track_ctl", "track": t, "action": "clear"})

# automix Dugan ON (8 membres)
a0 = api({"op": "get_automix"})
api({"op": "set_automix_cfg", "on": 1})
for s in range(8):
    api({"op": "set_automix", "src": s, "on": 1})
report("automix Dugan 8 membres", measure())
api({"op": "set_automix_cfg", "on": a0["on"]})
for s in range(8):
    api({"op": "set_automix", "src": s, "on": a0.get("members", [0]*18)[s]})

# vfocus ON
v0 = api({"op": "get_vfocus"}).get("on")
api({"op": "set_vfocus", "on": 1})
report("voix devant ON", measure())
api({"op": "set_vfocus", "on": v0})

# polling GUI agressif : 4 threads get_meters à 10 Hz
stop = False
def spam_meters():
    while not stop:
        api({"op": "get_meters"})
        time.sleep(0.1)
ths = [threading.Thread(target=spam_meters) for _ in range(4)]
for t in ths: t.start()
report("4x get_meters 10 Hz", measure())
stop = True
for t in ths: t.join()

# combiné : taps + sampler + automix + vfocus + polling
for t, (k, a) in enumerate([(1, 0), (1, 8), (2, 4), (3, 0)]):
    api({"op": "set_tap", "tap": t, "kind": k, "a": a, "b": -1})
api({"op": "set_automix_cfg", "on": 1})
api({"op": "set_vfocus", "on": 1})
stop = False
th1 = threading.Thread(target=spam_pad)
th2 = threading.Thread(target=spam_meters)
th1.start(); th2.start()
report("COMBINÉ (pire cas)", measure(8.0))
stop = True; th1.join(); th2.join()
api({"op": "sampler_stop", "slot": 0})
api({"op": "set_vfocus", "on": v0})
api({"op": "set_automix_cfg", "on": a0["on"]})
for t in range(3):
    api({"op": "set_tap", "tap": t, "kind": 0, "a": 0, "b": -1})
api({"op": "set_tap", "tap": 3, "kind": 3, "a": 0, "b": 1})

report("retour idle", measure())
print("\nNote: cores = [cpu0, cpu1, cpu2, cpu3] ; audio RT sur cores 2-3 (isolcpus)")
