SUMMARY = "Prebuilt Flutter SDK"
LICENSE = "CLOSED"
SRC_URI = "https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_3.22.0-stable.tar.xz"
SRC_URI[sha256sum] = "0000000000000000000000000000000000000000000000000000000000000000"
S = "${WORKDIR}/flutter"

RDEPENDS:${PN} += "dart-sdk"

PACKAGE_ARCH = "${MACHINE_ARCH}"

do_install() {
    install -d ${D}/opt/flutter
    cp -r ${S}/* ${D}/opt/flutter/
    install -d ${D}/usr/bin
    ln -sf /opt/flutter/bin/flutter ${D}/usr/bin/flutter
}

FILES:${PN} += "/opt/flutter /usr/bin/flutter"
