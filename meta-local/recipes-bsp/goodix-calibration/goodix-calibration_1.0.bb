SUMMARY = "Calibration tactile GT911 de la dalle DSI 8 pouces"
DESCRIPTION = "Matrice LIBINPUT_CALIBRATION_MATRIX mesurée avec l'ecran de \
calibration integre de mixer-console (5 mires, moindres carres). \
Recalibrable sur la board (SYSTEME > CALIBRER LE TACTILE) ; re-capturer la \
regle ici apres une recalibration definitive."
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-or-later;md5=fed54355545ffd980b814dab4a3b312c"

SRC_URI = "file://99-goodix-calibration.rules"
S = "${WORKDIR}"

do_install() {
    install -d ${D}${sysconfdir}/udev/rules.d
    install -m 0644 ${WORKDIR}/99-goodix-calibration.rules \
        ${D}${sysconfdir}/udev/rules.d/
}

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
