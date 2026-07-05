#!/bin/sh
# V9.1 — Daemon bump audio-critical IRQ kthreads to RT prio 90
#
# Sous PREEMPT_RT, chaque IRQ devient un kthread (irq/N-name) schedulable.
# Le default systemd PREEMPT_RT met les IRQ kthreads à prio 50.
# Notre audio_thread est à prio 80 → quand audio_thread tourne sur le core
# de l'IRQ kthread, ce dernier est préempté → snd_pcm_readi attend la
# notification → pics 2-5 ms occasionnels (priority inversion classique).
#
# Solution : passer les IRQ critiques (mailbox A53↔DSP, SDMA audio, USB)
# à prio 90 (> audio_thread 80).
#
# Test empirique V9.1 (Tauri MP3 90s) : 17 → 3 pics > 3 ms = -82%.
#
# DAEMON MODE : le mailbox IRQ kthread est respawné quand SOF se ré-init
# (PCM close/open). Un one-shot au boot ne suffit pas. Ce daemon poll
# toutes les POLL_INTERVAL secondes et re-bump si nouveau PID détecté
# (ou si prio drift de 90).

PATTERNS="30e60000.mailbox\\[3-0\\]
30e60000.mailbox\\[3-1\\]
30bd0000.dma-controller
30e10000.dma-controller
32f10100.usb
32f10108.usb"

PRIO=90
POLL_INTERVAL=5     # secondes entre 2 polls

echo "IRQ-PRIO-RT daemon started (prio $PRIO, poll ${POLL_INTERVAL}s)"

# V10-N8 — gouverneur cpufreq performance : élimine les dips 1.2 GHz et la
# latence de remontée ondemand (jank GUI au début de chaque interaction,
# mesure 2026-07-05 ; thermique 65-68°C, marge OK). Appliance audio.
for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do
    echo performance > "$g" 2>/dev/null
done
echo "cpufreq -> performance"

OLDIFS="$IFS"

# V10-N8 — cache des PID : les 6 pgrep -f par poll scannaient tout /proc
# toutes les 5 s = bouffées CPU périodiques sur les cores 0-1 (mesuré,
# « pompe à vélo » vue par l'utilisateur sur la page SYSTÈME). En régime
# établi on ne fait plus que 6 lectures de /proc/<pid>/stat (champ 40 =
# rt_priority) ; le pgrep ne re-tourne que si un PID a disparu ou dérivé.
# Boucle infinie : poll + bump si nécessaire
while true; do
    IFS='
'
    i=0
    for pat in $PATTERNS; do
        i=$((i + 1))
        eval "pid=\$CACHED_PID_$i"
        if [ -n "$pid" ] && [ -r "/proc/$pid/stat" ]; then
            rtp=$(awk '{print $40}' "/proc/$pid/stat" 2>/dev/null)
            [ "$rtp" = "$PRIO" ] && continue   # cache OK, zéro scan
        fi
        pid=$(pgrep -f "irq/.*$pat" | head -1)
        [ -z "$pid" ] && { eval "CACHED_PID_$i="; continue; }

        if chrt -f -p $PRIO "$pid" 2>/dev/null; then
            echo "$(date +%H:%M:%S) BUMP $pat (PID $pid) -> $PRIO"
            eval "CACHED_PID_$i=$pid"
        fi
    done
    IFS="$OLDIFS"

    sleep $POLL_INTERVAL
done
