SUMMARY = "V7.0-E6.d mixer-pro — console DAW SW (26 in / 4 bus FX / 18 out)"
DESCRIPTION = "Daemon C userspace temps-réel. Lit 3 paires de PCMs ALSA (DSP \
TAC5212, UAC2 gadget, Phone aloop), applique une matrice de routing avec 4 \
sends stéréo vers 4 bus FX, recombine via une matrice master 34 → 18 outputs. \
Latence cible < 10 ms. Contrôle via socket Unix /run/mixer-pro.sock (JSON). \
E6.e : 4 effets natifs C par bus (compressor, reverb, delay, eq) pilotables \
via set_fx_param. Désactivé par défaut (le user/GUI E7 active manuellement)."
HOMEPAGE = "https://github.com/Mjxkill/yocto-nxp-debix"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://mixer-pro.c;beginline=1;endline=2;md5=cfa333c28b94c54d228eecd95f238319"

SRC_URI = " \
    file://mixer-pro.c \
    file://mixer-pro.h \
    file://state.h \
    file://util.c \
    file://util.h \
    file://dsp_bq.c \
    file://dsp_bq.h \
    file://sampler.c \
    file://sampler.h \
    file://effects.c \
    file://effects.h \
    file://analyzer.c \
    file://analyzer.h \
    file://ml_features.c \
    file://ml_features.h \
    file://ml_features_test.c \
    file://mixer_pro_shm_tap.c \
    file://mixer_pro_shm_tap.h \
    file://mixerctl.c \
    file://Makefile \
    file://mixer-pro.service \
"

S = "${WORKDIR}"

inherit systemd pkgconfig

# V9.5.12 — fftw pour ml_features.c (utilisé par programme test + daemon ML).
# tensorflow-lite RETIRÉ : l'inférence NPU est dans mixer-ml-inference (daemon
# séparé, voir meta-local/recipes-audio/mixer-ml-inference).
DEPENDS = "alsa-lib lilv fftw"
# libfftwf = paquet runtime réel de fftw simple précision (-lfftw3f) ;
# "fftw" n'existe pas comme paquet binaire → do_rootfs apt échouait (V10-P4b)
RDEPENDS:${PN} = "alsa-lib lilv lv2 libfftwf"

do_compile() {
    oe_runmake CC="${CC}" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}"
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/mixer-pro        ${D}${bindir}/mixer-pro
    install -m 0755 ${B}/mixerctl         ${D}${bindir}/mixerctl
    install -m 0755 ${B}/ml_features_test ${D}${bindir}/ml_features_test

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/mixer-pro.service ${D}${systemd_unitdir}/system/mixer-pro.service
}

FILES:${PN} = " \
    ${bindir}/mixer-pro \
    ${bindir}/mixerctl \
    ${bindir}/ml_features_test \
    ${systemd_unitdir}/system/mixer-pro.service \
"

SYSTEMD_SERVICE:${PN} = "mixer-pro.service"
# V9.1 : enable par défaut. mixer-pro = service principal de la board audio
# (= console DAW). Désactiver manuellement si tests loopback-c-lowlat
# (systemctl disable mixer-pro).
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
