SUMMARY = "V9.5.20 mixer-ml-inference — daemon ML mastering enveloppe spectrale NPU"
DESCRIPTION = "Process séparé qui lit l'audio (NPU TAP IN /dev/imx-audio-tap-in \
pour HW IN, ou /dev/shm/mixer-pro-tap-usb pour USB IN), calcule les features \
(FFT 1024 + 11 floats), invoque le modèle TFLite INT8 sur NPU via VX delegate, \
et push les 62 params LV2 à mixer-pro via socket Unix set_insert_params_bulk. \
Isolation process critique : TFLite NPU + VX delegate dans le même process \
que les threads RT99 de mixer-pro provoque un freeze kernel (galcore + IRQ \
storm vs audio_thread). Process séparé = pas d'interférence."
HOMEPAGE = "https://github.com/Mjxkill/yocto-nxp-debix"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://mixer-ml-inference.c;beginline=1;endline=2;md5=7eded808ddbf99a9e00fc0258990e9da"

SRC_URI = " \
    file://mixer-ml-inference.c \
    file://ml_features_v3.c \
    file://ml_features_v3.h \
    file://ml_features_v3_tables.h \
    file://mixer_pro_shm_tap.h \
    file://imx-audio-tap-uapi.h \
    file://Makefile \
    file://mixer-ml-inference.service \
"

S = "${WORKDIR}"

inherit systemd

# fftw pour features FFT, tensorflow-lite (host + VX delegate runtime).
DEPENDS = "fftw tensorflow-lite"
RDEPENDS:${PN} = "fftw tensorflow-lite tensorflow-lite-vx-delegate mixer-pro"

do_compile() {
    oe_runmake CC="${CC}" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}"
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/mixer-ml-inference ${D}${bindir}/mixer-ml-inference

    install -d ${D}${sysconfdir}/mixer-pro
    # Le modèle est installé par le pipeline d'export (scp manuel) à
    # /etc/mixer-pro/mastering_v5_12_int8.tflite.

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/mixer-ml-inference.service \
        ${D}${systemd_unitdir}/system/mixer-ml-inference.service
}

FILES:${PN} = "/etc/mixer-pro  \
    ${bindir}/mixer-ml-inference \
    ${systemd_unitdir}/system/mixer-ml-inference.service \
"

SYSTEMD_SERVICE:${PN} = "mixer-ml-inference.service"
# Disabled par défaut — user active via systemctl enable + set_assistant_mode.
# V10-P4f : demarre au boot — sans lui le mode mastering est inerte
# (mode accepte par mixer-pro mais aucun parametre pousse vers l insert)
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
