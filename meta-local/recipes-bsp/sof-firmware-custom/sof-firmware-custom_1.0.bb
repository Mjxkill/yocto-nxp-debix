SUMMARY = "V7.0 custom SOF Zephyr firmware + tac5212 topology"
DESCRIPTION = "Pre-built SOF firmware image (sof-imx8m.ri) and ALSA topology \
blob (sof-imx8mp-tac5212.tplg) used by V7.0 audio platform. These artifacts \
are built from the project's local SOF source tree (see ARCHI §15 build \
procedure) and vendored here to guarantee reproducibility from a fresh \
git clone : the upstream meta-imx 'sof-zephyr' recipe ships a vendor \
binary that does NOT include the TAC5212 codec support nor the project \
patches (multiband_drc multi-config, NPU dual-tap, DRC D3 per-channel, \
SAI TX FIFO alignment). Without this recipe, a fresh image would have \
the stock NXP SOF firmware and the ALSA card 'softac5212tdm' would not \
appear at boot, blocking mixer-pro startup."

HOMEPAGE = "https://github.com/Mjxkill/sof"
LICENSE = "Apache-2.0 & BSD-3-Clause"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://sof-imx8m.ri \
    file://sof-imx8mp-tac5212.tplg \
"

S = "${WORKDIR}"

inherit allarch

# On the target tree, /lib/firmware/imx/sof is a symlink to
# sof-zephyr-xcc/ (set up by the vendor sof-zephyr recipe). The kernel
# loader follows that symlink to fetch sof-imx8m.ri. We install our .ri
# into BOTH variant directories so either symlink target works, and the
# tplg into the shared sof-tplg/ directory.
do_install() {
    install -d ${D}${nonarch_base_libdir}/firmware/imx/sof-zephyr-xcc
    install -m 0644 ${WORKDIR}/sof-imx8m.ri \
        ${D}${nonarch_base_libdir}/firmware/imx/sof-zephyr-xcc/sof-imx8m.ri
    install -d ${D}${nonarch_base_libdir}/firmware/imx/sof-zephyr-gcc
    install -m 0644 ${WORKDIR}/sof-imx8m.ri \
        ${D}${nonarch_base_libdir}/firmware/imx/sof-zephyr-gcc/sof-imx8m.ri

    install -d ${D}${nonarch_base_libdir}/firmware/imx/sof-tplg
    install -m 0644 ${WORKDIR}/sof-imx8mp-tac5212.tplg \
        ${D}${nonarch_base_libdir}/firmware/imx/sof-tplg/sof-imx8mp-tac5212.tplg
}

FILES:${PN} = " \
    ${nonarch_base_libdir}/firmware/imx/sof-zephyr-xcc/sof-imx8m.ri \
    ${nonarch_base_libdir}/firmware/imx/sof-zephyr-gcc/sof-imx8m.ri \
    ${nonarch_base_libdir}/firmware/imx/sof-tplg/sof-imx8mp-tac5212.tplg \
"

# Loaded by the kernel at boot — RDEPENDS on the firmware path. Conflicts
# with sof-zephyr's sof-imx8m.ri but we want OUR file to win. Use a higher
# priority by ordering in IMAGE_INSTALL append.

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
