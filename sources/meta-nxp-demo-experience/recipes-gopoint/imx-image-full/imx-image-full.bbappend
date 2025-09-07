ROOTFS_POSTPROCESS_COMMAND:append:mx93-nxp-bsp = "install_demo_93; "
ROOTFS_POSTPROCESS_COMMAND:append:mx8-nxp-bsp = "install_demo; "
ROOTFS_POSTPROCESS_COMMAND:append:mx95-nxp-bsp = "install_demo; "
ROOTFS_POSTPROCESS_COMMAND:append:mx7ulp-nxp-bsp = "install_demo; "

install_demo() {
    if ! grep -q "icon=${GPNT_APPS_FOLDER}/icon/icon_demo_launcher.png" ${IMAGE_ROOTFS}${sysconfdir}/xdg/weston/weston.ini
    then
       printf "\n[launcher]\nicon=${GPNT_APPS_FOLDER}/icon/icon_demo_launcher.png\npath=/usr/bin/gopoint\n\n[launcher]\nicon=/usr/share/weston/terminal.png\npath=/usr/bin/weston-terminal" >> ${IMAGE_ROOTFS}${sysconfdir}/xdg/weston/weston.ini
    fi
    if ! grep -q "HOME=/root/" ${IMAGE_ROOTFS}${sysconfdir}/default/weston
    then
        printf "\nHOME=/root/\nQT_QPA_PLATFORM=wayland" >> ${IMAGE_ROOTFS}${sysconfdir}/default/weston
    fi
}

install_demo_93() {
    if ! grep -q "icon=${GPNT_APPS_FOLDER}/icon/icon_demo_launcher.png" ${IMAGE_ROOTFS}${sysconfdir}/xdg/weston/weston.ini
    then
       printf "\n[launcher]\nicon=${GPNT_APPS_FOLDER}/icon/icon_demo_launcher.png\npath=QMLSCENE_DEVICE=softwarecontext /usr/bin/gopoint\n\n[launcher]\nicon=/usr/share/weston/terminal.png\npath=/usr/bin/weston-terminal" >> ${IMAGE_ROOTFS}${sysconfdir}/xdg/weston/weston.ini
    fi
    if ! grep -q "HOME=/root/" ${IMAGE_ROOTFS}${sysconfdir}/default/weston
    then
        printf "\nHOME=/root/\nQT_QPA_PLATFORM=wayland" >> ${IMAGE_ROOTFS}${sysconfdir}/default/weston
    fi
}

# Patch the deployed WIC image with flash.bin at seek=32, bs=1k, count=4064
patch_wic_flash() {
    SEEK_BLOCKS=32
    BS_BYTES=1024
    COUNT_BLOCKS=4064

    # Locate flash.bin relative to TOPDIR
    for CAND in "${TOPDIR}/../../flash.bin" "${TOPDIR}/../flash.bin" "${TOPDIR}/flash.bin"; do
        if [ -f "$CAND" ]; then
            FLASH_BIN="$CAND"
            break
        fi
    done

    [ -z "${FLASH_BIN:-}" ] && { bbwarn "flash.bin not found; skip WIC patch"; return 0; }

    # Target WIC filename: ${IMAGE_LINK_NAME}.wic (e.g. imx-image-full-${MACHINE}.rootfs.wic)
    TARGET_WIC="${DEPLOY_DIR_IMAGE}/${IMAGE_LINK_NAME}.wic"
    if [ ! -f "$TARGET_WIC" ]; then
        ALT_WIC="${DEPLOY_DIR_IMAGE}/imx-image-full-${MACHINE}.rootfs.wic"
        [ -f "$ALT_WIC" ] && TARGET_WIC="$ALT_WIC"
    fi

    [ -f "$TARGET_WIC" ] || { bbwarn "WIC not found: ${TARGET_WIC}; skipping"; return 0; }

    bbnote "Patching ${TARGET_WIC} with ${FLASH_BIN} at offset ${SEEK_BLOCKS}*${BS_BYTES} (no zero fill)"
    dd if="$FLASH_BIN" of="$TARGET_WIC" bs=$BS_BYTES seek=$SEEK_BLOCKS count=$COUNT_BLOCKS conv=notrunc status=none
}

# Dedicated task to run after image creation
do_patch_wic() {
    patch_wic_flash
}

addtask patch_wic after do_image_wic do_image_complete before do_build
