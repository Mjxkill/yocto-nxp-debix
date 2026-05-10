SUMMARY = "i.MX8MP NPU audio tap — kernel module exposing SOF DSP shared mem"
DESCRIPTION = "V7.0-E4 NPU dual-tap. Exposes 2 miscdevices : \
/dev/imx-audio-tap-in (capture brut, 0x94270000 256 KB) et \
/dev/imx-audio-tap-out (playback post-effets, 0x942B0000 256 KB, alias V3.2.2). \
Le SOF DSP firmware écrit dans ces 2 zones no-map du dsp_reserved_heap."
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
# R5 — Build-time sanity check (V7.0-E4 dual-tap) : NPU_TAP_IN_PHYS_ADDR et
# NPU_TAP_OUT_PHYS_ADDR doivent matcher entre l'UAPI kernel et SOF firmware.
# Divergence = corruption silencieuse (kernel/firmware accédant des adresses
# différentes). DT runtime check (A7) valide DT vs UAPI ; ce check valide
# UAPI vs SOF.
#
do_configure:prepend() {
    UAPI_HDR="${WORKDIR}/imx-audio-tap-uapi.h"
    SOF_HDR="${TOPDIR}/../sof/src/include/sof/audio/npu_tap.h"

    if [ ! -f "$SOF_HDR" ]; then
        bbwarn "SOF firmware header not found at $SOF_HDR — skipping cross-check (firmware not built locally?)"
        return 0
    fi

    for SYM in NPU_TAP_IN_PHYS_ADDR NPU_TAP_OUT_PHYS_ADDR; do
        UAPI_VAL=$(grep -E "^[[:space:]]*#define[[:space:]]+$SYM[[:space:]]" "$UAPI_HDR" | awk '{print $3}' | tr -d 'Uu')
        SOF_VAL=$(grep -E "^[[:space:]]*#define[[:space:]]+$SYM[[:space:]]" "$SOF_HDR" | awk '{print $3}' | tr -d 'Uu')

        if [ -z "$UAPI_VAL" ] || [ -z "$SOF_VAL" ]; then
            bbfatal "$SYM missing in UAPI ($UAPI_VAL) or SOF ($SOF_VAL) — R2/R5 single source of truth violation."
        fi

        if [ "$UAPI_VAL" != "$SOF_VAL" ]; then
            bbfatal "$SYM mismatch : kernel UAPI ($UAPI_VAL) vs SOF firmware ($SOF_VAL). \
Edit $UAPI_HDR or $SOF_HDR so both match."
        fi

        bbnote "$SYM cross-check OK : kernel UAPI = SOF firmware = $UAPI_VAL"
    done
}

# Auto-load at boot (un seul module, 2 instances DT bindées)
KERNEL_MODULE_AUTOLOAD = "imx-audio-tap"

RPROVIDES:${PN} = "imx-audio-tap"
COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
