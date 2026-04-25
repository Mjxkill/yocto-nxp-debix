# audio-tools

Userspace tools for testing/debugging the V4.2 SOF audio stack on the
Debix Model AB board (i.MX8MP, SAI7 TDM 8ch S32_LE @ 48 kHz, TAC5212
codecs).

## Files

| File | Purpose |
|------|---------|
| `loopback-c.c`           | Low-latency 8ch full-duplex loopback (capture → playback). VU meter overlay. |
| `vu8.c`                  | 8ch VU meter on capture only (no playback). Useful to debug channel signal levels. |
| `loopback-pipe.sh`       | Simple `arecord \| aplay` pipe loopback (no compilation needed). Latency tunable. |
| `sof-test-v42-probes.sh` | V4.2 + SOF Probes regression test (T1-T5: record / play / duplex / probes infra / dmesg). |

## Build (native gcc on the board)

After deploy, the C tools are compiled in place on the board:

```sh
# headers if missing (extract from yocto sysroot):
# tar xzf alsa-headers.tgz -C /usr/include/   # ships alsa/*.h

ln -sf libasound.so.2 /usr/lib/libasound.so   # only first time

gcc -O2 -Wall loopback-c.c -lasound -lm -o /root/tests/loopback-c
gcc -O2 -Wall vu8.c       -lasound -lm -o /root/tests/vu8
```

## Run

```sh
# Always run after boot:
/usr/bin/tac-reset

# 8ch full-duplex loopback, default low latency (period 64, n_periods 2)
/root/tests/loopback-c

# Stable 16ms latency, channel reversal disabled
/root/tests/loopback-c 256 3 0

# Tone sanity check (1 kHz on 8ch, ignore capture)
/root/tests/loopback-c 256 3 0 1

# VU meter only
/root/tests/vu8

# Pipe loopback (no compile needed)
/root/tests/loopback-pipe.sh 30      # 30 ms latency target

# Regression test
/root/tests/sof-test-v42-probes.sh
```

## Known limitations

- Userspace full-duplex has occasional xrun (drift between independent
  capture/playback DSP pipelines). Expected; the proper fix is
  DSP-internal routing — see project memory `project_mixer_goal.md`.
- TAC5212 codecs require `/usr/bin/tac-reset` after every boot to leave
  mute/power-down state — see project memory `tac_reset_required_after_boot.md`.
- The full-duplex pattern requires `snd_pcm_link()` + capture started
  *before* playback; otherwise capture is silent. See
  `sof_imx_full_duplex_userspace.md`.
