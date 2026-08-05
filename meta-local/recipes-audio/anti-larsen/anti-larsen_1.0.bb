SUMMARY = "V11-AL anti-larsen automatique (AFS) pour la console A.L.A."
DESCRIPTION = "Daemon userspace : lit le tap FX (play post-effets), détecte \
les raies de larsen (heuristique AFS : seuil + PNR + persistance + \
non-harmonicité) et pose des notchs RBJ dans les biquads DAC du TAC5212 \
(slots BQ 7-12). ARCHI/ARCHI_V11_ANTILARSEN.md."
HOMEPAGE = "https://github.com/Mjxkill/yocto-nxp-debix"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://anti-larsen.c;beginline=1;endline=1;md5=6d92a7bdd65350edea729bdff79cfd8c"

SRC_URI = " \
    file://anti-larsen.c \
    file://Makefile \
    file://anti-larsen.service \
    file://anti-larsen.conf \
"

S = "${WORKDIR}"

inherit systemd

DEPENDS = "fftwf"
RDEPENDS:${PN} = "libfftwf mixer-pro"

do_compile() {
    oe_runmake CC="${CC}" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}"
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/anti-larsen ${D}${bindir}/anti-larsen

    install -d ${D}${sysconfdir}/mixer-pro
    install -m 0644 ${WORKDIR}/anti-larsen.conf \
        ${D}${sysconfdir}/mixer-pro/anti-larsen.conf

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/anti-larsen.service \
        ${D}${systemd_unitdir}/system/anti-larsen.service
}

FILES:${PN} = " \
    ${bindir}/anti-larsen \
    ${sysconfdir}/mixer-pro/anti-larsen.conf \
    ${systemd_unitdir}/system/anti-larsen.service \
"

CONFFILES:${PN} = "${sysconfdir}/mixer-pro/anti-larsen.conf"

SYSTEMD_SERVICE:${PN} = "anti-larsen.service"
# enable=0 dans la conf par défaut : le service démarre, rend les slots
# flat et sort — activation réelle via la conf (E0/E1 board d'abord)
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
