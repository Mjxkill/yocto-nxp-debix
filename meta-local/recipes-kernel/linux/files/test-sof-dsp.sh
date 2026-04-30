#!/bin/sh
# Frequency sweep + FFT harmonic analysis for SOF DSP audio loopback
# Plays sine tones, records via loopback, analyzes fundamental + harmonics
# Usage: test-sof-dsp.sh [pas_hz]
#   pas_hz: frequency step (50, 100, 500, or 1000)
#
# IMPORTANT: Connect a loopback cable from TAC0 OUT to TAC0 IN before running

STEP=${1:-100}
DEVICE=hw:2,0
RATE=48000
NCH=8
AMP=0.25
DUR=2

case "$STEP" in
    50)   RANGE="50 100 150 200 300 500 800 1000 1500 2000 3000 5000 8000 12000 16000 20000" ;;
    100)  RANGE="100 200 300 400 500 1000 2000 3000 5000 8000 12000 16000 20000" ;;
    500)  RANGE="500 1000 1500 2000 2500 3000 4000 5000 8000 10000 15000 20000" ;;
    1000) RANGE="1000 2000 3000 4000 5000 8000 10000 15000 20000" ;;
    *)    echo "Usage: $0 [50|100|500|1000]"; exit 1 ;;
esac

mkdir -p /tmp/sof_test
cd /tmp/sof_test

echo "=== Generating tone files ==="
python3 << PYEOF
import wave, struct, math
freqs = [$(echo $RANGE | tr ' ' ',')]
amp = int($AMP * 2147483647)
rate = $RATE
dur = $DUR
nch = $NCH
for f in freqs:
    w = wave.open(f"/tmp/sof_test/tone_{f}.wav", "w")
    w.setnchannels(nch); w.setsampwidth(4); w.setframerate(rate)
    for i in range(rate * dur):
        val = int(amp * math.sin(2 * math.pi * f * i / rate))
        w.writeframes(struct.pack(f"<{nch}i", val, val, 0, 0, 0, 0, 0, 0))
    w.close()
print(f"Generated {len(freqs)} tone files")
PYEOF

echo ""
echo "=== Playing + recording each frequency (loopback required) ==="
for addr in 0x50 0x51 0x52 0x53; do
    i2cset -f -y 3 $addr 0x67 0xc9 2>/dev/null
    i2cset -f -y 3 $addr 0x6e 0xc9 2>/dev/null
done

for f in $RANGE; do
    aplay -D $DEVICE /tmp/sof_test/tone_${f}.wav 2>/dev/null &
    PLAY=$!
    sleep 0.3
    arecord -D $DEVICE -c $NCH -f S32_LE -r $RATE -d 1 /tmp/sof_test/rec_${f}.wav 2>/dev/null
    wait $PLAY
done

echo ""
echo "=== FFT analysis ==="
python3 << 'PYEOF'
import wave, struct, math, glob, re

def goertzel(samples, f, rate):
    N = len(samples)
    k = int(0.5 + N * f / rate)
    omega = 2 * math.pi * k / N
    coeff = 2 * math.cos(omega)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + coeff * s1 - s2
        s2 = s1; s1 = s0
    power = s1*s1 + s2*s2 - coeff*s1*s2
    return math.sqrt(power) / (N / 2) if power > 0 else 0

def to_dbfs(amp):
    if amp < 1:
        return -200.0
    return 20 * math.log10(amp / 2147483647)

files = sorted(glob.glob("/tmp/sof_test/rec_*.wav"),
               key=lambda x: int(re.search(r'rec_(\d+)', x).group(1)))

print(f"{'Freq':>7} {'Fund':>8} {'H2':>7} {'H3':>7} {'H4':>7} {'H5':>7} {'THD':>7}  Status")
print("-" * 70)

for fpath in files:
    f_sent = int(re.search(r'rec_(\d+)', fpath).group(1))
    w = wave.open(fpath, "r")
    nch, nf = w.getnchannels(), w.getnframes()
    rate = w.getframerate()
    w.setpos(int(rate * 0.1))
    samples_n = min(4096, nf - int(rate * 0.1))
    d = w.readframes(samples_n)
    s = struct.unpack(f"<{samples_n*nch}i", d)
    ch0 = list(s[0::nch])
    w.close()
    # Hann window
    N = len(ch0)
    for i in range(N):
        ch0[i] = int(ch0[i] * (0.5 - 0.5 * math.cos(2 * math.pi * i / N)))
    # Fundamental
    fund_amp = goertzel(ch0, f_sent, rate) * 2
    fund_db = to_dbfs(fund_amp)
    # Harmonics 2-5
    harmonics = []
    thd_sum = 0
    for h in range(2, 6):
        f_h = f_sent * h
        if f_h >= rate / 2:
            harmonics.append(-200.0)
            continue
        h_amp = goertzel(ch0, f_h, rate) * 2
        h_db = to_dbfs(h_amp)
        harmonics.append(h_db)
        if h_amp > 0 and fund_amp > 0:
            thd_sum += (h_amp / fund_amp) ** 2
    thd_db = 20 * math.log10(math.sqrt(thd_sum)) if thd_sum > 0 else -200.0
    # Status
    if fund_db < -80:
        status = "NO SIGNAL"
    elif thd_db > -40:
        status = "HIGH THD"
    elif thd_db > -60:
        status = "some THD"
    else:
        status = "CLEAN"
    print(f"{f_sent:5d}Hz {fund_db:+7.1f} {harmonics[0]:+6.1f} {harmonics[1]:+6.1f} "
          f"{harmonics[2]:+6.1f} {harmonics[3]:+6.1f} {thd_db:+6.1f}  {status}")
print()
print("Fund = fundamental (dBFS), H2-H5 = harmonics, THD = total harmonic distortion")
PYEOF

rm -f /tmp/sof_test/tone_*.wav
echo ""
echo "Recordings kept in /tmp/sof_test/rec_*.wav"
