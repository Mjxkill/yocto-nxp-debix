SUMMARY = "V10 kiosk : console mixer sur ecran DSI (chromium wayland kiosk)"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-or-later;md5=fed54355545ffd980b814dab4a3b312c"

SRC_URI = "file://mixer-kiosk.service"
S = "${WORKDIR}"

inherit systemd
SYSTEMD_SERVICE:${PN} = "mixer-kiosk.service"
# V10-P4b : GO/NO-GO valide (TESTS_V10_P4a : 3x60s lecture 0 xrun) — boot auto
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/mixer-kiosk.service ${D}${systemd_system_unitdir}/
}

RDEPENDS:${PN} = "chromium-ozone-wayland weston mixer-gui-http curl"
