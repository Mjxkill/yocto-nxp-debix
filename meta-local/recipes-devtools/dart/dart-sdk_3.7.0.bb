DESCRIPTION = "Dart SDK for Linux"
HOMEPAGE = "https://dart.dev"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=29b4ad63b1f1509efea6629404336393"

SRC_URI = "https://storage.googleapis.com/dart-archive/channels/stable/release/3.7.0/sdk/dartsdk-linux-arm64-release.zip"
SRC_URI:class-native = "https://storage.googleapis.com/dart-archive/channels/stable/release/3.7.0/sdk/dartsdk-linux-x64-release.zip"
SRC_URI:class-nativesdk = "https://storage.googleapis.com/dart-archive/channels/stable/release/3.7.0/sdk/dartsdk-linux-x64-release.zip"
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

