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

OLDIFS="$IFS"

# Boucle infinie : poll + bump si nécessaire
while true; do
    IFS='
'
    for pat in $PATTERNS; do
        pid=$(pgrep -f "irq/.*$pat" | head -1)
        [ -z "$pid" ] && continue   # silent miss, retry next poll

        # Lit la prio courante via chrt -p
        cur_prio=$(chrt -p "$pid" 2>/dev/null | awk -F': ' '/priority:/ {print $2}' | head -1)
        [ "$cur_prio" = "$PRIO" ] && continue   # déjà bonne prio

        if chrt -f -p $PRIO "$pid" 2>/dev/null; then
            echo "$(date +%H:%M:%S) BUMP $pat (PID $pid) ${cur_prio:-?} -> $PRIO"
        fi
    done
    IFS="$OLDIFS"

    sleep $POLL_INTERVAL
done
