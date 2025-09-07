DESCRIPTION = "Dart SDK for Linux"
HOMEPAGE = "https://dart.dev"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=29b4ad63b1f1509efea6629404336393"

SRC_URI = "https://storage.googleapis.com/dart-archive/channels/stable/release/3.7.0/sdk/dartsdk-linux-arm64-release.zip"
SRC_URI:class-native = "https://storage.googleapis.com/dart-archive/channels/stable/release/3.7.0/sdk/dartsdk-linux-x64-release.zip"
SRC_URI:class-nativesdk = "https://storage.googleapis.com/dart-archive/channels/stable/release/3.7.0/sdk/dartsdk-linux-x64-release.zip"
SRC_URI[sha256sum] = "7c849abc0d06a130d26d71490d5f2b4b2fe1ca477b1a9cee6b6d870e6f9d626f"
SRC_URI:class-native[sha256sum] = "367b5a6f1364a1697dc597775e5cd7333c332363902683a0970158cbb978b80d"
SRC_URI:class-nativesdk[sha256sum] = "367b5a6f1364a1697dc597775e5cd7333c332363902683a0970158cbb978b80d"
S = "${WORKDIR}/dart-sdk"

inherit pkgconfig

BBCLASSEXTEND = "native nativesdk"


do_install() {
    install -d ${D}/opt/${PN}
    cp -r ${S}/* ${D}/opt/${PN}/
    install -d ${D}${bindir}
    ln -s /opt/${PN}/bin/dart ${D}${bindir}/dart
    ln -s /opt/${PN}/bin/pub ${D}${bindir}/pub
}

FILES:${PN} += "/opt/${PN}"

RDEPENDS:${PN} += "bash"

