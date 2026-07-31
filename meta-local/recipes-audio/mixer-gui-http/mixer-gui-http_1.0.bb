SUMMARY = "V7.0-E7 mixer-gui-http — HTTP GUI for mixer-pro daemon"
DESCRIPTION = "Daemon HTTP C (libmicrohttpd) qui sert un GUI statique (HTML + \
Alpine.js + Tailwind via CDN) et bridge des appels REST vers le socket Unix \
/run/mixer-pro.sock du daemon mixer-pro. Permet de piloter le mixer 26 in × \
4 bus FX × 18 out + 4 effets natifs depuis un navigateur sur le LAN. Pool de \
4 sockets Unix persistants pour réduire l'overhead côté mixer-pro."
HOMEPAGE = "https://github.com/Mjxkill/yocto-nxp-debix"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://http_core.c;beginline=1;endline=2;md5=cfa333c28b94c54d228eecd95f238319"

SRC_URI = " \
    file://http_core.c \
    file://mixer_bridge.c \
    file://sse_state.c \
    file://alsa_ctl.c \
    file://api_routes.c \
    file://gui_http.h \
    file://Makefile \
    file://mixer-gui-http.service \
    file://ala-fx-restore.sh \
    file://ala-fx-restore.service \
    file://www/beta.html \
    file://www/beta.css \
    file://www/js/beta_core.js \
    file://www/js/beta_bridge.js \
    file://www/js/beta_pages.js \
    file://www/js/beta_fx.js \
    file://www/js/beta_perf.js \
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
    # V14.0 : beta.html = LA console, servie à la racine (index V7 supprimée)
    # Étape 7 : CSS + JS extraits en fichiers locaux (zéro CDN inchangé)
    install -m 0644 ${WORKDIR}/www/beta.html ${D}/var/www/mixer-gui/beta.html
    install -d ${D}/var/www/mixer-gui/static/js
    install -m 0644 ${WORKDIR}/www/beta.css  ${D}/var/www/mixer-gui/static/beta.css
    install -m 0644 ${WORKDIR}/www/js/*.js   ${D}/var/www/mixer-gui/static/js/

    # V10-N7b : restauration effets TAC/DSP au boot (post tac-reset)
    install -m 0755 ${WORKDIR}/ala-fx-restore.sh ${D}${bindir}/ala-fx-restore.sh

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/mixer-gui-http.service \
        ${D}${systemd_unitdir}/system/mixer-gui-http.service
    install -m 0644 ${WORKDIR}/ala-fx-restore.service \
        ${D}${systemd_unitdir}/system/ala-fx-restore.service
}

FILES:${PN} = " \
    ${bindir}/mixer-gui-http \
    ${bindir}/ala-fx-restore.sh \
    /var/www/mixer-gui/beta.html \
    /var/www/mixer-gui/static \
    ${systemd_unitdir}/system/mixer-gui-http.service \
    ${systemd_unitdir}/system/ala-fx-restore.service \
"

RDEPENDS:${PN} += "curl alsa-utils-alsactl"

SYSTEMD_SERVICE:${PN} = "mixer-gui-http.service ala-fx-restore.service"
# V10-P4b : GO kiosk acté (TESTS_V10_P4a) — la GUI démarre au boot
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
