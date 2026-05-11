SUMMARY = "V7.0-E6.b Phone simulated 2x2 PCM via snd-aloop"
DESCRIPTION = "Charge snd-aloop avec une carte ALSA 'Phone' 2x2 (placeholder \
pour la pile VoIP/SIP future, en attendant un modem hardware ou cellular)."
HOMEPAGE = "https://github.com/Mjxkill/yocto-nxp-debix"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://snd-aloop-phone.conf;beginline=1;endline=2;md5=dec22cde4fc37699430d31ec356a1667"

SRC_URI = " \
    file://snd-aloop-phone.conf \
    file://snd-aloop-phone.service \
"

S = "${WORKDIR}"

inherit systemd allarch

do_install() {
    install -d ${D}${sysconfdir}/modprobe.d
    install -m 0644 ${WORKDIR}/snd-aloop-phone.conf ${D}${sysconfdir}/modprobe.d/snd-aloop-phone.conf

    install -d ${D}${sysconfdir}/modules-load.d
    echo "snd-aloop" > ${D}${sysconfdir}/modules-load.d/snd-aloop-phone.conf

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/snd-aloop-phone.service ${D}${systemd_unitdir}/system/snd-aloop-phone.service
}

FILES:${PN} = " \
    ${sysconfdir}/modprobe.d/snd-aloop-phone.conf \
    ${sysconfdir}/modules-load.d/snd-aloop-phone.conf \
    ${systemd_unitdir}/system/snd-aloop-phone.service \
"

SYSTEMD_SERVICE:${PN} = "snd-aloop-phone.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
