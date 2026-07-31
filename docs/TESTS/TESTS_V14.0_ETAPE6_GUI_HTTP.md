# TESTS V14.0 — Étape 6 : mixer-gui-http.c → 5 modules

Date : 2026-07-31. Architecture : `docs/ARCHI/ARCHI_V14_RESTRUCTURATION.md` §5.
`mixer-gui-http.c` (1 654 lignes) supprimé, remplacé par 5 modules + header
interne `gui_http.h` (defines + protos + struct sclient/post_buf partagés).

| Fichier | Lignes | Contenu |
|---|---:|---|
| `http_core.c` | 177 | main + démarrage MHD (pool 16) + signaux + helpers HTTP (CORS, send_*, mime, chemins statiques) + mlog |
| `mixer_bridge.c` | 65 | requête JSON vers /run/mixer-pro.sock (connect-per-request) |
| `sse_state.c` | 335 | flux d'état versionné V10-P1 (producteur 30 Hz, ring/client, patches par section) + sysload |
| `alsa_ctl.c` | 344 | amixer whitelist + tac-reset + blobs TLV SOF + alsactl store débouncé |
| `api_routes.c` | 706 | on_request : routage GET/POST complet |
| `gui_http.h` | 131 | interne (jamais installé) |

## Validation board (binaire `ca3d4f75`)

| Test | Résultat |
|---|---|
| Build | 0 erreur (LIC_CHKSUM re-pointé http_core.c ; warnings format-truncation préexistants, lignes décalées) ✓ |
| /health, /, /beta, /api/state | 200 partout, racine = console A.L.A. ✓ |
| Bridge : POST /api/cmd get_state | v14.0-modules relayé ✓ |
| SSE meters /api/stream | data: 30 Hz avec VU vivants ✓ |
| SSE état /api/state/sse | full + patches par section + **sysload cpu[73,42,25,14] gpu 35 npu 8 dsp 73** ✓ |
| /api/sysload | « warming up » tant qu'aucun client état ne s'est connecté (comportement d'ORIGINE : calculé dans le cold-path du producteur), OK après ✓ |
| /api/alsa/contents (whitelist) | numid list servie ✓ |

## Test utilisateur : EN ATTENTE (GUI beta complète au navigateur)

## Reste

Étape 7 : beta.html (4 100) → HTML + CSS + JS locaux. Étape 8 : QML.
