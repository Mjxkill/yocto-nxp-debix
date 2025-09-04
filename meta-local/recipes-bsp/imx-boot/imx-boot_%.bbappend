# Use FIT second loader at 0x40200000 without OP-TEE
DEPLOY_OPTEE:imx8mpevk = "false"

do_compile:append() {
    if [ "${MACHINE}" = "imx8mpevk" ]; then
        bbnote "Regenerating u-boot.itb and flash.bin for ${MACHINE}"
        rm -f ${BOOT_STAGING}/u-boot.itb ${BOOT_STAGING}/flash.bin
        ${STAGING_BINDIR_NATIVE}/mkimage -E -p 0x5000 -f ${BOOT_STAGING}/u-boot.its ${BOOT_STAGING}/u-boot.itb
        ${BOOT_STAGING}/mkimage_imx8 -version v2 -fit \
            -loader ${BOOT_STAGING}/u-boot-spl-ddr.bin 0x920000 \
            -second_loader ${BOOT_STAGING}/u-boot.itb 0x40200000 0x60000 \
            -out ${BOOT_STAGING}/flash.bin
    fi
}

do_deploy:append() {
    if [ "${MACHINE}" = "imx8mpevk" ]; then
        install -m 0644 ${BOOT_STAGING}/flash.bin ${DEPLOYDIR}/imx-boot-${MACHINE}-sd.bin-flash_evk
    fi
}

