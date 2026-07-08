#!/usr/bin/env python3
# P8 — TOUT ACTIVÉ : gates+comps 16 voies, EQ TAC 3 bandes x 8 mics,
# automix 8 membres, vfocus, mastering, 4 taps, looper 6 pistes, sampler
# en rafale, 4 pollers GUI. Mesure 60 s puis RESTAURATION INTÉGRALE.
import json, math, subprocess, threading, time, urllib.request

API = "http://127.0.0.1:8080/api/cmd"
ALSA = "http://127.0.0.1:8080/api/alsa/set"

def api(op, timeout=8):
    rq = urllib.request.Request(API, data=json.dumps(op).encode(),
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(rq, timeout=timeout) as f:
            return json.loads(f.read())
    except Exception as e:
        return {"_exc": str(e)}

def alsa_set(numid, value):
    rq = urllib.request.Request(ALSA,
        data=json.dumps({"numid": numid, "value": value}).encode(),
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(rq, timeout=8) as f:
            return json.loads(f.read())
    except Exception as e:
        return {"_exc": str(e)}

def amixer_get_raw(numid):
    out = subprocess.run(["amixer", "-c", "softac5212tdm", "cget",
                          f"numid={numid}"], capture_output=True, text=True).stdout
    for l in out.splitlines():
        l = l.strip()
        if l.startswith(": values="):
            return l[len(": values="):]
    return None

def rbj_peaking(f, q, gdb):
    fs = 48000
    w0 = 2*math.pi*f/fs; cw, sw = math.cos(w0), math.sin(w0)
    A = 10**(gdb/40); al = sw/(2*q)
    b0 = 1+al*A; b1 = -2*cw; b2 = 1-al*A
    a0 = 1+al/A; a1 = -2*cw; a2 = 1-al/A
    N0, N1, N2, D1, D2 = b0/a0, b1/a0/2, b2/a0, -a1/a0/2, -a2/a0
    def q31(x):
        v = round(max(-1, min(0.9999999995, x))*0x80000000)
        v = max(-0x80000000, min(0x7FFFFFFF, v))
        if v < 0: v += 0x100000000
        return [(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255]
    out = []
    for c in (N0, N1, N2, D1, D2): out += q31(c)
    return ",".join(map(str, out))

def cpu_pct(dur):
    def snap():
        d = {}
        for l in open("/proc/stat"):
            if l.startswith("cpu") and l[3].isdigit():
                f = l.split()
                d[f[0]] = (int(f[4])+int(f[5]), sum(map(int, f[1:9])))
        return d
    a = snap(); time.sleep(dur); b = snap()
    return [round(100*(1-(b[c][0]-a[c][0])/max(1, b[c][1]-a[c][1])), 1)
            for c in sorted(a)]

print("=== SAUVEGARDE de l'état utilisateur")
exp0 = api({"op": "get_expander"})["channels"]
cmp0 = api({"op": "get_comp"})["channels"]
amx0 = api({"op": "get_automix"})
vf0 = api({"op": "get_vfocus"})
# EQ : BQ1/5/9 + BQ2/6/10 des 4 TACs (ADC) — numids relevés par nom
BQ_NUMIDS = {}
contents = subprocess.run(["amixer", "-c", "softac5212tdm", "controls"],
                          capture_output=True, text=True).stdout
for l in contents.splitlines():
    if "ADC BQ" in l and "Coefs" in l:
        numid = int(l.split(",")[0].split("=")[1])
        name = l.split("name='")[1].rstrip("'")
        BQ_NUMIDS[name] = numid
live_bqs = [f"TAC{t} ADC BQ{i} Coefs" for t in range(4) for i in (1, 5, 9, 2, 6, 10)]
bq0 = {n: amixer_get_raw(BQ_NUMIDS[n]) for n in live_bqs if n in BQ_NUMIDS}
print(f"  {len(bq0)} blobs EQ sauvegardés, 16 gates, 16 comps")

s0 = api({"op": "get_state"})
print(f"  xrun départ {s0['xrun']}, drops {s0.get('ring_drops')}")

print("=== ACTIVATION TOTALE")
# gates + comps sur 16 voies (seuils bas = actifs mais transparents sur silence)
for i in range(16):
    api({"op": "set_expander", "src": i, "on": 1, "threshold_db": -70.0,
         "ratio": 4.0, "attack_ms": 2.0, "release_ms": 150.0,
         "range_db": 40.0, "hold_ms": 50.0})
    api({"op": "set_comp", "src": i, "on": 1, "threshold_db": -30.0,
         "ratio": 3.0, "attack_ms": 5.0, "release_ms": 120.0,
         "makeup_db": 0.0})
# EQ 3 bandes sur les 8 mics (24 blobs, via le même chemin que la GUI)
neq = 0
for t in range(4):
    for ch, idxs in ((1, (1, 5, 9)), (2, (2, 6, 10))):
        for k, i in enumerate(idxs):
            n = f"TAC{t} ADC BQ{i} Coefs"
            if n in BQ_NUMIDS:
                blob = rbj_peaking([180, 950, 5200][k], 1.2, [-3, -4, 2][k])
                alsa_set(BQ_NUMIDS[n], blob)
                neq += 1
# automix 8 membres + vfocus + bandmix live (si réf dispo)
api({"op": "set_automix_cfg", "on": 1})
for s in range(8):
    api({"op": "set_automix", "src": s, "on": 1})
api({"op": "set_vfocus", "on": 1})
api({"op": "bandmix_live", "on": 1})
# mastering déjà on ; 4 taps FFT
for t, (k, a) in enumerate([(1, 0), (1, 8), (2, 4), (3, 0)]):
    api({"op": "set_tap", "tap": t, "kind": k, "a": a, "b": -1})
# looper : 6 pistes (silence 1 s chacune) en lecture
for t in range(6):
    api({"op": "looper_track_ctl", "track": t, "action": "rec"})
    time.sleep(1.0)
    api({"op": "looper_track_ctl", "track": t, "action": "play"})
api({"op": "looper_ctl", "action": "play_all"})
bm = api({"op": "bandmix_status"})
print(f"  {neq} blobs EQ actifs, looper 6 pistes, bandmix live={bm.get('live')}")

# sampler en rafale + 4 pollers GUI
stop = False
def spam_pad():
    while not stop:
        api({"op": "sampler_trigger", "slot": 0}); time.sleep(0.8)
def spam_meters():
    while not stop:
        api({"op": "get_meters"}); time.sleep(0.1)
ths = [threading.Thread(target=spam_pad)] + \
      [threading.Thread(target=spam_meters) for _ in range(4)]
for t in ths: t.start()

print("=== MESURE 60 s (tout actif)")
sA = api({"op": "get_state"})
c1 = cpu_pct(20); c2 = cpu_pct(20); c3 = cpu_pct(20)
sB = api({"op": "get_state"})
stop = True
for t in ths: t.join()

print(f"  CPU  0-20s : {c1}")
print(f"  CPU 20-40s : {c2}")
print(f"  CPU 40-60s : {c3}")
print(f"  prof_iter  : {sB['prof_iter_us']} µs (cap {sB['prof_cap_us']} + mix {sB['prof_mix_us']} + play {sB['prof_play_us']})")
print(f"  xrun Δ     : {sB['xrun'] - sA['xrun']}")
print(f"  drops Δ    : {sB.get('ring_drops', 0) - sA.get('ring_drops', 0)}")

print("=== RESTAURATION INTÉGRALE")
api({"op": "sampler_stop", "slot": 0})
api({"op": "looper_ctl", "action": "stop_all"})
for t in range(6):
    api({"op": "looper_track_ctl", "track": t, "action": "clear"})
for i, e in enumerate(exp0):
    api({"op": "set_expander", "src": i, "on": e["on"],
         "threshold_db": e["threshold_db"], "ratio": e["ratio"],
         "attack_ms": e["attack_ms"], "release_ms": e["release_ms"],
         "range_db": e["range_db"], "hold_ms": e["hold_ms"]})
for i, c in enumerate(cmp0):
    api({"op": "set_comp", "src": i, "on": c["on"],
         "threshold_db": c["threshold_db"], "ratio": c["ratio"],
         "attack_ms": c["attack_ms"], "release_ms": c["release_ms"],
         "makeup_db": c["makeup_db"]})
for n, raw in bq0.items():
    if raw:
        alsa_set(BQ_NUMIDS[n], raw)
api({"op": "set_automix_cfg", "on": amx0["on"], "resp_ms": amx0["resp_ms"]})
for s in range(8):
    api({"op": "set_automix", "src": s, "on": amx0.get("members", [0]*18)[s]})
api({"op": "set_vfocus", "on": vf0.get("on", 0)})
api({"op": "bandmix_live", "on": bm.get("live", 0) and 0})
for t in range(3):
    api({"op": "set_tap", "tap": t, "kind": 0, "a": 0, "b": -1})
api({"op": "set_tap", "tap": 3, "kind": 3, "a": 0, "b": 1})
time.sleep(0.5)

# vérification de la restauration
exp1 = api({"op": "get_expander"})["channels"]
cmp1 = api({"op": "get_comp"})["channels"]
ok_exp = all(abs(exp1[i]["threshold_db"] - exp0[i]["threshold_db"]) < 0.1
             and exp1[i]["on"] == exp0[i]["on"] for i in range(16))
ok_cmp = all(abs(cmp1[i]["threshold_db"] - cmp0[i]["threshold_db"]) < 0.1
             and cmp1[i]["on"] == cmp0[i]["on"] for i in range(16))
ok_bq = all(amixer_get_raw(BQ_NUMIDS[n]) == raw for n, raw in bq0.items() if raw)
sF = api({"op": "get_state"})
print(f"  gates restaurés  : {'OUI' if ok_exp else 'NON !'}")
print(f"  comps restaurés  : {'OUI' if ok_cmp else 'NON !'}")
print(f"  EQ restaurés     : {'OUI' if ok_bq else 'NON !'} ({len(bq0)} blobs)")
print(f"  xrun fin         : {sF['xrun']} (Δ total {sF['xrun'] - s0['xrun']})")
