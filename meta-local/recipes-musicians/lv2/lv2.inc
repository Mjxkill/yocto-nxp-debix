SUMMARY = "LV2 is an open standard for audio plugins"
LICENSE = "ISC"
LIC_FILES_CHKSUM = "file://COPYING;md5=b1b33d6fcf7d70cf1512e3c481768a9c"

PV = "1.18.10"
SRC_URI = "https://lv2plug.in/spec/lv2-${PV}.tar.xz"
SRC_URI[sha256sum] = "78c51bcf21b54e58bb6329accbb4dae03b2ed79b520f9a01e734bd9de530953f"
S = "${WORKDIR}/${BPN}-${PV}"

inherit meson pkgconfig
