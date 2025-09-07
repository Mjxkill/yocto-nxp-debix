DESCRIPTION = "Flutter SDK for Linux"
HOMEPAGE = "https://flutter.dev"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=1d84cf16c48e571923f837136633a265"

SRC_URI = "https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_3.13.9-stable.tar.xz"
SRC_URI:class-native = "https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_3.13.9-stable.tar.xz"
SRC_URI:class-nativesdk = "https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_3.13.9-stable.tar.xz"
SRC_URI[sha256sum] = "b6bc6f93423488c67110e0fe56523cd2260f3a4c379ed015cd1c7fab66362739"
SRC_URI:class-native[sha256sum] = "b6bc6f93423488c67110e0fe56523cd2260f3a4c379ed015cd1c7fab66362739"
SRC_URI:class-nativesdk[sha256sum] = "b6bc6f93423488c67110e0fe56523cd2260f3a4c379ed015cd1c7fab66362739"
S = "${WORKDIR}/flutter"

inherit pkgconfig

BBCLASSEXTEND = "native nativesdk"

do_install() {
    install -d ${D}/opt/${PN}
    cp -r ${S}/* ${D}/opt/${PN}/
    install -d ${D}${bindir}
    ln -s /opt/${PN}/bin/flutter ${D}${bindir}/flutter
}

FILES:${PN} += "/opt/${PN}"

RDEPENDS:${PN} += "bash dart-sdk"

