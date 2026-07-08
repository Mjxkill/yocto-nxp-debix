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
STATE=/var/lib/alsa/asound.state
CARD=softac5212tdm
WITNESS="TAC0 ADC Biquad Config"

# V13.1 (campagne de validation P6) : au boot le restore udev échoue
# (exit 99, carte/DSP pas prêts) et un restore unique silencieux peut
# échouer aussi ; ensuite le store débouncé de gui-http ÉCRASE
# asound.state avec l'état post-tac-reset → config TAC perdue à jamais.
# → copie de sûreté + retry avec VÉRIFICATION sur un contrôle témoin.
verify_witness() {
    want=$(sed -n "/name '$WITNESS'/,/}/{s/.*value '\(.*\)'/\1/p;}" "$STATE" | head -1)
    [ -n "$want" ] || return 0        # témoin absent du state → pas de vérif
    out=$(amixer -c "$CARD" cget "iface=MIXER,name='$WITNESS'" 2>/dev/null)
    got=$(echo "$out" | sed -n 's/^ *: values=\([0-9]*\).*/\1/p')
    idx=$(echo "$out" | sed -n "s|.*Item #\([0-9]*\) '$want'.*|\1|p")
    [ -n "$got" ] && [ "$got" = "$idx" ]
}

if [ $# -ge 1 ] && [ -d "$1" ]; then
    SCN="$1"
    if [ -f "$SCN/asound.state" ]; then
        alsactl restore -f "$SCN/asound.state" 2>&1 | grep -vi ucm || true
    fi
    [ -d "$SCN/dsp-blobs" ] && BLOB_DIR="$SCN/dsp-blobs"
else
    [ -f "$STATE" ] && cp "$STATE" "$STATE.boot"
    i=0
    while [ $i -lt 5 ]; do
        alsactl restore 2>&1 | grep -vi ucm || true
        verify_witness && { echo "fx-restore: alsactl vérifié (essai $((i+1)))"; break; }
        i=$((i+1))
        echo "fx-restore: vérif témoin KO, retry $i/5"
        sleep 2
    done
    [ $i -ge 5 ] && echo "fx-restore: ECHEC restore TAC après 5 essais (état sûr: $STATE.boot)"
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
