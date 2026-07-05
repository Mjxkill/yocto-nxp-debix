SUMMARY = "V10-NATIVE : console mixer native Qt6/QML (eglfs, sans wayland)"
DESCRIPTION = "Application ecran de la console Model AB : rendu GPU Vivante \
en plein ecran direct DRM/KMS (plateforme eglfs), aucun compositeur. \
Remplace le kiosk chromium (85-93 % CPU) — cible <= 15 % d'un core. \
N0 = squelette chassis + FPS (GO/NO-GO eglfs/Vivante, ARCHI_V10_NATIVE §5)."
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-or-later;md5=fed54355545ffd980b814dab4a3b312c"

SRC_URI = " \
    file://CMakeLists.txt \
    file://main.cpp \
    file://mixerclient.h \
    file://mixerclient.cpp \
    file://calibration.h \
    file://calibration.cpp \
    file://main.qml \
    file://qml/Strip.qml \
    file://qml/Fader.qml \
    file://qml/Knob.qml \
    file://qml/MeterBar.qml \
    file://qml/VUNeedle.qml \
    file://qml/SpectrumView.qml \
    file://qml/PageEffects.qml \
    file://qml/PageMastering.qml \
    file://qml/PageRouting.qml \
    file://qml/PageSystem.qml \
    file://qml/CalibrationOverlay.qml \
    file://qml/StripFxDrawer.qml \
    file://qml/IntroOverlay.qml \
    file://mixer-console.service \
    file://qml/stripfx.js \
"

S = "${WORKDIR}"

inherit qt6-cmake systemd

DEPENDS = "qtbase qtdeclarative qtdeclarative-native"
RDEPENDS:${PN} = "qtbase qtdeclarative"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/mixer-console.service ${D}${systemd_system_unitdir}/
    # config KMS : le DSI est card1 (eglfs choisit card0 par défaut)
    install -d ${D}${sysconfdir}
    echo "{ \"device\": \"/dev/dri/card1\" }" > ${D}${sysconfdir}/mixer-console-kms.json
}

FILES:${PN} += "${systemd_system_unitdir}/mixer-console.service ${sysconfdir}/mixer-console-kms.json"
SYSTEMD_SERVICE:${PN} = "mixer-console.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
