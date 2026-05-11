SUMMARY = "V7.0-E6.c ALSA route bridge — matrice N×M déclarative via alsaloop"
DESCRIPTION = "Lit /etc/alsa-route-bridge.conf et lance N alsaloop pour réaliser \
le routing entre les 3 paires de cartes ALSA (DSP TAC5212, UAC2Gadget, Phone). \
Désactivé par défaut au boot ; le GUI E7 ou l'utilisateur active selon usage. \
Édit du conf + restart service pour appliquer."
HOMEPAGE = "https://github.com/Mjxkill/yocto-nxp-debix"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://alsa-route-start.sh;beginline=1;endline=2;md5=6d41d4de7494c00c5469ba42c64d748e"

SRC_URI = " \
    file://alsa-route-bridge.conf \
    file://alsa-route-start.sh \
    file://alsa-route-stop.sh \
    file://alsa-route-bridge.service \
"

S = "${WORKDIR}"

inherit systemd allarch

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/alsa-route-start.sh ${D}${bindir}/alsa-route-start.sh
    install -m 0755 ${WORKDIR}/alsa-route-stop.sh  ${D}${bindir}/alsa-route-stop.sh

    install -d ${D}${sysconfdir}
    install -m 0644 ${WORKDIR}/alsa-route-bridge.conf ${D}${sysconfdir}/alsa-route-bridge.conf

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/alsa-route-bridge.service ${D}${systemd_unitdir}/system/alsa-route-bridge.service
}

FILES:${PN} = " \
    ${bindir}/alsa-route-start.sh \
    ${bindir}/alsa-route-stop.sh \
    ${sysconfdir}/alsa-route-bridge.conf \
    ${systemd_unitdir}/system/alsa-route-bridge.service \
"

SYSTEMD_SERVICE:${PN} = "alsa-route-bridge.service"
# Disable par défaut : l'utilisateur active selon ses usages (sinon conflits
# avec loopback-c-lowlat ou autres tests qui occupent les PCMs)
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

RDEPENDS:${PN} = "alsa-utils"
COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
