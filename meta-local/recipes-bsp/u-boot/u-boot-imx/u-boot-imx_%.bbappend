FILESEXTRAPATHS:prepend := "${THISDIR}:"

# Add FIT load address configuration for SPL
SRC_URI:append = " file://u-boot-spl-fitaddr.cfg"
