#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# V7.0-E6.c — Démarre N alsaloop selon /etc/alsa-route-bridge.conf
#
# Chaque ligne non-commentée du fichier conf représente une route ALSA
# (capture_pcm → playback_pcm). Le script lance un `alsaloop` par route
# en background et stocke les PIDs dans /run/alsa-route-bridge/<name>.pid
# pour que le stop-script puisse les killer proprement.

set -u

CONF=/etc/alsa-route-bridge.conf
RUNDIR=/run/alsa-route-bridge

if [ ! -f "$CONF" ]; then
    echo "alsa-route-start: ERROR - $CONF not found" >&2
    exit 1
fi

mkdir -p "$RUNDIR"

# Compter les routes actives (lignes non-vides non-commentées)
active=$(grep -Ec '^[[:space:]]*[^#[:space:]]' "$CONF" || true)
if [ "$active" -eq 0 ]; then
    echo "alsa-route-start: no active routes in $CONF, nothing to do"
    exit 0
fi

echo "alsa-route-start: starting $active route(s) from $CONF"

# Parse chaque ligne active
grep -E '^[[:space:]]*[^#[:space:]]' "$CONF" | while IFS='|' read -r name cap play ch rate fmt extra; do
    # Trim whitespace
    name=$(echo "$name" | tr -d ' ')
    cap=$(echo "$cap" | tr -d ' ')
    play=$(echo "$play" | tr -d ' ')
    ch=$(echo "$ch" | tr -d ' ')
    rate=$(echo "$rate" | tr -d ' ')
    fmt=$(echo "$fmt" | tr -d ' ')

    if [ -z "$name" ] || [ -z "$cap" ] || [ -z "$play" ]; then
        echo "alsa-route-start: WARN skipping malformed line: $name|$cap|$play"
        continue
    fi

    echo "alsa-route-start: $name : $cap -> $play (${ch}ch ${rate}Hz $fmt)"
    /usr/bin/alsaloop -C "$cap" -P "$play" -c "$ch" -r "$rate" -f "$fmt" $extra > "$RUNDIR/$name.log" 2>&1 &
    echo $! > "$RUNDIR/$name.pid"
done

# Tous les alsaloop forkent en background ; le main script exit immédiatement.
# Le service systemd Type=forking attend les PIDs des enfants.
exit 0
