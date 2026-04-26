SUMMARY = "i.MX8MP NPU audio tap — kernel module exposing SOF DSP shared mem"
DESCRIPTION = "V3.2.2 NPU tap. Exposes /dev/imx-audio-tap (miscdevice mmap) for \
userspace to read the post-effects audio ring buffer written by the SOF DSP \
firmware at NPU_TAP_PHYS_ADDR (0x942B0000, 256 KB no-map carve in dsp_reserved_heap)."
HOMEPAGE = "https://github.com/Mjxkill/yocto-nxp-debix"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://imx-audio-tap.c;beginline=1;endline=1;md5=7226e442a172bcf25807246d7ef1eba1"

inherit module

SRC_URI = " \
    file://imx-audio-tap.c \
    file://imx-audio-tap-uapi.h \
    file://Makefile \
"

S = "${WORKDIR}/sources-unpack"

# Stage sources into a single dir so the Makefile can build them
do_unpack:append() {
    bb.build.exec_func('do_stage_sources', d)
}

python do_stage_sources() {
    import shutil, os
    work = d.getVar('WORKDIR')
    src = d.getVar('S')
    os.makedirs(src, exist_ok=True)
    for f in ('imx-audio-tap.c', 'imx-audio-tap-uapi.h', 'Makefile'):
        shutil.copy(os.path.join(work, f), src)
}

#
# R5 — Build-time sanity check : NPU_TAP_PHYS_ADDR must match the value
# defined in sof/src/include/sof/audio/npu_tap.h. Divergence between the
# kernel UAPI header and the firmware header would cause silent corruption
# (kernel and firmware writing/reading different physical addresses).
# A7 runtime DT check validates the DT vs UAPI ; this check validates UAPI vs SOF.
#
do_configure:prepend() {
    UAPI_HDR="${WORKDIR}/imx-audio-tap-uapi.h"
    SOF_HDR="${TOPDIR}/../sof/src/include/sof/audio/npu_tap.h"

    if [ ! -f "$SOF_HDR" ]; then
        bbwarn "SOF firmware header not found at $SOF_HDR — skipping cross-check (firmware not built locally?)"
        return 0
    fi

    UAPI_ADDR=$(grep -E '^[[:space:]]*#define[[:space:]]+NPU_TAP_PHYS_ADDR' "$UAPI_HDR" | awk '{print $3}' | tr -d 'Uu')
    SOF_ADDR=$(grep -E '^[[:space:]]*#define[[:space:]]+NPU_TAP_PHYS_ADDR' "$SOF_HDR" | awk '{print $3}' | tr -d 'Uu')

    if [ "$UAPI_ADDR" != "$SOF_ADDR" ]; then
        bbfatal "NPU_TAP_PHYS_ADDR mismatch between kernel UAPI ($UAPI_ADDR) and SOF firmware ($SOF_ADDR). \
Edit $UAPI_HDR or $SOF_HDR so both match. R2/R5 single-source-of-truth violation."
    fi

    bbnote "NPU_TAP_PHYS_ADDR cross-check OK : kernel UAPI = SOF firmware = $UAPI_ADDR"
}

# Auto-load at boot
KERNEL_MODULE_AUTOLOAD = "imx-audio-tap"

RPROVIDES:${PN} = "imx-audio-tap"
COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
