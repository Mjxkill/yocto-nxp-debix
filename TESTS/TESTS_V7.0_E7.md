# Test Fiche : V7.0 — E7 (GUI HTTP V7.0 — daemon mixer-gui-http + frontend Alpine.js skeleton)

**Date** : 2026-05-11
**Statut** : **GO** — backend REST testé OK
**Commit** : `079d3ddb mixer-gui-http: V7.0-E7 — backend HTTP C (libmicrohttpd) + frontend skeleton`
**Branche** : `feature/v7.0-multiband-drc-tap`

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E7 GUI HTTP base |
| Préalable | E6.h GO (mixer-pro 2-thread + ring SPSC 8 périodes, latence 14 ms) |
| Daemon `mixer-pro` | v7.0-e6.h actif sur la carte |
| Daemon `mixer-gui-http` | v7.0-e7 nouveau |
| Topology | inchangée |
| Firmware SOF | inchangé |

## Architecture livrée

- **Recipe** : `meta-local/recipes-audio/mixer-gui-http/`
- **Stack** : libmicrohttpd (recette OE), thread pool 8, connect-per-request.
- **Frontend** : `/var/www/mixer-gui/index.html` — single-page Alpine.js + Tailwind CDN, glassmorphism dark.
- **Bridge** : pour chaque requête, connecte une socket Unix `/run/mixer-pro.sock` et forward la JSON.

## Endpoints exposés

| Route | Méthode | Comportement |
|---|---|---|
| `/` ou `/index.html` | GET | Sert le fichier statique HTML |
| `/health` | GET | `{"ok":true}` pour readiness |
| `/api/state` | GET | proxy `{"op":"get_state"}` vers mixer-pro |
| `/api/cmd` | POST | forward du body JSON brut vers mixer-pro |
| OPTIONS | * | CORS preflight `Access-Control-Allow-Origin: *` |

## Tests T7.X — exécutés board

| Test | Résultat |
|---|---|
| T7.1 build recipe | ✓ exit 0, binaire ~75 KB |
| T7.2 systemctl start mixer-gui-http | ✓ écoute 0.0.0.0:8080 |
| T7.3 GET /health | ✓ `{"ok":true}` |
| T7.4 GET /api/state | ✓ retourne le JSON de mixer-pro (version, frames, latency_us) |
| T7.5 POST /api/cmd `{"op":"set_send","in":0,"bus":0,"gain":0.5}` | ✓ propage à mixer-pro, écho ok |
| T7.6 set_mute / set_master / set_fx_param / reset_fx | ✓ tous OK |
| T7.7 mute_mask state propagation | ✓ confirmé |
| T7.8 CORS OPTIONS depuis browser distant | ✓ accepté |

## Validation utilisateur

> "ça fonctionne" — 2026-05-11

→ **GO**.

## Suite

- E7.1 = peak meters + SSE streaming temps réel 30 Hz
- E7.2 = UI premium "Apple-class" (glassmorphism, faders DAW)
- E7.3 = panels effets DSP complets
- E7.4 = effets TAC5212 + DSP DRC editor
- E7.5 = analyzer FFT + scope
- E7.6 = documentation consolidée
- E7.7 = polish UX (returns to tags, R1..R8 clickable)
