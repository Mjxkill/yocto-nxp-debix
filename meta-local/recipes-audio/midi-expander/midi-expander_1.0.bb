SUMMARY = "A.L.A. MIDI expander - SoundFont sound module into mixer P1/P2"
DESCRIPTION = "Daemon fluidsynth : MIDI in (gadget USB f_midi) vers ring SHM \
consomme par mixer-pro. SoundFont GeneralUser GS (S. Christian Collins, \
licence libre GeneralUser)."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

DEPENDS = "fluidsynth"

SRC_URI = " \
    file://midi-expander.c \
    file://Makefile \
    file://midi-expander.service \
    file://midi-expander.conf \
    https://github.com/mrbumpy409/GeneralUser-GS/raw/main/GeneralUser-GS.sf2;name=sf2;downloadfilename=GeneralUser-GS.sf2 \
"
SRC_URI[sf2.sha256sum] = "9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe"

S = "${WORKDIR}"

inherit systemd pkgconfig

SYSTEMD_SERVICE:${PN} = "midi-expander.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_compile() {
    oe_runmake midi-expander
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 midi-expander ${D}${bindir}/

    install -d ${D}${sysconfdir}/ala
    install -m 0644 midi-expander.conf ${D}${sysconfdir}/ala/

    install -d ${D}${datadir}/sounds/sf2
    install -m 0644 ${WORKDIR}/GeneralUser-GS.sf2 \
        ${D}${datadir}/sounds/sf2/GeneralUserGS.sf2

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 midi-expander.service ${D}${systemd_system_unitdir}/
}

FILES:${PN} += "${datadir}/sounds/sf2"
