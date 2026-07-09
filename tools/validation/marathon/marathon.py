#!/usr/bin/env python3
# MARATHON A.L.A. — stress 2 h avec vrai signal + compo MIDI + brasseur de
# paramètres journalisé + détection de glitchs corrélée + rafales larsen +
# sondes TAP OUT. Tout l'état utilisateur est sauvegardé puis restauré.
#
# Usage : python3 marathon.py [durée_s]      (défaut 7200)
import json, math, os, pathlib, random, signal, struct, subprocess, sys
import threading, time, urllib.request
import numpy as np

BOARD = "192.168.0.198"
API = f"http://{BOARD}:8080/api/cmd"
ALSA = f"http://{BOARD}:8080/api/alsa/set"
SINK = "alsa_output.usb-Electrosens_Debix_UAC2_8x8_v7.0-e6a-00.playback.0.0"
SRC = "alsa_input.usb-Electrosens_Debix_UAC2_8x8_v7.0-e6a-00.capture.0.0"
AUXMAP = "aux0,aux1,aux2,aux3,aux4,aux5,aux6,aux7"
HERE = pathlib.Path(__file__).parent
STEMS = HERE.parent / "stems"
CAPDIR = pathlib.Path("/home/michael/marathon_capture")
CAPDIR.mkdir(exist_ok=True)
DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 7200
T0 = time.time()
STOP = threading.Event()
LOCK = threading.Lock()

def log(kind, msg):
    line = f"{time.time():.3f},{time.strftime('%H:%M:%S')},{kind},{msg}"
    with LOCK, open(HERE / "marathon_log.csv", "a") as f:
        f.write(line + "\n")
    print(line, flush=True)

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

def http_get(path, timeout=6):
    try:
        with urllib.request.urlopen(f"http://{BOARD}:8080{path}", timeout=timeout) as f:
            return json.loads(f.read())
    except Exception as e:
        return {"_exc": str(e)}

def ssh(cmd, timeout=30):
    return subprocess.run(["ssh", f"root@{BOARD}", cmd],
                          capture_output=True, text=True, timeout=timeout).stdout

