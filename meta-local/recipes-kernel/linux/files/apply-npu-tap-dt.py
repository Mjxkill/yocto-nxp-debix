#!/usr/bin/env python3
"""Apply NPU dual-tap device tree modifications to imx8mp-evk.dts.

V7.0-E4 — passe de 1 carve (V3.2.2 npu_tap_buffer @0x942B0000) à 2 carves :
  - tap_in_buffer  @0x94270000 (256 KB) : capture brut post-DAI RX
  - tap_out_buffer @0x942B0000 (256 KB) : playback post-effets (alias V3.2.2)

Run after apply-tac5212-dt.py at do_patch:append.

Modifications appliquées via DT overrides (pas d'édition de imx8mp.dtsi) :
  1. Override &dsp_reserved_heap pour reclaim 512 KB : 0xef0000 → 0xe70000
     (au lieu de -256 KB en V3.2.2) afin de carver 2 zones de 256 KB.
  2. Add tap_in_buffer @0x94270000 (256 KB no-map) — NEW V7.0-E4.
  3. Add tap_out_buffer @0x942B0000 (256 KB no-map) — alias V3.2.2.
  4. Add imx_audio_tap_in node (compatible electrosens,imx-audio-tap +
     memory-region=tap_in_buffer + device-name="imx-audio-tap-in").
  5. Add imx_audio_tap_out node (idem mais memory-region=tap_out_buffer +
     device-name="imx-audio-tap-out").

Disposition mémoire :
  0x93400000 dsp_reserved_heap (taille 0xe70000 = 14.687 MB)
  0x94270000 tap_in_buffer  (256 KB) ← début dsp_heap + 0xe70000
  0x942B0000 tap_out_buffer (256 KB)
  0x942F0000 vdev0vring0    (préservé)
"""
import sys

dts_path = sys.argv[1]
with open(dts_path, 'r') as f:
    content = f.read()

if 'tap_in_buffer' in content or 'tap_out_buffer' in content:
    print("NPU dual-tap DT changes already applied, skipping")
    sys.exit(0)

# Si le DT contient déjà l'ancien npu_tap_buffer (V3.2.2), on refuse pour ne
# pas créer une double-carve incohérente. L'utilisateur doit nettoyer.
if 'npu_tap_buffer' in content:
    print("ERROR: V3.2.2 npu_tap_buffer present in DT — must be removed first")
    print("       (the V7.0-E4 overlay supersedes it with tap_out_buffer @ same addr)")
    sys.exit(1)

npu_tap_overlay = """
/* V7.0-E4 NPU dual-tap — DT overlay (added by apply-npu-tap-dt.py) */

/* (1) Reduce dsp_reserved_heap by 512 KB to make room for 2 tap buffers.
 *     Original: 0x93400000, size 0xef0000  (15.875 MB)
 *     Modified: 0x93400000, size 0xe70000  (14.687 MB, -512 KB)
 *     Freed range: 0x94270000-0x942EFFFF = 512 KB pour 2 carves.
 */
&dsp_reserved_heap {
\treg = <0 0x93400000 0 0xe70000>;
};

/* (2) tap_in_buffer carve — 256 KB no-map @ 0x94270000 (capture brut, E4). */
&{/reserved-memory} {
\ttap_in_buffer: tap_in_buffer@94270000 {
\t\tcompatible = "shared-dma-pool";
\t\treg = <0 0x94270000 0 0x40000>;
\t\tno-map;
\t};
};

/* (3) tap_out_buffer carve — 256 KB no-map @ 0x942B0000 (play post-FX, E5).
 *     Alias adresse V3.2.2 — préserve compat firmware hook playback existant.
 */
&{/reserved-memory} {
\ttap_out_buffer: tap_out_buffer@942b0000 {
\t\tcompatible = "shared-dma-pool";
\t\treg = <0 0x942b0000 0 0x40000>;
\t\tno-map;
\t};
};

/* (4) imx_audio_tap_in platform node — module imx-audio-tap bind, expose
 *     /dev/imx-audio-tap-in (prop device-name).
 */
/ {
\timx_audio_tap_in: imx_audio_tap_in {
\t\tcompatible = "electrosens,imx-audio-tap";
\t\tmemory-region = <&tap_in_buffer>;
\t\tdevice-name = "imx-audio-tap-in";
\t\tstatus = "okay";
\t};
};

/* (5) imx_audio_tap_out platform node — expose /dev/imx-audio-tap-out (E5). */
/ {
\timx_audio_tap_out: imx_audio_tap_out {
\t\tcompatible = "electrosens,imx-audio-tap";
\t\tmemory-region = <&tap_out_buffer>;
\t\tdevice-name = "imx-audio-tap-out";
\t\tstatus = "okay";
\t};
};
"""

with open(dts_path, 'a') as f:
    f.write(npu_tap_overlay)

print("NPU dual-tap DT changes applied successfully (V7.0-E4)")
print("  - &dsp_reserved_heap size: 0xef0000 -> 0xe70000 (-512 KB)")
print("  - tap_in_buffer  @0x94270000: NEW 256 KB no-map carve")
print("  - tap_out_buffer @0x942B0000: NEW 256 KB no-map carve (V3.2.2 alias)")
print("  - imx_audio_tap_in  : device-name=imx-audio-tap-in")
print("  - imx_audio_tap_out : device-name=imx-audio-tap-out")
