FILESEXTRAPATHS:prepend := "${THISDIR}/:"

# Apply defconfig patch so SPL loads the FIT image from the correct address
SRC_URI:append:imx8mpevk = " \
    file://0001-imx8mp_evk-set-spl-load-fit-address.patch;patch=1 \
    "
