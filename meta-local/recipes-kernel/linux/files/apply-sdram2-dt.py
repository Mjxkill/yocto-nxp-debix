#!/usr/bin/env python3
"""Apply V5.4.1 SDRAM2 device tree modifications to imx8mp-evk.dts.

Run after apply-npu-tap-dt.py at do_patch:append.

Two modifications, all via DT overrides (no edit of imx8mp.dtsi SoC-level):
  1. Add sdram2_reserved@a0000000 in &{/reserved-memory} (8 MB no-map carve).
     Phase 1a.3 DSP-only DDR for matrix/effects buffers (mixer16, strips IN/OUT).
  2. Append &sdram2_reserved to the dsp@3b6e8000 node memory-region property
     so the rproc translation table allows the DSP to address 0xA0000000+.

V5.4.1 size : 8 MB (matches firmware SDRAM2_SIZE in memory.h). Future expansion
requires both DT carve update AND firmware rebuild (SDRAM2_SIZE).
"""
import sys
import re

dts_path = sys.argv[1]
with open(dts_path, 'r') as f:
    content = f.read()

if 'sdram2_reserved' in content:
    print("SDRAM2 DT changes already applied, skipping")
    sys.exit(0)

# (1) Add sdram2_reserved carve via &{/reserved-memory} override appended at file end.
sdram2_overlay = """
/* V5.4.1 SDRAM2 — DT overlay (added by apply-sdram2-dt.py) */

/* (1) DSP-only DDR carve — 8 MB no-map at 0xA0000000.
 *     Hors SOF MEMORY{} principal (au-delà de SDRAM1@0x92C00000+8M).
 *     Couvert par cacheattr region 5 (digit 5 du _memmap_cacheattr_imx8_wt_allvalid
 *     passé à 1 = WT cacheable côté HiFi4). Phase 1a.3 buffers matrix/effects.
 */
&{/reserved-memory} {
\tsdram2_reserved: sdram2_reserved@a0000000 {
\t\tcompatible = "shared-dma-pool";
\t\treg = <0 0xA0000000 0 0x800000>;
\t\tno-map;
\t};
};
"""

# Append SDRAM2 reserved-memory carve.
with open(dts_path, 'a') as f:
    f.write(sdram2_overlay)

# (2) Append &sdram2_reserved to dsp memory-region.
# The dsp node lives at &dsp (or dsp@3b6e8000 path) in imx8mp dtsi. The default
# memory-region in NXP BSP lists vdev0buffer + vdev0vring{0,1} + dsp_reserved.
# We need to extend it to include &sdram2_reserved without modifying the SoC dtsi.
#
# Pattern: search for an existing "&dsp { memory-region = ..." override, or add one.
with open(dts_path, 'r') as f:
    content = f.read()

# Match existing override block "&dsp { ... memory-region = < ... >; ... }"
# Looking for a single-line memory-region inside a &dsp{} block.
m = re.search(r'(&dsp\s*\{[^}]*memory-region\s*=\s*<)([^>]+)(>;)', content, re.DOTALL)
if m:
    existing_refs = m.group(2).strip()
    if '&sdram2_reserved' in existing_refs:
        print("SDRAM2 DT changes : &sdram2_reserved already in dsp memory-region")
    else:
        new_refs = existing_refs + ' &sdram2_reserved'
        new_content = content[:m.start(2)] + new_refs + content[m.end(2):]
        with open(dts_path, 'w') as f:
            f.write(new_content)
        print("  - dsp memory-region extended with &sdram2_reserved (in-place)")
else:
    # No existing &dsp {} override block — append one.
    dsp_override = """
/* (2) Extend dsp memory-region to authorize SDRAM2 access from the rproc. */
&dsp {
\tmemory-region = <&dsp_vdev0buffer>, <&dsp_vdev0vring0>,
\t                <&dsp_vdev0vring1>, <&dsp_reserved_heap>,
\t                <&sdram2_reserved>;
};
"""
    with open(dts_path, 'a') as f:
        f.write(dsp_override)
    print("  - dsp memory-region appended with &sdram2_reserved (new override)")

print("SDRAM2 DT changes applied successfully (V5.4.1)")
print("  - sdram2_reserved@a0000000: NEW 8 MB no-map carve")
