#!/bin/bash
# V4.2 + SOF Probes regression test
# Tests:
#   T1 record 8ch (SAI_Capture, hw:2,0)
#   T2 play 8ch siren (SAI_Playback, hw:2,1)
#   T3 duplex play + record simultaneous
#   T4 SOF probes infra alive (compr device + debugfs)
#   T5 dmesg errors / xrun analysis
set +e

DEV_REC="hw:2,0"
DEV_PLAY="hw:2,1"
CH=8
FMT="S32_LE"
RATE=48000
DUR=5
SIREN=/root/tests/siren.wav
OUT=/root/tests
PASS=0
FAIL=0

ok() { echo "  -> OK"; PASS=$((PASS+1)); }
ko() { echo "  *** FAIL: $1 ***"; FAIL=$((FAIL+1)); }

echo "=========================================="
echo " V4.2 + SOF Probes regression test"
echo " $(date)"
echo "=========================================="

# Generate siren if missing
if [ ! -f "$SIREN" ]; then
  echo "[setup] Generating $SIREN ..."
  sox -n -c $CH -r $RATE -b 32 "$SIREN" \
    synth 5 sine 300:800 \
    synth 5 sine 800:300
fi

echo ""
echo "[T1] Record 8ch ${DUR}s on $DEV_REC ..."
arecord -D $DEV_REC -c $CH -f $FMT -r $RATE -d $DUR "$OUT/test_t1_rec.wav" 2>&1 | tail -2
RC=$?
SIZE=$(stat -c%s "$OUT/test_t1_rec.wav" 2>/dev/null || echo 0)
[ $RC -eq 0 ] && [ "$SIZE" -gt 100000 ] && ok || ko "rc=$RC size=$SIZE"

sleep 1

echo ""
echo "[T2] Play siren 8ch on $DEV_PLAY ..."
aplay -D $DEV_PLAY "$SIREN" 2>&1 | tail -2
RC=$?
[ $RC -eq 0 ] && ok || ko "aplay rc=$RC"

sleep 1

echo ""
echo "[T3] Duplex: aplay $DEV_PLAY + arecord $DEV_REC simultaneously ..."
arecord -D $DEV_REC -c $CH -f $FMT -r $RATE -d 6 "$OUT/test_t3_duplex.wav" 2>/dev/null &
REC_PID=$!
sleep 0.3
aplay -D $DEV_PLAY "$SIREN" 2>&1 | tail -2
PLAY_RET=$?
wait $REC_PID 2>/dev/null
REC_RET=$?
SIZE=$(stat -c%s "$OUT/test_t3_duplex.wav" 2>/dev/null || echo 0)
echo "  aplay rc=$PLAY_RET, arecord rc=$REC_RET, capture size=$SIZE"
[ $PLAY_RET -eq 0 ] && [ $REC_RET -eq 0 ] && [ "$SIZE" -gt 100000 ] && ok || ko "duplex"

sleep 1

echo ""
echo "[T4] SOF Probes infra check ..."
[ -e /sys/kernel/debug/sof/probe_points ] && echo "  /sys/kernel/debug/sof/probe_points present" || ko "probe_points missing"
[ -e /sys/kernel/debug/sof/probe_points_remove ] && echo "  /sys/kernel/debug/sof/probe_points_remove present" || ko "probe_points_remove missing"
COMPR=$(ls /dev/snd/compr* 2>/dev/null | head -1)
[ -n "$COMPR" ] && echo "  compress device: $COMPR" || ko "no compr device"
lsmod | grep -q snd_sof_imx_probes && echo "  snd_sof_imx_probes loaded" || ko "snd_sof_imx_probes not loaded"
lsmod | grep -q snd_sof_probes && echo "  snd_sof_probes loaded" || ko "snd_sof_probes not loaded"
ls /sys/bus/auxiliary/drivers/snd_sof_probes/snd_sof.imx-probes.0 >/dev/null 2>&1 && echo "  driver bound to imx-probes.0" || ko "driver not bound"
echo "  -> probes infra: OK (if no FAIL above)"

echo ""
echo "[T5] dmesg errors after tests ..."
# Exclude expected boot info messages (deferred probe = normal bootstrap order)
ERRS=$(dmesg | grep -iE "xrun|underrun|overrun|ipc.*error|pcm.*error|sof.*err|probe.*fail" \
  | grep -v sdma-imx7d \
  | grep -v "binding deferred" \
  | grep -v "Parent card not yet available" \
  | tail -10)
if [ -z "$ERRS" ]; then
  echo "  no errors detected"
  PASS=$((PASS+1))
else
  echo "$ERRS"
  ko "dmesg errors detected"
fi

echo ""
echo "[stats] noise floor analysis"
echo "--- T1 record solo ---"
sox "$OUT/test_t1_rec.wav" -n stats 2>&1 | head -6
echo "--- T3 duplex record ---"
sox "$OUT/test_t3_duplex.wav" -n stats 2>&1 | head -6

echo ""
echo "=========================================="
echo " Result: $PASS pass / $FAIL fail"
echo "=========================================="
exit $FAIL
