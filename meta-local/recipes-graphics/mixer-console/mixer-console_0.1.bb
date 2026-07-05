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
    file://main.qml \
"

S = "${WORKDIR}"

inherit qt6-cmake

DEPENDS = "qtbase qtdeclarative qtdeclarative-native"
RDEPENDS:${PN} = "qtbase qtdeclarative"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
