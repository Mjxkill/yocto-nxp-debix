#!/usr/bin/env python3
# P2 — smoke test des ops mixer-pro via /api/cmd (exécuté SUR le board pour
# éviter 66 round-trips ssh). Round-trips non destructifs : lire → écrire
# une valeur test → relire → comparer → restaurer → relire.
import json, subprocess, sys, time, urllib.request

API = "http://127.0.0.1:8080/api/cmd"
R = {"pass": [], "fail": [], "skip": []}

def api(op, timeout=6):
    d = json.dumps(op).encode()
    rq = urllib.request.Request(API, data=d,
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(rq, timeout=timeout) as f:
            return json.loads(f.read())
    except Exception as e:
        return {"_exc": str(e)}

def check(name, cond, detail=""):
    (R["pass"] if cond else R["fail"]).append((name, detail))
    print(("PASS " if cond else "FAIL ") + name + ("  " + detail if detail and not cond else ""))

def get_ok(opname, extra=None, field=None):
    o = {"op": opname}
    if extra: o.update(extra)
    r = api(o)
    ok = r.get("ok") is True and (field is None or field in r)
    check("get:" + opname, ok, json.dumps(r)[:120])
    return r

# ---------- 1. tous les GET ----------
xrun0 = api({"op": "get_state"}).get("xrun")
get_ok("get_state", field="version")
get_ok("get_meters", field="analyzer")
get_ok("get_meters_lite") if True else None
get_ok("get_alsa", {"numid": 15})
get_ok("get_automix", field="on")
get_ok("get_comp")
get_ok("get_expander", field="channels")
get_ok("get_fx", {"bus": 0})
get_ok("get_input_map")
get_ok("get_insert")
get_ok("get_insert_bypass")
get_ok("get_midix")
get_ok("get_output_gain")
get_ok("get_strip_routing", {"src": 0})
get_ok("get_taps", field="taps")
get_ok("get_vfocus")
get_ok("get_drift")
get_ok("get_assistant")
get_ok("bandmix_status")
get_ok("looper_status")
get_ok("sampler_list", field="slots")
get_ok("scene_list", field="scenes")
r = api({"op": "list_lv2_plugins"})
check("get:list_lv2_plugins", r.get("ok") is True, json.dumps(r)[:100])

# ---------- 2. round-trips SET non destructifs ----------
def roundtrip(name, read, write, restore, cmp):
    """read()->v0 ; write() ; read()->v1 cmp(v1) ; restore() ; read()->v2==v0"""
    v0 = read()
    write()
    time.sleep(0.15)
    v1 = read()
    ok1 = cmp(v1)
    restore()
    time.sleep(0.15)
    v2 = read()
    ok2 = (v2 == v0)
    check("rt:" + name, ok1 and ok2,
          f"écrit={ok1} restauré={ok2} v0={v0} v1={v1} v2={v2}"[:160])

# mute strip 7 (le moins utilisé des mics)
roundtrip("set_mute",
    lambda: api({"op": "get_state"})["mute_mask"] & (1 << 7),
    lambda: api({"op": "set_mute", "src": 7, "mute": 1}),
    lambda: api({"op": "set_mute", "src": 7, "mute": 0}),
    lambda v: v != 0)

# input gain strip 7
def gain7():
    r = api({"op": "get_strip_routing", "src": 7})
    return round(r.get("gain", -999), 3)
g0 = gain7()
import math
if g0 > -900:
    g0db = 20*math.log10(max(g0,1e-6))
    roundtrip("set_input_gain",
        gain7,
        lambda: api({"op": "set_input_gain", "src": 7, "gain": 0.237}),
        lambda: api({"op": "set_input_gain", "src": 7, "gain": g0}),
        lambda v: abs(v - 0.237) < 0.01)
else:
    R["skip"].append(("set_input_gain", "champ gain introuvable"))
    print("SKIP set_input_gain (schéma get_strip_routing) :",
          json.dumps(api({"op": "get_strip_routing", "src": 7}))[:200])

# output gain out 7
def og():
    r = api({"op": "get_output_gain"})
    return r.get("gains", [None]*18)[7]
o0 = og()
if o0 is not None:
    roundtrip("set_output_gain",
        og,
        lambda: api({"op": "set_output_gain", "out": 7, "db": -6.0}),
        lambda: api({"op": "set_output_gain", "out": 7,
                     "db": 20*math.log10(max(o0,1)/1000.0)}),
        lambda v: abs(v - 501) < 6)
else:
    print("SKIP set_output_gain, schéma:", json.dumps(api({"op": "get_output_gain"}))[:200])
    R["skip"].append(("set_output_gain", "schéma"))

# tap 2 (libre)
roundtrip("set_tap",
    lambda: api({"op": "get_taps"})["taps"][2].get("k"),
    lambda: api({"op": "set_tap", "tap": 2, "kind": 1, "a": 3, "b": -1}),
    lambda: api({"op": "set_tap", "tap": 2, "kind": 0, "a": 0, "b": -1}),
    lambda v: v == 1)

# expander ch 7 : threshold
def exp7():
    return api({"op": "get_expander"})["channels"][7]
e0 = exp7()
roundtrip("set_expander",
    lambda: round(exp7()["threshold_db"], 1),
    lambda: api({"op": "set_expander", "src": 7, "threshold_db": -55.0}),
    lambda: api({"op": "set_expander", "src": 7, "threshold_db": e0["threshold_db"]}),
    lambda v: abs(v - (-55.0)) < 0.2)

# comp ch 7
def comp7():
    r = api({"op": "get_comp"})
    ch = r.get("channels")
    return round(ch[7]["threshold_db"], 1) if ch else None
c0 = comp7()
if c0 is not None:
    roundtrip("set_comp",
        comp7,
        lambda: api({"op": "set_comp", "src": 7, "threshold_db": -33.0}),
        lambda: api({"op": "set_comp", "src": 7, "threshold_db": c0}),
        lambda v: abs(v - (-33.0)) < 0.2)
else:
    print("SKIP set_comp, schéma:", json.dumps(api({"op": "get_comp"}))[:200])
    R["skip"].append(("set_comp", "schéma"))

# vfocus : on/off
def vf():
    return api({"op": "get_vfocus"}).get("on")
v0 = vf()
roundtrip("set_vfocus",
    vf,
    lambda: api({"op": "set_vfocus", "on": 0 if v0 else 1}),
    lambda: api({"op": "set_vfocus", "on": v0}),
    lambda v: v != v0)

# automix : respons ms (cfg)
def amx():
    return api({"op": "get_automix"})
a0 = amx()
if "resp_ms" in a0:
    roundtrip("set_automix_cfg",
        lambda: round(amx()["resp_ms"], 1),
        lambda: api({"op": "set_automix_cfg", "resp_ms": 123.0}),
        lambda: api({"op": "set_automix_cfg", "resp_ms": a0["resp_ms"]}),
        lambda v: abs(v - 123.0) < 0.5)
else:
    print("SKIP set_automix_cfg, schéma get_automix:", json.dumps(a0)[:250])
    R["skip"].append(("set_automix_cfg", "schéma"))

# insert bypass global
def ib():
    r = api({"op": "get_insert_bypass"})
    return r.get("bypass", r.get("on"))
b0 = ib()
if b0 is not None:
    roundtrip("insert_bypass",
        ib,
        lambda: api({"op": "set_insert_bypass", "on": 1 if b0 else 0}),
        lambda: api({"op": "set_insert_bypass", "on": 0 if b0 else 1}),
        lambda v: v != b0)
else:
    print("SKIP insert_bypass, schéma:", json.dumps(api({"op": "get_insert_bypass"}))[:200])
    R["skip"].append(("insert_bypass", "schéma"))

# send strip7 -> bus0
def send7():
    r = api({"op": "get_strip_routing", "src": 7})
    s = r.get("sends")
    return round(s[0], 2) if s is not None else None
s0 = send7()
if s0 is not None:
    roundtrip("set_send",
        send7,
        lambda: api({"op": "set_send", "in": 7, "bus": 0, "gain": 0.1}),
        lambda: api({"op": "set_send", "in": 7, "bus": 0, "gain": s0}),
        lambda v: abs(v - 0.1) < 0.01)
else:
    print("SKIP set_send, schéma:", json.dumps(api({"op": "get_strip_routing", "src": 7}))[:250])
    R["skip"].append(("set_send", "schéma"))

# bandmix live toggle
bs = api({"op": "bandmix_status"})
if bs.get("ok"):
    lv = bs.get("live")
    roundtrip("bandmix_live",
        lambda: api({"op": "bandmix_status"}).get("live"),
        lambda: api({"op": "bandmix_live", "on": 0 if lv else 1}),
        lambda: api({"op": "bandmix_live", "on": lv}),
        lambda v: v != lv)

# ---------- 3. robustesse : args invalides -> erreur propre ----------
for name, op in [
    ("set_mute src hors bornes", {"op": "set_mute", "src": 99, "mute": 1}),
    ("set_tap kind invalide", {"op": "set_tap", "tap": 2, "kind": 9, "a": 0}),
    ("set_input_gain sans src", {"op": "set_input_gain", "gain_db": 0}),
    ("sampler_trigger slot invalide", {"op": "sampler_trigger", "slot": 99}),
    ("scene_recall slot vide", {"op": "scene_recall", "slot": 4}),
    ("op inconnue", {"op": "n_existe_pas"}),
]:
    r = api(op)
    ok = r.get("ok") is False or "err" in r or "_exc" not in r and r.get("ok") is not True
    check("rob:" + name, r.get("ok") is not True, json.dumps(r)[:100])

# ---------- bilan ----------
time.sleep(0.3)
xrun1 = api({"op": "get_state"}).get("xrun")
print(f"\nxrun delta pendant P2 : {xrun1 - xrun0}")
print(f"PASS={len(R['pass'])} FAIL={len(R['fail'])} SKIP={len(R['skip'])}")
for n, d in R["fail"]:
    print("  FAIL:", n, d)
