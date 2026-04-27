#!/usr/bin/env python3
"""Apply NPU tap V3.2.2 device tree modifications to imx8mp-evk.dts.

Run after apply-tac5212-dt.py at do_patch:append.

Three modifications, all via DT overrides (no edit of imx8mp.dtsi SoC-level):
  1. Override &dsp_reserved_heap to reduce its size : 0xef0000 → 0xeb0000
     (-256 KB) so npu_tap_buffer@942b0000 can be carved without overlap.
  2. Add npu_tap_buffer@942b0000 in &{/reserved-memory} (256 KB no-map).
  3. Add imx_audio_tap node at root with compatible="electrosens,imx-audio-tap"
     and memory-region=<&npu_tap_buffer> for the kernel module to bind.

R7 sentinelle mono-DAI : V3.2.2 firmware uses ONE shared 256 KB tap buffer.
M6 limitation Phase 2 : if multi-DAI playback simultaneous needed, this DT
will need to be refactored with per-DAI buffer carve (4 zones).
"""
import sys

dts_path = sys.argv[1]
with open(dts_path, 'r') as f:
    content = f.read()

if 'npu_tap_buffer' in content:
    print("NPU tap DT changes already applied, skipping")
    sys.exit(0)

# All three modifications appended at the end of the file as DT overrides.
# This is the recommended pattern for layered DTs : keep base SoC dtsi
# unchanged, override only what we need in the board-level dts.
npu_tap_overlay = """
/* V3.2.2 NPU tap — DT overlay (added by apply-npu-tap-dt.py) */

/* (1) Reduce dsp_reserved_heap by 256 KB to make room for npu_tap_buffer.
 *     Original: 0x93400000, size 0xef0000  (15.875 MB)
 *     Modified: 0x93400000, size 0xeb0000  (15.687 MB, -256 KB)
 *     Freed range: 0x942b0000-0x942effff = 256 KB for npu_tap_buffer.
 */
&dsp_reserved_heap {
\treg = <0 0x93400000 0 0xeb0000>;
};

/* (2) NPU tap shared buffer carve — 256 KB no-map at 0x942b0000.
 *     Hors SOF MEMORY{} (au-delà de 0x93400000), dans cacheattr region 4
 *     (write-through cacheable, accessible DSP HiFi4 et A53). Pile avant
 *     vdev0vring0@942f0000.
 */
&{/reserved-memory} {
\tnpu_tap_buffer: npu_tap_buffer@942b0000 {
\t\tcompatible = "shared-dma-pool";
\t\treg = <0 0x942b0000 0 0x40000>;
\t\tno-map;
\t};
};

/* (3) NPU tap platform node — kernel module imx-audio-tap binds here.
 *     R7 sentinelle mono-DAI : un seul DAI playback peut être tapé.
 */
/ {
\timx_audio_tap: imx_audio_tap {
\t\tcompatible = "electrosens,imx-audio-tap";
\t\tmemory-region = <&npu_tap_buffer>;
\t\tstatus = "okay";
\t};
};
"""

# Append to file (after the last closing brace of the dts).
with open(dts_path, 'a') as f:
    f.write(npu_tap_overlay)

print("NPU tap DT changes applied successfully (V3.2.2)")
print("  - &dsp_reserved_heap size: 0xef0000 -> 0xeb0000 (-256 KB)")
print("  - npu_tap_buffer@942b0000: NEW 256 KB no-map carve")
print("  - imx_audio_tap node: NEW (compatible=electrosens,imx-audio-tap)")
