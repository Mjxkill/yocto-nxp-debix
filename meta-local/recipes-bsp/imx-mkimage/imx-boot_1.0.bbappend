# flash.bin PRÉBUILDÉ — SOURCE DE VÉRITÉ VERSIONNÉE (revue code 2026-07-28, F9)
#
# Provenance : U-Boot vendor Debix Model AB, PATCHÉ BINAIREMENT (SPL FIT load
# address, V10-NATIVE — voir docs + u-boot-imx bbappend). NE PAS recompiler
# u-boot-imx pour le remplacer : le u-boot buildé ne boote pas sur cette carte.
# md5 attendu : f88e3b46373ecde35cca9d8c1ea35434
#
# Priorité : 1) copie versionnée dans la layer (files/flash.bin — référence),
#            2) ${TOPDIR}/../flash.bin (copie de travail racine, historique).
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
IMX_BOOT_PREBUILT_LAYER = "${@bb.utils.which(d.getVar('FILESPATH'), 'flash.bin')}"
IMX_BOOT_PREBUILT ?= "${IMX_BOOT_PREBUILT_LAYER}"

do_deploy:append() {
    # If a known-good flash.bin is available (from vendor image), prefer it so WIC packs the working boot container.
    if [ -f "${IMX_BOOT_PREBUILT}" ]; then
        bbnote "Using prebuilt flash.bin at ${IMX_BOOT_PREBUILT} for imx-boot"
        # Garde-fou : le binaire patché est unique — vérifier le md5 attendu
        MD5=$(md5sum "${IMX_BOOT_PREBUILT}" | cut -d' ' -f1)
        if [ "$MD5" != "f88e3b46373ecde35cca9d8c1ea35434" ]; then
            bbfatal "flash.bin md5 inattendu ($MD5) — ce binaire est patché à la main (V10-NATIVE), toute divergence doit être volontaire : mettre à jour le md5 ici ET la provenance."
        fi
        install -m 0644 ${IMX_BOOT_PREBUILT} ${DEPLOYDIR}/imx-boot-prebuilt
        ln -sf imx-boot-prebuilt ${DEPLOYDIR}/${BOOT_NAME}
        ln -sf imx-boot-prebuilt ${DEPLOYDIR}/${BOOT_NAME}-tagged
        ln -sf imx-boot-prebuilt ${DEPLOYDIR}/${BOOT_NAME}-untagged
    else
        bbwarn "IMX_BOOT_PREBUILT not found (${IMX_BOOT_PREBUILT}); keeping generated imx-boot"
    fi
}
