SUMMARY = "Systemd unit to autostart flutter-embedded on Wayland"
LICENSE = "CLOSED"

inherit systemd

RDEPENDS:${PN} = "flutter-embedded flutter-sdk"

SRC_URI = "file://flutter-embedded.service"

S = "${WORKDIR}"

SYSTEMD_SERVICE:${PN} = "flutter-embedded.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/flutter-embedded.service ${D}${systemd_system_unitdir}/
}

FILES:${PN} += "${systemd_system_unitdir}/flutter-embedded.service"
