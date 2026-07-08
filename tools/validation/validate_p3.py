#!/usr/bin/env python3
# P3 — persistance : restart mixer-pro + scène complète (slot 5 libre).
import json, subprocess, time, urllib.request

API = "http://127.0.0.1:8080/api/cmd"

def api(op, timeout=8):
    d = json.dumps(op).encode()
    rq = urllib.request.Request(API, data=d,
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(rq, timeout=timeout) as f:
            return json.loads(f.read())
    except Exception as e:
        return {"_exc": str(e)}

def http(path, body=None, timeout=30):
    rq = urllib.request.Request("http://127.0.0.1:8080" + path,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(rq, timeout=timeout) as f:
            return json.loads(f.read())
    except Exception as e:
        return {"_exc": str(e)}

P, F = [], []
def check(n, c, d=""):
    (P if c else F).append(n)
    print(("PASS " if c else "FAIL ") + n + ("  " + str(d) if d and not c else ""))

def snap():
    sr7 = api({"op": "get_strip_routing", "src": 7})
    return {
        "mute7": api({"op": "get_state"})["mute_mask"] & (1 << 7),
        "gain7": round(sr7["gain"], 3),
        "send70": round(sr7["sends"][0], 3),
        "out7": api({"op": "get_output_gain"})["gains"][7],
        "exp7_thr": round(api({"op": "get_expander"})["channels"][7]["threshold_db"], 1),
        "vfocus": api({"op": "get_vfocus"}).get("on"),
        "amx_resp": round(api({"op": "get_automix"}).get("resp_ms", -1), 1),
    }

def wait_mixer(t=25):
    for _ in range(t * 2):
        r = api({"op": "get_state"}, timeout=2)
        if r.get("ok"):
            return True
        time.sleep(0.5)
    return False

user = snap()
print("état utilisateur :", user)

# ---- valeurs test distinctives ----
api({"op": "set_mute", "src": 7, "mute": 1})
api({"op": "set_input_gain", "src": 7, "gain": 0.333})
api({"op": "set_send", "in": 7, "bus": 0, "gain": 0.25})
api({"op": "set_output_gain", "out": 7, "db": -9.0})
api({"op": "set_expander", "src": 7, "threshold_db": -47.0})
api({"op": "set_vfocus", "on": 0 if user["vfocus"] else 1})
api({"op": "set_automix_cfg", "resp_ms": 222.0})
time.sleep(2.5)          # débounce save 1 Hz

test = snap()
print("état test posé  :", test)

# ---- restart mixer-pro ----
subprocess.run(["systemctl", "restart", "mixer-pro"])
ok = wait_mixer()
check("restart: socket revenu", ok)
time.sleep(2)
after = snap()
check("restart: mute7 persisté", (after["mute7"] != 0) == True, after)
check("restart: gain7 persisté", abs(after["gain7"] - 0.333) < 0.01, after)
check("restart: send7->0 persisté", abs(after["send70"] - 0.25) < 0.01, after)
check("restart: out7 persisté", abs(after["out7"] - 355) < 8, after)
check("restart: expander thr persisté", abs(after["exp7_thr"] - (-47.0)) < 0.2, after)
check("restart: vfocus persisté", after["vfocus"] == test["vfocus"], after)
check("restart: automix resp_ms persisté", abs(after["amx_resp"] - 222.0) < 1, after)

# ---- scène complète sur slot 5 (libre) ----
r = http("/api/scene/save", {"slot": 5, "name": "VALIDTEST"})
check("scene: save slot 5", r.get("ok") is True, r)
# modifie 2 params puis recall → doivent revenir aux valeurs test
api({"op": "set_mute", "src": 7, "mute": 0})
api({"op": "set_input_gain", "src": 7, "gain": 0.9})
time.sleep(0.3)
r = http("/api/scene/recall", {"slot": 5}, timeout=60)
check("scene: recall slot 5", r.get("ok") is True, r)
time.sleep(2)
rec = snap()
check("scene: mute7 rappelé", rec["mute7"] != 0, rec)
check("scene: gain7 rappelé", abs(rec["gain7"] - 0.333) < 0.01, rec)
sc = api({"op": "scene_list"})["scenes"][5]
check("scene: nom slot5", sc["used"] == 1 and sc["name"] == "VALIDTEST", sc)

# ---- restauration état utilisateur ----
api({"op": "set_mute", "src": 7, "mute": 1 if user["mute7"] else 0})
api({"op": "set_input_gain", "src": 7, "gain": user["gain7"]})
api({"op": "set_send", "in": 7, "bus": 0, "gain": user["send70"]})
import math
api({"op": "set_output_gain", "out": 7,
     "db": 20 * math.log10(max(user["out7"], 1) / 1000.0)})
api({"op": "set_expander", "src": 7, "threshold_db": user["exp7_thr"]})
api({"op": "set_vfocus", "on": user["vfocus"]})
api({"op": "set_automix_cfg", "resp_ms": user["amx_resp"]})
time.sleep(2.5)
fin = snap()
check("restore: état utilisateur rétabli",
      all(abs(fin[k] - user[k]) < 0.02 if isinstance(user[k], float)
          else fin[k] == user[k] for k in user), {"user": user, "fin": fin})

xr = api({"op": "get_state"})["xrun"]
print(f"\nxrun total après P3 : {xr}")
print(f"PASS={len(P)} FAIL={len(F)}")
for n in F: print("  FAIL:", n)
