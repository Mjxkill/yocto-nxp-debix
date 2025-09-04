FILESEXTRAPATHS:prepend := "${THISDIR}/:"
SRC_URI += "file://u-boot-spl-fitaddr.cfg"
UBOOT_CONFIG_FRAGMENTS:append = " u-boot-spl-fitaddr.cfg"

