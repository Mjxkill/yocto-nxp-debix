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
    file://effects.c \
    file://effects.h \
    file://analyzer.c \
    file://analyzer.h \
    file://mixerctl.c \
    file://Makefile \
    file://mixer-pro.service \
"

S = "${WORKDIR}"

inherit systemd pkgconfig

DEPENDS = "alsa-lib lilv"
# V9.2 — lilv = LV2 host library. RDEPENDS sur lilv + lv2 (core spec)
# pour avoir libraries + core LV2 namespaces dispo au runtime. Plugins LV2
# tiers (Calf, x42, …) optionnels — l'utilisateur installe via dpkg / Yocto
# IMAGE_INSTALL selon ses besoins ; mixer-pro charge dynamiquement.
RDEPENDS:${PN} = "alsa-lib lilv lv2"

do_compile() {
    oe_runmake CC="${CC}" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}"
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/mixer-pro ${D}${bindir}/mixer-pro
    install -m 0755 ${B}/mixerctl  ${D}${bindir}/mixerctl

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/mixer-pro.service ${D}${systemd_unitdir}/system/mixer-pro.service
}

FILES:${PN} = " \
    ${bindir}/mixer-pro \
    ${bindir}/mixerctl \
    ${systemd_unitdir}/system/mixer-pro.service \
"

SYSTEMD_SERVICE:${PN} = "mixer-pro.service"
# V9.1 : enable par défaut. mixer-pro = service principal de la board audio
# (= console DAW). Désactiver manuellement si tests loopback-c-lowlat
# (systemctl disable mixer-pro).
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
