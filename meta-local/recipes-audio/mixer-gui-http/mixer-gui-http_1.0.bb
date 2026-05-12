SUMMARY = "V7.0-E7 mixer-gui-http — HTTP GUI for mixer-pro daemon"
DESCRIPTION = "Daemon HTTP C (libmicrohttpd) qui sert un GUI statique (HTML + \
Alpine.js + Tailwind via CDN) et bridge des appels REST vers le socket Unix \
/run/mixer-pro.sock du daemon mixer-pro. Permet de piloter le mixer 26 in × \
4 bus FX × 18 out + 4 effets natifs depuis un navigateur sur le LAN. Pool de \
4 sockets Unix persistants pour réduire l'overhead côté mixer-pro."
HOMEPAGE = "https://github.com/Mjxkill/yocto-nxp-debix"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://mixer-gui-http.c;beginline=1;endline=2;md5=7eded808ddbf99a9e00fc0258990e9da"

SRC_URI = " \
    file://mixer-gui-http.c \
    file://Makefile \
    file://mixer-gui-http.service \
    file://www/index.html \
"

S = "${WORKDIR}"

inherit systemd

DEPENDS = "libmicrohttpd alsa-lib"
RDEPENDS:${PN} = "libmicrohttpd alsa-lib mixer-pro"

do_compile() {
    oe_runmake CC="${CC}" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}"
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/mixer-gui-http ${D}${bindir}/mixer-gui-http

    install -d ${D}/var/www/mixer-gui
    install -m 0644 ${WORKDIR}/www/index.html ${D}/var/www/mixer-gui/index.html

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/mixer-gui-http.service \
        ${D}${systemd_unitdir}/system/mixer-gui-http.service
}

FILES:${PN} = " \
    ${bindir}/mixer-gui-http \
    /var/www/mixer-gui/index.html \
    ${systemd_unitdir}/system/mixer-gui-http.service \
"

SYSTEMD_SERVICE:${PN} = "mixer-gui-http.service"
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
