#!/usr/bin/env python3
"""Apply NPU tap V3.2.2 device tree modifications to imx8mp-evk.dts.

Run after apply-tac5212-dt.py at do_configure:append.

Three modifications:
  1. Reduce dsp_reserved_heap@93400000 size : 0xef0000 → 0xeb0000 (-256 KB)
     to make room for npu_tap_buffer@942b0000.
  2. Add npu_tap_buffer@942b0000 reserved-memory node (256 KB, no-map).
  3. Add imx_audio_tap node with compatible="electrosens,imx-audio-tap"
     and memory-region=<&npu_tap_buffer> for the kernel module to bind.

R7 sentinelle mono-DAI : V3.2.2 firmware uses ONE shared 256 KB tap buffer.
M6 limitation Phase 2 : if multi-DAI playback simultaneous needed, this DT
will need to be refactored with per-DAI buffer carve (4 zones).
"""
import sys, re

dts_path = sys.argv[1]
with open(dts_path, 'r') as f:
    content = f.read()

if 'npu_tap_buffer' in content:
    print("NPU tap DT changes already applied, skipping")
    sys.exit(0)

# 1. Reduce dsp_reserved_heap size from 0xef0000 to 0xeb0000 (-256 KB = -0x40000)
# Original: dsp_reserved_heap: dsp_reserved_heap@93400000 {
#               reg = <0 0x93400000 0 0xef0000>;
new_content, n_subs = re.subn(
    r'(dsp_reserved_heap@93400000 \{[^}]*reg = <0 0x93400000 0 )0xef0000(>;)',
    r'\g<1>0xeb0000\g<2>',
    content,
    flags=re.DOTALL,
)
if n_subs != 1:
    print(f"ERROR: expected 1 dsp_reserved_heap match, got {n_subs}", file=sys.stderr)
    sys.exit(1)
content = new_content

# 2. Add npu_tap_buffer node after dsp_reserved_heap node, inside reserved-memory {}
# Insert just before the closing of reserved-memory block — find a stable anchor.
# We insert it right after dsp_reserved_heap declaration.
def insert_npu_tap(m):
    return m.group(0) + """

\t\tnpu_tap_buffer: npu_tap_buffer@942b0000 {
\t\t\tcompatible = "shared-dma-pool";
\t\t\treg = <0 0x942b0000 0 0x40000>;   /* 256 KB, V3.2.2 NPU tap */
\t\t\tno-map;
\t\t};"""

new_content, n_subs = re.subn(
    r'dsp_reserved_heap@93400000 \{[^}]*\};',
    insert_npu_tap,
    content,
    flags=re.DOTALL,
)
if n_subs != 1:
    print(f"ERROR: expected 1 dsp_reserved_heap insertion match, got {n_subs}",
          file=sys.stderr)
    sys.exit(1)
content = new_content

# 3. Add imx_audio_tap node at root level — placed after the reserved-memory
# block closes. Insert it just before the next top-level entry. We use the
# closing of reserved-memory followed by an empty-line as the anchor.
imx_audio_tap_node = """
\timx_audio_tap: imx_audio_tap {
\t\tcompatible = "electrosens,imx-audio-tap";
\t\tmemory-region = <&npu_tap_buffer>;
\t\tstatus = "okay";
\t};
"""

# Find the closing brace of the reserved-memory { ... } block at root level.
# Pattern: the reserved-memory block has "ranges;" then nodes inside, then "};\n"
# at single-tab indentation. Anchor on a stable nearby element : the soc@0
# node which usually follows.
def find_reserved_memory_end(text):
    """Return index just after the 'reserved-memory { ... };' closing brace."""
    # Find 'reserved-memory {' at root level
    m = re.search(r'\n\treserved-memory \{', text)
    if not m:
        return -1
    start = m.start()
    # Walk forward, counting braces
    depth = 0
    i = m.end() - 1   # position of the '{'
    while i < len(text):
        c = text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                # Find the trailing semicolon
                j = text.find(';', i)
                if j == -1:
                    return -1
                return j + 1
        i += 1
    return -1

end = find_reserved_memory_end(content)
if end == -1:
    print("ERROR: could not locate end of reserved-memory block", file=sys.stderr)
    sys.exit(1)

content = content[:end] + imx_audio_tap_node + content[end:]

with open(dts_path, 'w') as f:
    f.write(content)

print("NPU tap DT changes applied successfully (V3.2.2)")
print("  - dsp_reserved_heap@93400000: 0xef0000 -> 0xeb0000 (-256 KB)")
print("  - npu_tap_buffer@942b0000: NEW 256 KB no-map carve")
print("  - imx_audio_tap node: NEW (compatible=electrosens,imx-audio-tap)")
