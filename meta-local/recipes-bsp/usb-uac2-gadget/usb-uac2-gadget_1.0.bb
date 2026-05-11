SUMMARY = "V7.0-E6.a USB UAC2 8x8 gadget setup (configfs + systemd)"
DESCRIPTION = "Expose le board Debix comme carte son USB 8 ch in + 8 ch out vers \
un PC hôte. Stream 48 kHz S32_LE. Setup via configfs au boot (oneshot service). \
UDC = 38100000.usb (1er DWC3). usb_f_uac2 + libcomposite sont builtin kernel."
HOMEPAGE = "https://github.com/Mjxkill/yocto-nxp-debix"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://usb-uac2-setup.sh;beginline=1;endline=2;md5=6d41d4de7494c00c5469ba42c64d748e"

SRC_URI = " \
    file://usb-uac2-setup.sh \
    file://usb-uac2-gadget.service \
"

S = "${WORKDIR}"

inherit systemd allarch

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/usb-uac2-setup.sh ${D}${bindir}/usb-uac2-setup.sh

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/usb-uac2-gadget.service ${D}${systemd_unitdir}/system/usb-uac2-gadget.service
}

FILES:${PN} = " \
    ${bindir}/usb-uac2-setup.sh \
    ${systemd_unitdir}/system/usb-uac2-gadget.service \
"

SYSTEMD_SERVICE:${PN} = "usb-uac2-gadget.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

RDEPENDS:${PN} = "bash"
COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
