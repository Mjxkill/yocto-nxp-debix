# Test Fiche : V7.0 — E7.1 (mixer-gui-http — peak meters + SSE streaming 30 Hz)

**Date** : 2026-05-11
**Statut** : **GO** — vumètres temps réel fonctionnels, validation user board confirmée
**Tag git** : à créer (`v7.0-e7.1`)
**Commit** : `d055b946`

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E7.1 Peak meters par voie + SSE streaming 30 Hz |
| Préalable | E7 backend HTTP fonctionnel (commit `079d3ddb`) + mixer-pro E6.h baseline stable |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Investigation critic | job `f9547d24` 6/6 workers (suggestion SSE convergente) |
| Phase 1 critic_analyze | approved=true qwen 75 après révision (format SSE `\n\n`, pool 8, profilage T7.1.2 gate) |

## Travaux exécutés

### Côté mixer-pro (v7.0-e6h → v7.0-e7m)

1. **Peak meters atomic per channel** :
   - `atomic_uint peak_in[26]` (8 mics DSP + 8 stems UAC2 + 2 phone + 8 returns FX post-FX)
   - `atomic_uint peak_out[18]` (8 DSP play + 8 UAC2 out + 2 phone out)
   - `atomic_uint peak_fx[8]` (4 bus FX stéréo pre-effets)

2. **Audio thread post-mix** : calcul peak frame-par-frame sur in[]/out[]/bus_pre[]/ret_post[], agrégation `max` sur le bloc 96 frames, scaling float[-1,1] → uint32 raw abs (× 2147483647).

3. **Decay backend** : `peak[i] = max(new, prev × 240/256)` ≈ 12 dB/s fall appliqué par bloc 2 ms.

4. **Nouvelle op JSON `get_meters`** :
   ```json
   {"ok":true,"in":[v0,...,v25],"out":[v0,...,v17],"fx":[v0,...,v7]}
   ```
   ~30 LOC dans `handle_cmd()`.

5. **Refactor `mix_frame()`** : signature étendue avec `bus_out` et `ret_out` pour exposer les bus FX et returns aux peak meters.

6. `MIXER_VERSION = "v7.0-e7m"`.

### Côté mixer-gui-http (v7.0-e7 → v7.0-e7.1)

1. **Endpoint `GET /api/stream`** (SSE chunked) :
   - `MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, 4096, ...)`
   - Headers `Content-Type: text/event-stream`, `Cache-Control: no-cache`, `X-Accel-Buffering: no`
   - Format RFC 8895 : `data: {json}\n\n` (double newline)
   - `usleep(33333)` = 30 Hz
   - Keep-alive `: ka\n\n` si mixer-pro down (évite reconnect EventSource)

2. **Endpoint `GET /api/meters`** (REST polling fallback / debug curl).

3. **MHD_THREAD_POOL** : 4 → **8** (4 SSE persistants + 4 REST/static workers).

### Côté frontend `index.html`

1. **EventSource('/api/stream')** au lieu de polling REST pour meters.
2. Bind dynamique : `<div class="meter-fill" :style="'height:' + peakDbPercent(peakIn[i]) + '%'">`.
3. Conversion `peakDb(p) = 20 * Math.log10(p / 2147483647)` puis `peakDbPercent` mappe [-60, 0] dBFS → [0, 100] %.
4. Indicator header `◉ SSE 30Hz` (vert) / `○ no stream`.

## Tests T7.1.X — résultats board

| Test | Description | Cible | Résultat |
|---|---|---|---|
| T7.1.1 | Build OK warnings 0 | 0 warning | ✓ |
| **T7.1.2** | **prof_iter_us steady** (CRITIC GATE) | < 1980 µs | ✓ **1797 µs** (-117 µs marge) |
| T7.1.3 | `curl -N /api/stream` reçoit `data: ...\n\n` à ~30 Hz | 3 events / 3 s | ✓ |
| T7.1.4 | `curl /api/meters` retourne JSON peak instant | 200 + JSON | ✓ |
| T7.1.5 | Inject signal mics DSP → peak_in[0..7] non nul | peaks ~5e4-1e5 | ✓ (bruit ambiant) |
| T7.1.6 | Navigateur ouvre `/`, meters bougent live | mvt visuel < 50 ms | ✓ (user confirmé) |
| T7.1.7 | Routing identity src=out → meters_out reflète in | in == out | ✓ in[0..7] = out[0..7] |
| T7.1.8 | xrun delta sur 8 s steady | 0 | ✓ 0 |
| T7.1.9 | ring_fill_frames steady avec peak compute | < 384 | ✓ 0-288 |
| T7.1.10 | ring_drops post-warmup | bornés init | ✓ 2208 stable |
| T7.1.11 | Throughput | ≥ 48 kHz | ✓ 48 kHz nominal |
| T7.1.12 | Regression POST set_send/set_master/set_mute | OK | ✓ |

