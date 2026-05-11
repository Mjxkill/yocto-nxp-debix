#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# V7.0-E6.c — Stoppe les alsaloop lancés par alsa-route-start.sh

RUNDIR=/run/alsa-route-bridge

if [ ! -d "$RUNDIR" ]; then
    exit 0
fi

for pidfile in "$RUNDIR"/*.pid; do
    [ -f "$pidfile" ] || continue
    pid=$(cat "$pidfile" 2>/dev/null)
    name=$(basename "$pidfile" .pid)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        echo "alsa-route-stop: killing $name (pid $pid)"
        kill -TERM "$pid" 2>/dev/null
    fi
    rm -f "$pidfile"
done

exit 0
