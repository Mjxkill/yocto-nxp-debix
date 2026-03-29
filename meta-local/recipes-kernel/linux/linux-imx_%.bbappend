FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Apply board audio/MIPI updates and enable SPDIF DIR/DIT support
SRC_URI += "file://0001-imx8mp-evk-audio-mipi.patch"
SRC_URI += "file://spdif.cfg"
SRC_URI += "file://disable-at24.cfg"

# TAC5212 codec driver and device tree configuration
SRC_URI += "file://tac5212.c"
SRC_URI += "file://tac5212.h"
SRC_URI += "file://apply-tac5212-dt.py"
SRC_URI += "file://tac5212.cfg"

# Install TAC5212 driver into kernel tree, patch Kconfig/Makefile and DTS
do_patch:prepend() {
    # Copy driver source files
    cp ${WORKDIR}/tac5212.c ${S}/sound/soc/codecs/tac5212.c
    cp ${WORKDIR}/tac5212.h ${S}/sound/soc/codecs/tac5212.h

    # Patch fsl_sai: async mode - force RX as consumer (BCD=0, FSD=0)
    # TX stays provider, RX receives clocks from shared wire via its own pins
    if ! grep -q "async RX consumer fix" ${S}/sound/soc/fsl/fsl_sai.c; then
        sed -i '/FSL_SAI_CR4_FSP | FSL_SAI_CR4_FSD_MSTR, val_cr4);/{
            a\
\n\t/* async RX consumer fix: RX must be consumer (clocks from TX via shared wire) */\
\tif (!sai->synchronous[RX] && !sai->synchronous[TX] && !tx) {\
\t\tregmap_update_bits(sai->regmap, FSL_SAI_xCR2(false, ofs),\
\t\t\t\t   FSL_SAI_CR2_BCD_MSTR, 0);\
\t\tregmap_update_bits(sai->regmap, FSL_SAI_xCR4(false, ofs),\
\t\t\t\t   FSL_SAI_CR4_FSD_MSTR, 0);\
\t\tsai->is_consumer_mode[false] = true;\
\t}
        }' ${S}/sound/soc/fsl/fsl_sai.c
    fi

    # Patch fsl_sai_trigger: when capture starts, also start TX for clock gen
    if ! grep -q "async capture TX start" ${S}/sound/soc/fsl/fsl_sai.c; then
        sed -i '/FSL_SAI_CSR_xIE_MASK, FSL_SAI_FLAGS);/a\
\n\t\t/* async capture TX start: enable TX clocks when RX starts */\
\t\tif (!tx && !sai->synchronous[RX]) {\
\t\t\tregmap_update_bits(sai->regmap, FSL_SAI_xCR3(true, ofs),\
\t\t\t\t\t   FSL_SAI_CR3_TRCE_MASK, FSL_SAI_CR3_TRCE(1));\
\t\t\tregmap_update_bits(sai->regmap, FSL_SAI_xCSR(true, ofs),\
\t\t\t\t\t   FSL_SAI_CSR_TERE, FSL_SAI_CSR_TERE);\
\t\t}' ${S}/sound/soc/fsl/fsl_sai.c
    fi

    # Add Kconfig entry before SND_SOC_TAS2552
    if ! grep -q SND_SOC_TAC5212 ${S}/sound/soc/codecs/Kconfig; then
        sed -i '/config SND_SOC_TAS2552/i\
config SND_SOC_TAC5212\
\ttristate "Texas Instruments TAC5212 CODEC"\
\tdepends on I2C\
\tselect REGMAP_I2C\
\thelp\
\t  Enable support for TI TAC5212 stereo audio ADC/DAC codec.\
' ${S}/sound/soc/codecs/Kconfig
    fi

    # Add Makefile obj and build entries
    if ! grep -q tac5212 ${S}/sound/soc/codecs/Makefile; then
        sed -i '/snd-soc-tas5720-objs/a\snd-soc-tac5212-objs := tac5212.o' ${S}/sound/soc/codecs/Makefile
        sed -i '/obj-\$(CONFIG_SND_SOC_TAS5720)/a\obj-$(CONFIG_SND_SOC_TAC5212)\t+= snd-soc-tac5212.o' ${S}/sound/soc/codecs/Makefile
    fi
}

# Apply TAC5212 DT changes after all patches are applied
do_patch:append() {
    python3 ${WORKDIR}/apply-tac5212-dt.py ${S}/arch/arm64/boot/dts/freescale/imx8mp-evk.dts
}

# Force TAC5212 config into .config after kernel configure
do_configure:append() {
    cfg="${B}/.config"
    if [ -f "$cfg" ]; then
        sed -i 's/# CONFIG_SND_SOC_TAC5212 is not set/CONFIG_SND_SOC_TAC5212=m/' "$cfg"
        if ! grep -q "CONFIG_SND_SOC_TAC5212" "$cfg"; then
            echo "CONFIG_SND_SOC_TAC5212=m" >> "$cfg"
        fi
        oe_runmake -C ${S} O=${B} olddefconfig
    fi
}

# Allow linux-imx-src debug package to pass QA despite build path references
INSANE_SKIP:linux-imx-src += "buildpaths"
