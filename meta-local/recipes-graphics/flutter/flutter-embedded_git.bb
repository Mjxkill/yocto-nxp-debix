DESCRIPTION = "Example Flutter embedder for embedded Linux"
HOMEPAGE = "https://github.com/sony/flutter-embedded-linux"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=d45359c88eb146940e4bede4f08c821a"

SRC_URI = "git://github.com/sony/flutter-embedded-linux.git;branch=master;protocol=https"
SRC_URI += " file://use-flutter-sdk-engine.patch"
SRCREV = "1653fa656bf9fe9fa5f84789a59b0c07671a8fc8"

S = "${WORKDIR}/git"

inherit cmake pkgconfig

FLUTTER_ENGINE_SUBDIR = "${@'linux-arm64-release' if d.getVar('TARGET_ARCH') == 'aarch64' else 'linux-x64-release'}"
FLUTTER_ENGINE_LIB_PATH = "${RECIPE_SYSROOT}/opt/flutter-sdk/bin/cache/artifacts/engine/${FLUTTER_ENGINE_SUBDIR}/libflutter_engine.so"

# Link against Flutter engine which requires fontconfig symbols at link time
DEPENDS += "wayland wayland-native virtual/egl libxkbcommon flutter-sdk fontconfig"
EXTRA_OECMAKE += "-DUSER_PROJECT_PATH=${S}/examples/flutter-wayland-client"
EXTRA_OECMAKE += " -DFLUTTER_EMBEDDER_LIB=${FLUTTER_ENGINE_LIB_PATH}"

# do_install simply installs demo binary if build executed
# do_install will not run under -n parse

do_install() {
    install -d ${D}${bindir}
    # Upstream target name varies; prefer flutter-client, fallback to embedding_demo
    if [ -f ${B}/flutter-client ]; then
        install -m0755 ${B}/flutter-client ${D}${bindir}/flutter-embedded
    elif [ -f ${B}/embedding_demo ]; then
        install -m0755 ${B}/embedding_demo ${D}${bindir}/flutter-embedded
    fi
}

FILES:${PN} += "${bindir}/flutter-embedded"

RDEPENDS:${PN} += "flutter-sdk"
