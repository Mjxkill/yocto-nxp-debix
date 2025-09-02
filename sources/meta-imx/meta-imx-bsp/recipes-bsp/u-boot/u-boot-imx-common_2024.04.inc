DESCRIPTION = "i.MX U-Boot suppporting i.MX reference boards."

LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://Licenses/gpl-2.0.txt;md5=b234ee4d69f5fce4486a80fdaf4a4263"

#John_gao UBOOT_SRC ?= "git://github.com/nxp-imx/uboot-imx.git;protocol=https"
UBOOT_SRC ?= "git://github.com/debix-tech/uboot-nxp-debix.git;protocol=https"
SRC_URI = "${UBOOT_SRC};branch=${SRCBRANCH}"
SRCBRANCH = "lf_v2024.04-debix_model_ab_2gbddr"
LOCALVERSION ?= "-${SRCBRANCH}"
SRCREV = "32a28bda1e7f995703d828cf4891f86c905b4232"

DEPENDS += " \
    bc-native \
    bison-native \
    dtc-native \
    flex-native \
    gnutls-native \
    xxd-native \
"

S = "${WORKDIR}/git"
B = "${WORKDIR}/build"

inherit fsl-u-boot-localversion

BOOT_TOOLS = "imx-boot-tools"
