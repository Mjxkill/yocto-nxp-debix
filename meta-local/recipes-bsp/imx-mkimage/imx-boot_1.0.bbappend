IMX_BOOT_PREBUILT ?= "${TOPDIR}/../flash.bin"

do_deploy:append() {
    # If a known-good flash.bin is available (from vendor image), prefer it so WIC packs the working boot container.
    if [ -f "${IMX_BOOT_PREBUILT}" ]; then
        bbnote "Using prebuilt flash.bin at ${IMX_BOOT_PREBUILT} for imx-boot"
        install -m 0644 ${IMX_BOOT_PREBUILT} ${DEPLOYDIR}/imx-boot-prebuilt
        ln -sf imx-boot-prebuilt ${DEPLOYDIR}/${BOOT_NAME}
        ln -sf imx-boot-prebuilt ${DEPLOYDIR}/${BOOT_NAME}-tagged
        ln -sf imx-boot-prebuilt ${DEPLOYDIR}/${BOOT_NAME}-untagged
    else
        bbwarn "IMX_BOOT_PREBUILT not found (${IMX_BOOT_PREBUILT}); keeping generated imx-boot"
    fi
}
