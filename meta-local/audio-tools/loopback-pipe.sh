#!/bin/bash
# Simple low-latency loopback hw:2,0 -> hw:2,1 via arecord | aplay
# Usage: loopback-pipe.sh [latency_ms]
#   default 30 ms (good compromise)
#   minimum stable observed: ~10 ms (at smaller values overrun risk increases)
#
# Total userspace latency ≈ 2 * latency_ms + pipe buffer (~21 ms typical)
# Add ~5 ms DSP pipeline + ~2 ms ADC/DAC = effective audible delay
#
# Stop with Ctrl+C.

LAT_MS="${1:-30}"
CDEV="hw:2,0"
PDEV="hw:2,1"
CH=8
FMT=S32_LE
RATE=48000

# buffer = LAT_MS * RATE / 1000  (frames per channel)
BUF_FRAMES=$((LAT_MS * RATE / 1000))
PER_FRAMES=$((BUF_FRAMES / 4))   # 4 periods per buffer

# convert frames to microseconds for arecord/aplay --buffer-time / --period-time
BUF_US=$((BUF_FRAMES * 1000000 / RATE))
PER_US=$((PER_FRAMES * 1000000 / RATE))

echo "=== Pipe loopback ==="
echo " capture : $CDEV"
echo " playback: $PDEV"
echo " format  : $FMT  rate=$RATE  channels=$CH"
echo " buffer  : $BUF_FRAMES frames ($BUF_US us = ${LAT_MS} ms per side)"
echo " period  : $PER_FRAMES frames ($PER_US us)"
echo "Ctrl+C to stop."
echo ""

# -B = buffer time us; -F = period time us
exec sh -c "arecord -D $CDEV -c $CH -f $FMT -r $RATE -B $BUF_US -F $PER_US - 2>/dev/null \
    | aplay -D $PDEV -c $CH -f $FMT -r $RATE -B $BUF_US -F $PER_US - 2>/dev/null"
