#!/usr/bin/env python3
# P4 — fonctionnel : sampler, looper, midix/synthé, automix, bandmix,
# taps/analyzer, larsen. Transitions d'état + cohérence, sans toucher
# aux réglages utilisateur.
import json, time, urllib.request

API = "http://127.0.0.1:8080/api/cmd"
P, F = [], []

def api(op, timeout=8):
    rq = urllib.request.Request(API, data=json.dumps(op).encode(),
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(rq, timeout=timeout) as f:
            return json.loads(f.read())
    except Exception as e:
        return {"_exc": str(e)}

def http_get(path, timeout=8):
    try:
        with urllib.request.urlopen("http://127.0.0.1:8080" + path, timeout=timeout) as f:
            return json.loads(f.read())
    except Exception as e:
        return {"_exc": str(e)}

def check(n, c, d=""):
    (P if c else F).append(n)
    print(("PASS " if c else "FAIL ") + n + ("  " + str(d)[:140] if d and not c else ""))

xrun0 = api({"op": "get_state"})["xrun"]

# ---------- SAMPLER ----------
sl = api({"op": "sampler_list"})["slots"]
loaded = [s for s in sl if s["name"]]
check("sampler: >=1 slot chargé", len(loaded) >= 1, sl)
if loaded:
    s0 = loaded[0]["slot"]
    api({"op": "sampler_trigger", "slot": s0})
    time.sleep(0.4)
    st = api({"op": "sampler_list"})["slots"][s0]
    check("sampler: playing=1 après trigger", st["playing"] == 1, st)
    check("sampler: pos avance", st["pos_s"] > 0, st)
    api({"op": "sampler_stop", "slot": s0})
    time.sleep(0.2)
    st = api({"op": "sampler_list"})["slots"][s0]
    check("sampler: stop", st["playing"] == 0, st)
    r = api({"op": "sampler_reload"})
    check("sampler: reload ok", r.get("ok") is True, r)
    time.sleep(0.5)
    st = api({"op": "sampler_list"})["slots"][s0]
    check("sampler: slot toujours chargé après reload", st["name"] == loaded[0]["name"], st)

# ---------- LOOPER ----------
ls = api({"op": "looper_status"})
check("looper: status ok", ls.get("ok") is True, ls)
st0 = json.dumps(ls)
# piste 5 (dernière) : rec silence 1.5 s → play_all → clear
tr0 = ls.get("tracks", [{}]*6)[5] if "tracks" in ls else {}
r = api({"op": "looper_track_ctl", "track": 5, "action": "rec"})
check("looper: rec piste 5 accepté", r.get("ok") is True, r)
time.sleep(1.5)
r = api({"op": "looper_track_ctl", "track": 5, "action": "play"})  # play = fige la loop
time.sleep(0.5)
ls2 = api({"op": "looper_status"})
tr = ls2.get("tracks", [{}]*6)[5] if "tracks" in ls2 else {}
print("  looper piste 5 après rec :", json.dumps(tr)[:160])
check("looper: piste 5 a une longueur", tr.get("len", tr.get("len_s", 0)) not in (0, None), ls2)
api({"op": "looper_ctl", "action": "play_all"})
time.sleep(0.5)
api({"op": "looper_ctl", "action": "stop_all"})
r = api({"op": "looper_track_ctl", "track": 5, "action": "clear"})
time.sleep(0.3)
ls3 = api({"op": "looper_status"})
tr3 = ls3.get("tracks", [{}]*6)[5] if "tracks" in ls3 else {}
check("looper: piste 5 vide après clear", tr3.get("len", tr3.get("len_s", 1)) in (0, 0.0), ls3)

# ---------- MIDIX / SYNTHÉ ----------
r = api({"op": "get_midix"})
check("midix: get ok", r.get("ok") is True, r)
r = api({"op": "midix_ctl", "cmd": "status"})
check("midix: ctl status proxy", r.get("ok") is True or "status" in json.dumps(r), r)

# ---------- AUTOMIX (enable global via set_automix_cfg) ----------
a0 = api({"op": "get_automix"})
for on in (1, 0):
    api({"op": "set_automix_cfg", "on": on})
    time.sleep(0.2)
    check(f"automix: on={on}", api({"op": "get_automix"})["on"] == on)
api({"op": "set_automix_cfg", "on": a0["on"]})
# adhésion par tranche : round-trip src 7
m0 = a0.get("members", [0]*18)[7]
api({"op": "set_automix", "src": 7, "on": 0 if m0 else 1})
time.sleep(0.2)
m1 = api({"op": "get_automix"}).get("members", [None]*18)[7]
api({"op": "set_automix", "src": 7, "on": m0})
check("automix: adhésion src7 round-trip", m1 != m0, (m0, m1))

# ---------- BANDMIX (measure sans lock = lecture seule) ----------
bs = api({"op": "bandmix_status"})
check("bandmix: status champs", all(k in bs for k in ("live",)), bs)
r = api({"op": "bandmix_measure"})          # sans src → refus propre attendu
check("bandmix: measure sans src refusé", r.get("ok") is False, r)
r = api({"op": "bandmix_measure", "src": -1})  # annulation = toujours ok
check("bandmix: measure cancel ok", r.get("ok") is True, r)

# ---------- TAPS / ANALYZER (4 taps simultanés) ----------
for t, (k, a) in enumerate([(1, 0), (1, 8), (2, 0), (3, 0)]):
    api({"op": "set_tap", "tap": t, "kind": k, "a": a, "b": -1})
time.sleep(0.6)
m = api({"op": "get_meters"})
an = m.get("analyzer", [])
check("taps: 4 taps actifs", [t["k"] for t in an] == [1, 1, 2, 3], an)
# libère tout (tap 3 = master pour la page mixer web → on le remet)
for t in range(3):
    api({"op": "set_tap", "tap": t, "kind": 0, "a": 0, "b": -1})
api({"op": "set_tap", "tap": 3, "kind": 3, "a": 0, "b": 1})
tp = api({"op": "get_taps"})["taps"]
check("taps: libérés + master rétabli",
      tp[0]["k"] == 0 and tp[1]["k"] == 0 and tp[2]["k"] == 0 and tp[3]["k"] == 3, tp)

# ---------- ANTI-LARSEN (état + toggle round-trip) ----------
lr = http_get("/api/larsen")
check("larsen: GET ok", lr.get("ok") is True, lr)
en0 = lr.get("enable")
if en0 is not None:
    def post_larsen(v):
        rq = urllib.request.Request("http://127.0.0.1:8080/api/larsen",
            data=json.dumps({"enable": v}).encode(),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(rq, timeout=8) as f:
            return json.loads(f.read())
    post_larsen(0 if en0 else 1)
    time.sleep(0.5)
    l1 = http_get("/api/larsen")
    post_larsen(en0)
    time.sleep(0.5)
    l2 = http_get("/api/larsen")
    check("larsen: toggle + retour",
          l1.get("enable") != en0 and
          l2.get("enable") == en0, (lr, l1, l2))

# ---------- routes GUI ----------
for path in ("/api/state", "/api/sysload", "/api/meters", "/api/drift"):
    r = http_get(path)
    check("http: " + path, r.get("ok") is True or "_exc" not in r, r)

xr = api({"op": "get_state"})["xrun"]
print(f"\nxrun delta pendant P4 : {xr - xrun0}")
print(f"PASS={len(P)} FAIL={len(F)}")
for n in F: print("  FAIL:", n)
