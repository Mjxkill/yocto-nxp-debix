SUMMARY = "Prebuilt Dart SDK"
LICENSE = "CLOSED"
SRC_URI = "https://storage.googleapis.com/dart-archive/channels/stable/release/3.7.0/sdk/dartsdk-linux-arm64-release.zip"
SRC_URI[sha256sum] = "0000000000000000000000000000000000000000000000000000000000000000"
S = "${WORKDIR}/dart-sdk"

inherit unzip

RDEPENDS:${PN} = ""

PACKAGE_ARCH = "${MACHINE_ARCH}"

do_install() {
    install -d ${D}/opt/dart-sdk
    cp -r ${S}/* ${D}/opt/dart-sdk/
    install -d ${D}/usr/bin
    ln -sf /opt/dart-sdk/bin/dart ${D}/usr/bin/dart
}

FILES:${PN} += "/opt/dart-sdk /usr/bin/dart"