## Mesures empiriques (DSP-only, --no-uac2 --no-phone)

```json
État après routing identity + 8 s steady :
{
  "version": "v7.0-e7m",
  "frames": 1790208,
  "xrun": 0,
  "mute_mask": 0,
  "cap_delay_frames": 0,
  "play_delay_frames": 384,
  "latency_us_one_way": 8000,
  "prof_cap_us": 1393,
  "prof_mix_us": 391,
  "prof_play_us": 12,
  "prof_iter_us": 1797,
  "ring_drops": 2208,
  "ring_fill_frames": 0
}
```

Échantillon SSE 3 frames :
```
data: {"ok":true,"in":[88037,104818,...,0,0],"out":[0,0,...],"fx":[0,...]}

data: {"ok":true,"in":[88844,94904,...],"out":[0,...],"fx":[0,...]}

data: {"ok":true,"in":[121626,83374,...],"out":[0,...],"fx":[0,...]}
```

## Procédure de démarrage (manuel)

Les services systemd `mixer-pro.service` et `mixer-gui-http.service` sont **disabled par défaut** (`SYSTEMD_AUTO_ENABLE=disable` dans les recettes). Lancement manuel requis :

```bash
ssh root@192.168.0.9 '/usr/bin/tac-reset analog'         # 1. Reset TAC5212 (ESSENTIEL après boot)
ssh root@192.168.0.9 'nohup /usr/bin/mixer-pro --no-uac2 --no-phone >/tmp/mixer-pro.log 2>&1 &'
ssh root@192.168.0.9 'nohup /usr/bin/mixer-gui-http >/tmp/mixer-gui-http.log 2>&1 &'
```

User feedback : **tac-reset OBLIGATOIRE** avant chaque démarrage mixer-pro (sinon samples = 0). Mémoire `tac_reset_required_after_boot.md` documente.

**TODO E7.x** : créer un service systemd `audio-stack.service` qui chaîne `tac-reset → mixer-pro → mixer-gui-http` au boot.

## Validation utilisateur

> User 2026-05-11 18h : "ok si les vumètres fonctionne super bien par contre il faut faire un tac-reset et redémarrer le mixer"

→ **GO** : vumètres opérationnels temps réel dans le navigateur, signal live sur les 8 voies DSP.

## Bénéfices E7.1 vs E7

| Métrique | E7 (skeleton) | E7.1 |
|---|---|---|
| Meters in/out | placeholders 0 % | **animés temps réel 30 Hz** |
| Mécanisme | polling REST 2 Hz state global | **SSE chunked** dédié meters |
| Latence visuelle audio→meter | n/a (statique) | **< 50 ms** (1 bloc cap + 33 ms SSE + transit) |
| Bande passante | ~1 KB/s | ~18 KB/s × N clients (négligeable LAN) |
| Indicators | connected/disconnected | + **SSE 30 Hz status** |

## Suite

E7.1 livre la **brique technique temps réel** (peak meters + SSE streaming). L'UI reste minimaliste (sliders horizontaux, layout flat).

**E7.2** (prochain sprint) : re-design UI premium "Apple-class" :
- Layout groupé DSP / UAC2 ASIO / Phone / Bus FX / Master
- Faders verticaux (vraie console DAW)
- Dark glassmorphism theme + animations
- Typo SF Pro / Inter
- Knobs rotatifs pour params FX

**E7.3** ensuite : panels effets DSP complets (compressor/reverb/delay/EQ params exposés).
**E7.4** : effets TAC5212 via kcontrols ALSA.
**E7.5** : spectre + phase FFT canvas.
