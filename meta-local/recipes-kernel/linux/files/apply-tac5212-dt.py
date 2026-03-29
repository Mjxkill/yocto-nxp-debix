#!/usr/bin/env python3
"""Apply TAC5212 TDM device tree modifications to imx8mp-evk.dts.

Run after 0001-imx8mp-evk-audio-mipi.patch has been applied.
"""
import sys, re

dts_path = sys.argv[1]
with open(dts_path, 'r') as f:
    content = f.read()

if 'tac5212' in content:
    print("TAC5212 DT changes already applied, skipping")
    sys.exit(0)

# 1. Remove spdif_dit node
content = re.sub(
    r'\n\tspdif_dit: spdif-dit-0 \{[^}]*\};\n',
    '\n', content)

# 2. Replace sound-dac-out block with sound-tac5212
content = re.sub(
    r'\n\tsound-dac-out \{.*?\n\t\};',
    """
\tsound-tac5212 {
\t\tcompatible = "fsl,imx-audio-card";
\t\tmodel = "tac5212-tdm";
\t\tstatus = "okay";
\t\tpri-dai-link {
\t\t\tlink-name = "tac5212 tdm";
\t\t\tformat = "dsp_b";
\t\t\tdai-tdm-slot-num = <8>;
\t\t\tdai-tdm-slot-width = <32>;
\t\t\tbitclock-master = <&tac5212_cpu>;
\t\t\tframe-master = <&tac5212_cpu>;
\t\t\ttac5212_cpu: cpu {
\t\t\t\tsound-dai = <&sai7>;
\t\t\t};
\t\t\tcodec {
\t\t\t\tsound-dai = <&tac0>, <&tac1>, <&tac2>, <&tac3>;
\t\t\t};
\t\t};
\t};""", content, flags=re.DOTALL)

# 3. SAI7: keep mclk-direction-output (SAI is bus master),
#    asynchronous mode (TX=provider, RX=consumer on same physical wires),
#    enable both RX and TX datalines,
#    remove fixed clock rate so driver can switch PLL for 44.1k/48k families
def fix_sai7(m):
    block = m.group(0)
    block = block.replace('fsl,dataline = <1 0 1>;', 'fsl,dataline = <0 1 1>;')
    # Remove fixed clock rate so SAI driver can reparent PLL dynamically
    block = re.sub(r'\n\tassigned-clock-rates = <12288000>;', '', block)
    # Override clocks and clock-names to add pll8k/pll11k for 44.1k support
    # Insert before status = "okay"
    block = block.replace(
        '\tstatus = "okay";',
        '\tclocks = <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_SAI7_IPG>, <&clk IMX8MP_CLK_DUMMY>,\n'
        '\t\t <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_SAI7_MCLK1>, <&clk IMX8MP_CLK_DUMMY>,\n'
        '\t\t <&clk IMX8MP_CLK_DUMMY>,\n'
        '\t\t <&clk IMX8MP_AUDIO_PLL1_OUT>, <&clk IMX8MP_AUDIO_PLL2_OUT>;\n'
        '\tclock-names = "bus", "mclk0", "mclk1", "mclk2", "mclk3", "pll8k", "pll11k";\n'
        '\tstatus = "okay";')
    return block
content = re.sub(r'&sai7 \{.*?\n\};', fix_sai7, content, flags=re.DOTALL)

# 4. Add RX_DATA00 pin to pinctrl_sai7
content = content.replace(
    'MX8MP_IOMUXC_ECSPI2_MOSI__AUDIOMIX_SAI7_TX_DATA00\t0x1c4\n\t\t>;',
    'MX8MP_IOMUXC_ECSPI2_MOSI__AUDIOMIX_SAI7_TX_DATA00\t0x1c4\n'
    '\t\t\t/* RX pins with SION enabled (0x13 = mux mode 3 + SION bit) */\n'
    '\t\t\t0x1E0 0x440 0x538 0x13 0x1\t\t\t\t0x1c4\n'
    '\t\t\t0x1E4 0x444 0x530 0x13 0x1\t\t\t\t0x1c4\n'
    '\t\t\t0x1E8 0x448 0x534 0x13 0x1\t\t\t\t0x1c4\n\t\t>;')

# 5. Disable eeprom@50 and hym8563@51
content = re.sub(
    r'(eeprom\S* eeprom@50 \{)\n(\s*compatible = "atmel,24c02";)',
    r'\1\n\2\n\t\tstatus = "disabled";',
    content)
content = re.sub(
    r'(hym8563\S* hym8563@51 \{)\n(\s*compatible = "haoyu,hym8563";)',
    r'\1\n\2\n\t\tstatus = "disabled";',
    content)

# 6. Add TAC5212 I2C nodes inside &i2c4
# Find the block containing pinctrl_i2c4, track braces to find its closing };
lines = content.split('\n')
i2c4_start = None
for i, line in enumerate(lines):
    if 'pinctrl_i2c4' in line:
        # Walk back to find the opening { of this block
        for j in range(i, -1, -1):
            if '{' in lines[j]:
                i2c4_start = j
                break
        break

if i2c4_start is not None:
    depth = 0
    i2c4_end = None
    for j in range(i2c4_start, len(lines)):
        depth += lines[j].count('{') - lines[j].count('}')
        if depth == 0:
            i2c4_end = j
            break

    if i2c4_end is not None:
        tac_block = [
            '',
            '\ttac0: audio-codec@50 {',
            '\t\tcompatible = "ti,tac5212";',
            '\t\treg = <0x50>;',
            '\t\t#sound-dai-cells = <0>;',
            '\t\tsound-name-prefix = "TAC0";',
            '\t};',
            '',
            '\ttac1: audio-codec@51 {',
            '\t\tcompatible = "ti,tac5212";',
            '\t\treg = <0x51>;',
            '\t\t#sound-dai-cells = <0>;',
            '\t\tsound-name-prefix = "TAC1";',
            '\t};',
            '',
            '\ttac2: audio-codec@52 {',
            '\t\tcompatible = "ti,tac5212";',
            '\t\treg = <0x52>;',
            '\t\t#sound-dai-cells = <0>;',
            '\t\tsound-name-prefix = "TAC2";',
            '\t};',
            '',
            '\ttac3: audio-codec@53 {',
            '\t\tcompatible = "ti,tac5212";',
            '\t\treg = <0x53>;',
            '\t\t#sound-dai-cells = <0>;',
            '\t\tsound-name-prefix = "TAC3";',
            '\t};',
        ]
        # Insert before the closing }; of &i2c4
        for line in reversed(tac_block):
            lines.insert(i2c4_end, line)
        content = '\n'.join(lines)

with open(dts_path, 'w') as f:
    f.write(content)

print("TAC5212 DT changes applied successfully")