def rbj_peaking(f, q, gdb):
    fs = 48000
    w0 = 2*math.pi*f/fs; cw, sw = math.cos(w0), math.sin(w0)
    A = 10**(gdb/40); al = sw/(2*q)
    b0, b1, b2 = 1+al*A, -2*cw, 1-al*A
    a0, a1, a2 = 1+al/A, -2*cw, 1-al/A
    def q31(x):
        v = round(max(-1, min(0.9999999995, x))*0x80000000)
        v = max(-0x80000000, min(0x7FFFFFFF, v))
        if v < 0: v += 0x100000000
        return [(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255]
    out = []
    for c in (b0/a0, b1/a0/2, b2/a0, -a1/a0/2, -a2/a0): out += q31(c)
    return ",".join(map(str, out))

UNITY = "127,255,255,255," + ",".join(["0"]*16)

# ---------------------------------------------------------------- état
def amixer_get_raw(numid):
    out = ssh(f"amixer -c softac5212tdm cget numid={numid} | tail -1")
    return out.strip().split("values=")[-1] if "values=" in out else None

def save_user_state():
    st = {"routing": {}, "exp": api({"op": "get_expander"})["channels"],
          "cmp": api({"op": "get_comp"})["channels"],
          "amx": api({"op": "get_automix"}),
          "vf": api({"op": "get_vfocus"}),
          "ib": api({"op": "get_insert_bypass"}),
          "mute": api({"op": "get_state"})["mute_mask"],
          "midix": api({"op": "midix_ctl", "cmd": "status"}),
          "bq": {}}
    for s in range(26):
        st["routing"][s] = api({"op": "get_strip_routing", "src": s})
    ctl = ssh("amixer -c softac5212tdm controls | grep 'ADC BQ'")
    st["bq_numids"] = {}
    for l in ctl.splitlines():
        if "Coefs" in l:
            numid = int(l.split(",")[0].split("=")[1])
            name = l.split("name='")[1].rstrip("'")
            st["bq_numids"][name] = numid
    for t in range(4):
        for i in (1, 5, 9, 2, 6, 10):
            n = f"TAC{t} ADC BQ{i} Coefs"
            if n in st["bq_numids"]:
                st["bq"][n] = amixer_get_raw(st["bq_numids"][n])
    return st

def restore_user_state(st):
    log("restore", "début restauration intégrale")
    api({"op": "sampler_stop", "slot": 0})
    api({"op": "looper_ctl", "action": "stop_all"})
    for t in range(6):
        api({"op": "looper_track_ctl", "track": t, "action": "clear"})
        api({"op": "looper_track_cfg", "track": t, "src_a": t, "src_b": -1,
             "gain_db": 0.0})
    for s in range(26):
        r = st["routing"][s]
        api({"op": "set_input_gain", "src": s, "gain": r["gain"]})
        api({"op": "set_mute", "src": s, "mute": 1 if (st["mute"] >> s) & 1 else 0})
        for b in range(8):
            api({"op": "set_send", "in": s, "bus": b, "gain": r["sends"][b]})
        for o in range(18):
            api({"op": "set_master", "src": s, "out": o, "gain": r["master"][o]})
    for i, e in enumerate(st["exp"]):
        api({"op": "set_expander", "src": i, "on": e["on"],
             "threshold_db": e["threshold_db"], "ratio": e["ratio"],
             "attack_ms": e["attack_ms"], "release_ms": e["release_ms"],
             "range_db": e["range_db"], "hold_ms": e["hold_ms"]})
    for i, c in enumerate(st["cmp"]):
        api({"op": "set_comp", "src": i, "on": c["on"],
             "threshold_db": c["threshold_db"], "ratio": c["ratio"],
             "attack_ms": c["attack_ms"], "release_ms": c["release_ms"],
             "makeup_db": c["makeup_db"]})
    for n, raw in st["bq"].items():
        if raw:
            alsa_set(st["bq_numids"][n], raw)
    api({"op": "set_automix_cfg", "on": st["amx"]["on"],
         "resp_ms": st["amx"]["resp_ms"]})
    for s in range(8):
        api({"op": "set_automix", "src": s,
             "on": st["amx"].get("members", [0]*18)[s]})
    api({"op": "set_vfocus", "on": st["vf"].get("on", 0)})
    api({"op": "set_insert_bypass",
         "on": st["ib"].get("mastering_on", 1)})
    for c, p in enumerate(st["midix"].get("chans", [])):
        api({"op": "midix_ctl", "cmd": "prog", "chan": c, "num": p})
    for t in range(3):
        api({"op": "set_tap", "tap": t, "kind": 0, "a": 0, "b": -1})
    api({"op": "set_tap", "tap": 3, "kind": 3, "a": 0, "b": 1})
    log("restore", "restauration terminée")

# ---------------------------------------------------------------- threads
def midi_loop():
    mid = str(HERE / "compo.mid")
    while not STOP.is_set():
        p = subprocess.Popen(["aplaymidi", "-p", "20:0", mid])
        while p.poll() is None:
            if STOP.is_set():
                p.terminate()
                break
            time.sleep(1)
        time.sleep(0.5)
    subprocess.run(["amidi", "-p", "hw:1,0,0", "-S",
                    "B0 7B 00 B1 7B 00 B2 7B 00 B3 7B 00 B9 7B 00"])

def stems_loop():
    files = [STEMS / "mix_novoice.wav", STEMS / "drums.wav",
             STEMS / "bass.wav", STEMS / "mix_novoice.wav"]
    i = 0
    while not STOP.is_set():
        f = files[i % len(files)]
        if not f.exists():
            f = STEMS / "mix_novoice.wav"
        p = subprocess.Popen(["pw-play", "--target", SINK,
                              "--channel-map", AUXMAP, str(f)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        while p.poll() is None:
            if STOP.is_set():
                p.terminate()
                break
            time.sleep(1)
        i += 1

def make_larsen_wav():
    # 22 s : sinus 2,4 kHz sur aux6 montant de -50 à -8 dB (croissance expo
    # ~2 dB/s = signature larsen), les autres canaux à zéro.
    fs, dur, f = 48000, 28.0, 2400.0
    n = int(fs * dur)
    t = np.arange(n) / fs
    db = np.minimum(-50 + t * (50 / 18.0), 0.0)      # rampe puis plateau 0 dB
    env = 10 ** (db / 20)
    x = (np.sin(2*np.pi*f*t) * env * 32767).astype(np.int16)
    data = np.zeros((n, 8), dtype=np.int16)
    data[:, 6] = x
    raw = data.tobytes()
    with open(HERE / "larsen.wav", "wb") as w:
        w.write(b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVE")
        w.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 8, fs, fs*16, 16, 16))
        w.write(b"data" + struct.pack("<I", len(raw)) + raw)

def larsen_burst():
    """Toutes les ~15 min : route U7→master, joue la rampe, mesure le temps
    de réaction de l'anti-larsen (1er notch), coupe, restaure."""
    time.sleep(240)
    solo = False
    while not STOP.is_set():
        solo = not solo
        r0 = api({"op": "get_strip_routing", "src": 14})
        dip = {}
        if solo:                       # larsen de balance : musique -18 dB
            for src in (8, 9, 16, 17):
                dip[src] = api({"op": "get_strip_routing", "src": src})["gain"]
                api({"op": "set_input_gain", "src": src, "gain": dip[src] * 0.12})
        api({"op": "set_master", "src": 14, "out": 0, "gain": 2.0})
        api({"op": "set_master", "src": 14, "out": 1, "gain": 2.0})
        log("larsen", f"burst début (2.4 kHz U7, solo={int(solo)})")
        p = subprocess.Popen(["pw-play", "--target", SINK,
                              "--channel-map", AUXMAP, str(HERE / "larsen.wav")],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t_start, t_notch, notch_info = time.time(), None, ""
        while p.poll() is None and not STOP.is_set():
            lr = http_get("/api/larsen", timeout=3)
            nt = lr.get("notches", [])
            if nt and t_notch is None:
                t_notch = time.time()
                notch_info = json.dumps(nt)[:120]
                p.terminate()
                break
            time.sleep(0.1)
        p.terminate()
        if t_notch:
            log("larsen", f"REACTION {t_notch - t_start:.2f}s notches={notch_info}")
        else:
            log("larsen", "PAS de notch pendant la rampe 22 s (-50→-8 dB)")
        api({"op": "set_master", "src": 14, "out": 0, "gain": r0["master"][0]})
        api({"op": "set_master", "src": 14, "out": 1, "gain": r0["master"][1]})
        for src, g in dip.items():
            api({"op": "set_input_gain", "src": src, "gain": g})
        for _ in range(90):          # release + attente inter-burst ~15 min
            if STOP.is_set(): return
            time.sleep(10)

def looper_cycle():
    """Toutes les ~10 min : piste 5 configurée sur U1 (stems), rec 4 s,
    lecture 90 s (glitch fin de boucle visible dans la capture), clear."""
    time.sleep(120)
    while not STOP.is_set():
        api({"op": "looper_track_cfg", "track": 5, "src_a": 8, "src_b": 9,
             "gain_db": -3.0})
        api({"op": "looper_track_ctl", "track": 5, "action": "rec"})
        log("looper", "rec 4 s depuis U1/U2")
        time.sleep(4)
        api({"op": "looper_track_ctl", "track": 5, "action": "play"})
        api({"op": "looper_ctl", "action": "play_all"})
        ls = api({"op": "looper_status"})
        ln = ls.get("tracks", [{}]*6)[5].get("len_s", 0)
        log("looper", f"play boucle {ln}s x ~20 tours")
        for _ in range(9):
            if STOP.is_set(): break
            time.sleep(10)
        api({"op": "looper_ctl", "action": "stop_all"})
        api({"op": "looper_track_ctl", "track": 5, "action": "clear"})
        log("looper", "clear")
        for _ in range(50):
            if STOP.is_set(): return
            time.sleep(10)

def changer():
    """Brasseur : une action aléatoire toutes les 8-25 s, journalisée."""
    rng = random.Random(1234)
    bq_state = save0["bq_numids"]
    while not STOP.is_set():
        time.sleep(rng.uniform(8, 25))
        a = rng.choices(
            ["fader", "eq", "gate", "comp", "automix", "vfocus",
             "mastering", "pad", "send", "prog", "mute"],
            weights=[22, 15, 10, 10, 8, 8, 8, 8, 6, 3, 2])[0]
        try:
            if a == "fader":
                s = rng.choice([8, 9, 16, 17])       # stems + compo
                g = rng.uniform(0.3, 1.1)
                api({"op": "set_input_gain", "src": s, "gain": g})
                log("chg", f"fader src{s} -> {g:.2f}")
            elif a == "eq":
                t = rng.randint(0, 3); ch = rng.choice([(1,(1,5,9)),(2,(2,6,10))])
                i = rng.choice(ch[1])
                f = rng.choice([150, 400, 900, 2500, 6000])
                g = rng.uniform(-8, 0)
                n = f"TAC{t} ADC BQ{i} Coefs"
                if n in bq_state:
                    alsa_set(bq_state[n], rbj_peaking(f, 1.5, g))
                    log("chg", f"eq {n} peak {f}Hz {g:.1f}dB")
            elif a == "gate":
                s = rng.randint(8, 15)
                on = rng.randint(0, 1)
                api({"op": "set_expander", "src": s, "on": on,
                     "threshold_db": rng.uniform(-70, -45)})
                log("chg", f"gate src{s} on={on}")
            elif a == "comp":
                s = rng.randint(8, 15)
                api({"op": "set_comp", "src": s, "on": rng.randint(0, 1),
                     "threshold_db": rng.uniform(-35, -20),
                     "ratio": rng.uniform(2, 6)})
                log("chg", f"comp src{s}")
            elif a == "automix":
                on = rng.randint(0, 1)
                api({"op": "set_automix_cfg", "on": on})
                log("chg", f"automix on={on}")
            elif a == "vfocus":
                on = rng.randint(0, 1)
                api({"op": "set_vfocus", "on": on})
                log("chg", f"vfocus on={on}")
            elif a == "mastering":
                on = rng.randint(0, 1)
                api({"op": "set_insert_bypass", "on": on})
                log("chg", f"mastering on={on}")
            elif a == "pad":
                api({"op": "sampler_trigger", "slot": 0})
                log("chg", "pad jingle")
            elif a == "send":
                s = rng.choice([8, 9, 16, 17]); b = rng.randint(0, 3)
                g = rng.uniform(0, 0.4)
                api({"op": "set_send", "in": s, "bus": b, "gain": g})
                log("chg", f"send src{s}->bus{b} {g:.2f}")
            elif a == "prog":
                c = rng.choice([0, 1, 3])
                p = rng.choice([0, 8, 33, 48, 52, 80])
                api({"op": "midix_ctl", "cmd": "prog", "chan": c, "num": p})
                log("chg", f"midix prog ch{c} -> {p}")
            elif a == "mute":
                s = rng.choice([8, 9])
                api({"op": "set_mute", "src": s, "mute": 1})
                time.sleep(rng.uniform(1, 3))
                api({"op": "set_mute", "src": s, "mute": 0})
                log("chg", f"mute pulse src{s}")
        except Exception as e:
            log("chg_err", f"{a}: {e}")

def capture_analyze():
    """Chunks 300 s ; détection de clics (2e différence vs médiane locale)
    sur les 2 premiers canaux ; garde les chunks suspects."""
    CH = 2
    while not STOP.is_set():
        t_start = time.time()
        fn = CAPDIR / f"cap_{int(t_start)}.wav"
        p = subprocess.Popen(["pw-record", "--target", SRC, "--channels", "8",
                              "--rate", "48000", "--format", "s32",
                              "--channel-map", AUXMAP, str(fn)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(300):
            if STOP.is_set(): break
            time.sleep(1)
        p.terminate(); p.wait()
        threading.Thread(target=analyze_chunk, args=(fn, t_start),
                         daemon=True).start()

def analyze_chunk(fn, t_start):
    try:
        import wave
        w = wave.open(str(fn), "rb")
        nfr = w.getnframes(); nch = w.getnchannels()
        data = np.frombuffer(w.readframes(nfr), dtype=np.int32)
        data = data.reshape(-1, nch)
        w.close()
        events = 0
        details = []
        for ch in range(2):
            x = data[:, ch].astype(np.float64)
            if len(x) < 48000: continue
            rms = np.sqrt(np.mean(x**2))
            if rms < 1000:   # silence total (<-66 dB rel 24 bits bas) : dropout ?
                log("cap", f"{fn.name} ch{ch} silencieux (rms={rms:.0f})")
                continue
            d2 = np.abs(np.diff(x, 2))
            k = 960                                   # 20 ms
            nwin = len(d2) // k
            loc = np.median(d2[:nwin*k].reshape(-1, k), axis=1) + 1.0
            th = np.repeat(loc * 60.0, k)             # 60x médiane locale
            d2c = d2[:len(th)]
            idx = np.where((d2c > th) & (d2c > rms * 0.6))[0]
            # regroupe en événements espacés > 10 ms, ignore bords 100 ms
            last = -99999
            for i in idx:
                if i < 4800 or i > len(d2c) - 4800: continue
                if i - last > 480:
                    ts = t_start + i / 48000.0
                    details.append((ch, ts, float(d2c[i] / (loc[min(i//k, nwin-1)]+1))))
                    events += 1
                last = i
        if events:
            for ch, ts, ratio in details[:20]:
                log("CLICK", f"{time.strftime('%H:%M:%S', time.localtime(ts))}"
                             f".{int(ts%1*1000):03d} ch{ch} ratio={ratio:.0f}"
                             f" fichier={fn.name}")
            log("cap", f"{fn.name}: {events} événements suspects → CONSERVÉ")
        else:
            fn.unlink(missing_ok=True)
            log("cap", f"{fn.name}: propre, supprimé")
    except Exception as e:
        log("cap_err", f"{fn.name}: {e}")

def monitor():
    prev = api({"op": "get_state"})
    while not STOP.is_set():
        for _ in range(30):
            if STOP.is_set(): return
            time.sleep(1)
        s = api({"op": "get_state"})
        if s.get("ok"):
            dx = s["xrun"] - prev.get("xrun", 0)
            dd = s.get("ring_drops", 0) - prev.get("ring_drops", 0)
            if dx or dd:
                log("XRUN", f"delta xrun={dx} drops={dd} iter={s['prof_iter_us']}")
            else:
                log("mon", f"xrun={s['xrun']} iter={s['prof_iter_us']}us ok")
            prev = s

def tap_probe():
    time.sleep(300)
    while not STOP.is_set():
        ib = api({"op": "get_insert_bypass"})
        out = ssh("python3 /root/tests/tap_rms.py /dev/imx-audio-tap-out",
                  timeout=20).strip().replace("\n", " | ")
        log("tap", f"{out} mastering_on={ib.get('mastering_on')}")
        for _ in range(60):
            if STOP.is_set(): return
            time.sleep(10)

# ---------------------------------------------------------------- main
log("run", f"MARATHON début, durée {DUR}s")
save0 = save_user_state()
with open(HERE / "user_state.json", "w") as f:
    json.dump({k: v for k, v in save0.items() if k != "bq_numids"} |
              {"bq_numids": save0["bq_numids"]}, f, default=str)
log("run", f"état utilisateur sauvegardé ({len(save0['bq'])} blobs EQ)")
make_larsen_wav()
# routing marathon : stems (8/9) + compo (16/17) vers le master 0/1
# (chemin mastering + tap FX + anti-larsen) ET vers les sorties USB 8/9
# (capture PC). Restauré intégralement en fin de run.
for src in (8, 9, 16, 17):
    for o, g in ((0, 0.5), (1, 0.5), (8, 0.7), (9, 0.7)):
        api({"op": "set_master", "src": src, "out": o, "gain": g})
log("run", "routing marathon posé (8/9/16/17 -> out0/1 + out8/9)")
s_start = api({"op": "get_state"})

# taps : 2 sur les entrées compo/stems, 3 = master (comme la GUI)
api({"op": "set_tap", "tap": 2, "kind": 1, "a": 16, "b": -1})
api({"op": "set_tap", "tap": 3, "kind": 3, "a": 0, "b": 1})

threads = [threading.Thread(target=f, daemon=True) for f in
           (midi_loop, stems_loop, larsen_burst, looper_cycle,
            changer, capture_analyze, monitor, tap_probe)]
for t in threads: t.start()

def finish(*_):
    STOP.set()
signal.signal(signal.SIGTERM, finish)
signal.signal(signal.SIGINT, finish)

t_end = T0 + DUR
while time.time() < t_end and not STOP.is_set():
    time.sleep(5)
STOP.set()
log("run", "arrêt des threads…")
for t in threads: t.join(timeout=30)
subprocess.run(["pkill", "pw-play"]); subprocess.run(["pkill", "pw-record"])
subprocess.run(["pkill", "aplaymidi"])
subprocess.run(["amidi", "-p", "hw:1,0,0", "-S",
                "B0 7B 00 B1 7B 00 B2 7B 00 B3 7B 00 B9 7B 00"])

s_end = api({"op": "get_state"})
log("run", f"BILAN xrun {s_start.get('xrun')} -> {s_end.get('xrun')} "
           f"(delta {s_end.get('xrun', 0) - s_start.get('xrun', 0)}), "
           f"drops delta {s_end.get('ring_drops', 0) - s_start.get('ring_drops', 0)}")
restore_user_state(save0)
log("run", "MARATHON terminé")
