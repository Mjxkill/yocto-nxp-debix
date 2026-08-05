SUMMARY = "V16 voice-clean — nettoyage de la voix (DTLN TFLite CPU / soustraction spectrale)"
DESCRIPTION = "Daemon CPU3 FIFO60 : consomme le ring SHM voix+réf de mixer-pro, \
applique le mode choisi (bouton BRUT/DTLN/GTCRN/SPECSUB), repousse la voix \
traitée. R&D écoute — ARCHI_V16_VOICE_CLEAN.md."
HOMEPAGE = "https://github.com/Mjxkill/yocto-nxp-debix"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://voice-clean.c;beginline=1;endline=2;md5=7eded808ddbf99a9e00fc0258990e9da"

SRC_URI = " \
    file://voice-clean.c \
    file://voice_clean_shm.h \
    file://Makefile \
    file://voice-clean.service \
    file://dtln_1.tflite \
    file://dtln_2.tflite \
"

S = "${WORKDIR}"

inherit systemd pkgconfig

DEPENDS = "fftw tensorflow-lite"
RDEPENDS:${PN} = "libfftwf tensorflow-lite mixer-pro"

do_compile() {
    oe_runmake CC="${CC}" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}"
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/voice-clean ${D}${bindir}/voice-clean
    install -d ${D}${datadir}/voice-clean
    install -m 0644 ${WORKDIR}/dtln_1.tflite ${D}${datadir}/voice-clean/
    install -m 0644 ${WORKDIR}/dtln_2.tflite ${D}${datadir}/voice-clean/
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/voice-clean.service ${D}${systemd_unitdir}/system/
}

FILES:${PN} = " \
    ${bindir}/voice-clean \
    ${datadir}/voice-clean \
    ${systemd_unitdir}/system/voice-clean.service \
"

SYSTEMD_SERVICE:${PN} = "voice-clean.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
