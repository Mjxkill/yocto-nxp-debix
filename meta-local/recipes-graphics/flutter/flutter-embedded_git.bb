DESCRIPTION = "Example Flutter embedder for embedded Linux"
HOMEPAGE = "https://github.com/sony/flutter-embedded-linux"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=d45359c88eb146940e4bede4f08c821a"

SRC_URI = "git://github.com/sony/flutter-embedded-linux.git;branch=master;protocol=https"
SRCREV = "1653fa656bf9fe9fa5f84789a59b0c07671a8fc8"

S = "${WORKDIR}/git"

inherit cmake pkgconfig

DEPENDS += "wayland wayland-native virtual/egl"
EXTRA_OECMAKE += "-DUSER_PROJECT_PATH=${S}/examples/flutter-wayland-client"

# do_install simply installs demo binary if build executed
# do_install will not run under -n parse

do_install() {
    install -d ${D}${bindir}
    if [ -f ${B}/embedding_demo ]; then
        install -m0755 ${B}/embedding_demo ${D}${bindir}/flutter-embedded
    fi
}

FILES:${PN} += "${bindir}/flutter-embedded"

RDEPENDS:${PN} += "flutter-sdk"
