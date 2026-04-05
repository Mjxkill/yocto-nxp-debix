SUMMARY = "TAC5212 boot-time reset service and script"
DESCRIPTION = "Installs tac-reset script and systemd service for automatic \
TAC5212 codec initialization at boot"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://tac-reset.sh \
    file://tac-reset.service \
"

S = "${WORKDIR}"

inherit systemd

SYSTEMD_SERVICE:${PN} = "tac-reset.service"
SYSTEMD_AUTO_ENABLE = "enable"

RDEPENDS:${PN} = "i2c-tools alsa-utils python3-core bash"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/tac-reset.sh ${D}${bindir}/tac-reset

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/tac-reset.service ${D}${systemd_unitdir}/system/
}

FILES:${PN} = " \
    ${bindir}/tac-reset \
    ${systemd_unitdir}/system/tac-reset.service \
"
