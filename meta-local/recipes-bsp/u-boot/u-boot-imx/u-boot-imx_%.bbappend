FILESEXTRAPATHS:prepend := "${THISDIR}/:"

# Patch defconfig to set SPL FIT load address
SRC_URI:append = " file://0001-imx8mp_evk-set-spl-load-fit-address.patch"
