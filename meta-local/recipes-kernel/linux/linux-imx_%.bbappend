FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# V9.0 — PREEMPT_RT patch series (kernel.org)
# Applied first (before our other custom patches) since RT touches core kernel
# infrastructure (locking, scheduling, IRQ). Our custom patches (TAC5212,
# NPU tap, multicodec) are user-space-adjacent and don't conflict with RT.
SRC_URI += "file://patch-6.6.36-rt35.patch.gz;apply=yes;striplevel=1"
SRC_URI += "file://rt.cfg"

# Apply board audio/MIPI updates and enable SPDIF DIR/DIT support
SRC_URI += "file://0001-imx8mp-evk-audio-mipi.patch"
SRC_URI += "file://spdif.cfg"
SRC_URI += "file://disable-at24.cfg"

# SOF probes support for i.MX (additive: new file imx-probes.c, new Kconfig
# entry, no behavior change to existing code)
SRC_URI += "file://imx-probes.c"
SRC_URI += "file://apply-imx-probes.py"
SRC_URI += "file://sof-imx-probes.cfg"

# TAC5212 codec driver and device tree configuration
SRC_URI += "file://tac5212.c"
SRC_URI += "file://tac5212.h"
SRC_URI += "file://apply-tac5212-dt.py"
SRC_URI += "file://tac5212.cfg"

# V3.2.2 NPU tap — DT carve for shared mem at 0x942B0000 (256 KB no-map)
SRC_URI += "file://apply-npu-tap-dt.py"

# V5.4.1 SDRAM2 — DT carve for DSP-only matrix/effects (8 MB no-map @0xA0000000)
SRC_URI += "file://apply-sdram2-dt.py"

# V7.0-E7.4.b — patch simple-card.c to support N codec phandles per DAI link
# (upstream hardcodes num_codecs=1, blocking multi-TAC TDM binding)
SRC_URI += "file://apply-simple-card-multicodec.py"
# V7.0-E7.4.b — patch imx-card.c to leave link->id at sequential default
# (upstream forces link->id from cpu DT args, breaks SOF topology matching)
SRC_URI += "file://apply-imx-card-linkid.py"

# Install TAC5212 driver into kernel tree, patch Kconfig/Makefile and DTS
do_patch:prepend() {
    # Copy SOF imx-probes source file then patch Kconfig/Makefile/imx8m.c
    cp ${WORKDIR}/imx-probes.c ${S}/sound/soc/sof/imx/imx-probes.c
    python3 ${WORKDIR}/apply-imx-probes.py ${S}

    # Copy driver source files
    cp ${WORKDIR}/tac5212.c ${S}/sound/soc/codecs/tac5212.c
    cp ${WORKDIR}/tac5212.h ${S}/sound/soc/codecs/tac5212.h

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
    # V3.2.2 NPU tap DT carve (npu_tap_buffer@942b0000 + imx_audio_tap node)
    python3 ${WORKDIR}/apply-npu-tap-dt.py ${S}/arch/arm64/boot/dts/freescale/imx8mp-evk.dts
    # V5.4.1 SDRAM2 DT carve (sdram2_reserved@a0000000 + dsp memory-region append)
    python3 ${WORKDIR}/apply-sdram2-dt.py ${S}/arch/arm64/boot/dts/freescale/imx8mp-evk.dts
    # V7.0-E7.4.b simple-card multi-codec support (idempotent, backward-compat)
    python3 ${WORKDIR}/apply-simple-card-multicodec.py ${S}
    # V7.0-E7.4.b imx-card link_id : keep sequential default for SOF tplg match
    python3 ${WORKDIR}/apply-imx-card-linkid.py ${S}
}

# Force TAC5212 + SOF imx-probes + PREEMPT_RT config into .config after kernel configure
do_configure:append() {
    cfg="${B}/.config"
    if [ -f "$cfg" ]; then
        sed -i 's/# CONFIG_SND_SOC_TAC5212 is not set/CONFIG_SND_SOC_TAC5212=m/' "$cfg"
        if ! grep -q "CONFIG_SND_SOC_TAC5212" "$cfg"; then
            echo "CONFIG_SND_SOC_TAC5212=m" >> "$cfg"
        fi
        sed -i 's/# CONFIG_SND_SOC_SOF_IMX_PROBES is not set/CONFIG_SND_SOC_SOF_IMX_PROBES=m/' "$cfg"
        if ! grep -q "CONFIG_SND_SOC_SOF_IMX_PROBES" "$cfg"; then
            echo "CONFIG_SND_SOC_SOF_IMX_PROBES=m" >> "$cfg"
        fi

        # V9.0 — Switch preempt model from CONFIG_PREEMPT to CONFIG_PREEMPT_RT.
        # The 4 preempt models are mutually exclusive Kconfig choices, so
        # merge_config of rt.cfg alone is silently rejected if CONFIG_PREEMPT=y
        # is already set (case here from defconfig). Forced switch via sed
        # before olddefconfig resolves the choice deterministically.
        sed -i 's/^CONFIG_PREEMPT=y/# CONFIG_PREEMPT is not set/' "$cfg"
        sed -i 's/^CONFIG_PREEMPT_DYNAMIC=y/# CONFIG_PREEMPT_DYNAMIC is not set/' "$cfg"
        sed -i 's/^# CONFIG_PREEMPT_RT is not set/CONFIG_PREEMPT_RT=y/' "$cfg"
        if ! grep -q "^CONFIG_PREEMPT_RT=y" "$cfg"; then
            echo "CONFIG_PREEMPT_RT=y" >> "$cfg"
        fi
        # Threaded IRQs default (also need `threadirqs` in bootargs)
        sed -i 's/^# CONFIG_IRQ_FORCED_THREADING_DEFAULT is not set/CONFIG_IRQ_FORCED_THREADING_DEFAULT=y/' "$cfg"
        if ! grep -q "^CONFIG_IRQ_FORCED_THREADING_DEFAULT=y" "$cfg"; then
            echo "CONFIG_IRQ_FORCED_THREADING_DEFAULT=y" >> "$cfg"
        fi

        # V10-P4a - le defconfig NXP laisse CONFIG_USB_GADGET_DEBUG=y, qui
        # compile les pr_debug du gadget : le path UAC2 imprime 2 printk PAR
        # PAQUET USB (~4000 lignes/s pendant toute lecture) -> journald+syslogd
        # ~45 % du core 0 + contention printk dans la completion USB.
        # Mesure 2026-07-04 : 13 xruns/min (lecture+kiosk), 0 sans kiosk.
        sed -i 's/^CONFIG_USB_GADGET_DEBUG=y/# CONFIG_USB_GADGET_DEBUG is not set/' "$cfg" 

        oe_runmake -C ${S} O=${B} olddefconfig
    fi
}

# Allow linux-imx-src debug package to pass QA despite build path references
INSANE_SKIP:linux-imx-src += "buildpaths"
