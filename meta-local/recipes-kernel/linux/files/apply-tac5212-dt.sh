#!/bin/sh
# Apply TAC5212 TDM device tree modifications to imx8mp-evk.dts
# Run after 0001-imx8mp-evk-audio-mipi.patch has been applied

DTS="$1"

if [ ! -f "$DTS" ]; then
    echo "ERROR: DTS file not found: $DTS"
    exit 1
fi

# Skip if already applied
if grep -q "tac5212" "$DTS"; then
    echo "TAC5212 DT changes already applied, skipping"
    exit 0
fi

# 1. Replace sound-dac-out block with sound-tac5212
sed -i '/spdif_dit: spdif-dit-0 {/,/^[[:space:]]*};$/d' "$DTS"

sed -i '/sound-dac-out {/,/^[[:space:]]*};/{
    /sound-dac-out {/,/^[[:space:]]*};/c\
\tsound-tac5212 {\
\t\tcompatible = "fsl,imx-audio-card";\
\t\tmodel = "tac5212-tdm";\
\t\tstatus = "okay";\
\t\tpri-dai-link {\
\t\t\tlink-name = "tac5212 tdm";\
\t\t\tformat = "dsp_b";\
\t\t\tdai-tdm-slot-num = <8>;\
\t\t\tdai-tdm-slot-width = <32>;\
\t\t\tbitclock-master = <\&tac5212_codec>;\
\t\t\tframe-master = <\&tac5212_codec>;\
\t\t\tcpu {\
\t\t\t\tsound-dai = <\&sai7>;\
\t\t\t};\
\t\t\ttac5212_codec: codec {\
\t\t\t\tsound-dai = <\&tac0>, <\&tac1>, <\&tac2>, <\&tac3>;\
\t\t\t};\
\t\t};\
\t};
}' "$DTS"

# 2. Modify &sai7 node: remove mclk-direction-output, change dataline
sed -i '/&sai7 {/,/^};/{
    /fsl,sai-mclk-direction-output/d
    s/fsl,dataline = <1 0 1>;/fsl,dataline = <1 1 1>;/
}' "$DTS"

# 3. Add RX_DATA00 pin to pinctrl_sai7
sed -i '/pinctrl_sai7: sai7grp {/,/>;/{
    /MX8MP_IOMUXC_ECSPI2_MOSI__AUDIOMIX_SAI7_TX_DATA00/a\
\t\t\tMX8MP_IOMUXC_ECSPI1_MISO__AUDIOMIX_SAI7_RX_DATA00\t0x1c4
}' "$DTS"

# 4. Add TAC5212 codec nodes and disable conflicts on &i2c4 (i2c@30a50000)
# Find the i2c4 node (30a50000) and add TAC nodes + disable eeprom/rtc
sed -i '/i2c@30a50000 {/,/^[[:space:]]*};/{
    /eeprom@50 {/,/};/{
        s/compatible = "atmel,24c02";/compatible = "atmel,24c02";\n\t\t\t\tstatus = "disabled";/
    }
    /hym8563@51 {/,/};/{
        /compatible = "haoyu,hym8563";/a\
\t\t\t\tstatus = "disabled";
    }
}' "$DTS"

# Add TAC5212 nodes before the closing of i2c@30a50000
# Find the last }; of i2c@30a50000 block and insert before it
sed -i '/i2c@30a50000 {/{
    :loop
    N
    /\n[[:space:]]*};$/!b loop
    s/\n[[:space:]]*};$/\
\n\t\t\ttac0: audio-codec@50 {\
\t\t\t\tcompatible = "ti,tac5212";\
\t\t\t\treg = <0x50>;\
\t\t\t\t#sound-dai-cells = <0>;\
\t\t\t\tti,bus-controller;\
\t\t\t};\
\n\t\t\ttac1: audio-codec@51 {\
\t\t\t\tcompatible = "ti,tac5212";\
\t\t\t\treg = <0x51>;\
\t\t\t\t#sound-dai-cells = <0>;\
\t\t\t};\
\n\t\t\ttac2: audio-codec@52 {\
\t\t\t\tcompatible = "ti,tac5212";\
\t\t\t\treg = <0x52>;\
\t\t\t\t#sound-dai-cells = <0>;\
\t\t\t};\
\n\t\t\ttac3: audio-codec@53 {\
\t\t\t\tcompatible = "ti,tac5212";\
\t\t\t\treg = <0x53>;\
\t\t\t\t#sound-dai-cells = <0>;\
\t\t\t};\
\n\t\t};/
}' "$DTS"

echo "TAC5212 DT changes applied successfully"
