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

    # Patch fsl_sai: in sync mode, ensure BCD/FSD set on BOTH directions
    # Both TX and RX pins drive the same BCLK/FSYNC (same SAI, shared wire)
    if ! grep -q "sync mode BCD/FSD fix" ${S}/sound/soc/fsl/fsl_sai.c; then
        sed -i '/FSL_SAI_CR4_FSP | FSL_SAI_CR4_FSD_MSTR, val_cr4);/{
            a\
\n\t/* sync mode BCD/FSD fix: set BCD/FSD on both directions */\
\tif (sai->synchronous[RX] && !sai->synchronous[TX]) {\
\t\tregmap_update_bits(sai->regmap, FSL_SAI_xCR2(true, ofs),\
\t\t\t\t   FSL_SAI_CR2_BCD_MSTR, val_cr2 & FSL_SAI_CR2_BCD_MSTR);\
\t\tregmap_update_bits(sai->regmap, FSL_SAI_xCR4(true, ofs),\
\t\t\t\t   FSL_SAI_CR4_FSD_MSTR, val_cr4 & FSL_SAI_CR4_FSD_MSTR);\
\t}
        }' ${S}/sound/soc/fsl/fsl_sai.c
    fi

    # Patch fsl_sai_trigger: in sync mode capture, enable TX TRCE for clock gen
    if ! grep -q "sync mode TRCE fix" ${S}/sound/soc/fsl/fsl_sai.c; then
        sed -i '/FSL_SAI_CSR_xIE_MASK, FSL_SAI_FLAGS);/a\
\n\t\t/* sync mode TRCE fix: enable opposite TRCE for clock generation */\
\t\tif (fsl_sai_dir_is_synced(sai, adir) && !tx)\
\t\t\tregmap_update_bits(sai->regmap, FSL_SAI_xCR3(!tx, ofs),\
\t\t\t\t\t   FSL_SAI_CR3_TRCE_MASK, FSL_SAI_CR3_TRCE(1));' ${S}/sound/soc/fsl/fsl_sai.c
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
