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
    file://mixerctl.c \
    file://Makefile \
    file://mixer-pro.service \
"

S = "${WORKDIR}"

inherit systemd

DEPENDS = "alsa-lib"
RDEPENDS:${PN} = "alsa-lib"

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
# Disable par défaut : conflits potentiels avec loopback-c-lowlat et autres
# tests qui prennent les 3 paires PCMs en exclusif. L'utilisateur ou le GUI E7
# active manuellement.
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
