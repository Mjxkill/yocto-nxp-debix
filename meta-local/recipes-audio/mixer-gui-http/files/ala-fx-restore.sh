#!/bin/sh
# V10-N7b — Restauration des effets au boot, APRÈS le tac-reset (qui écrase
# les registres TAC restaurés trop tôt par alsa-restore) :
#   1. alsactl restore  : re-applique TAC/PGA (INTEGER/ENUM) sauvés
#   2. replay des blobs DSP BYTES (DRC/MULTIBAND) — ignorés par alsactl —
#      via l'endpoint gui-http (même chemin de code que l'application
#      manuelle depuis les GUIs)
# V13-SCENES E2 — argument optionnel $1 = répertoire de SCÈNE contenant
#   asound.state + dsp-blobs/ : restaure depuis la scène au lieu de l'état
#   courant (utilisé par POST /api/scene/recall).
set -u
BLOB_DIR=/var/lib/mixer-pro/dsp-blobs

if [ $# -ge 1 ] && [ -d "$1" ]; then
    SCN="$1"
    if [ -f "$SCN/asound.state" ]; then
        alsactl restore -f "$SCN/asound.state" 2>/dev/null || true
    fi
    [ -d "$SCN/dsp-blobs" ] && BLOB_DIR="$SCN/dsp-blobs"
else
    alsactl restore 2>/dev/null || true
fi

[ -d "$BLOB_DIR" ] || exit 0
for f in "$BLOB_DIR"/*.hex; do
    [ -f "$f" ] || continue
    numid=$(basename "$f" .hex)
    hex=$(cat "$f")
    ok=$(curl -s -m 10 -X POST http://127.0.0.1:8080/api/dsp/blob/set \
         -H "Content-Type: application/json" \
         -d "{\"numid\":$numid,\"hex\":\"$hex\"}")
    case "$ok" in
        *'"ok":true'*) echo "fx-restore: blob $numid OK" ;;
        *)             echo "fx-restore: blob $numid ECHEC ($ok)" ;;
    esac
done
exit 0
