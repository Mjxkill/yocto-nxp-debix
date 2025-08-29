IMAGE_INSTALL:append = "\
    pipewire pipewire-pulse wireplumber \
    gstreamer1.0 gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad ${@bb.utils.contains('LICENSE_FLAGS_ACCEPTED', 'commercial', 'gstreamer1.0-plugins-ugly', '', d)} \
    ladspa-sdk lv2 gstreamer1.0-plugins-ladspa \
    ardour tensorflow-lite armnn onnxruntime fftw libsamplerate \
"

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
