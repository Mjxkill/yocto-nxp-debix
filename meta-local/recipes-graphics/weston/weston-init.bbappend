# V10-P4f — weston.ini du projet (rotation ecran DSI).
# Le panneau DSI est un 800x1280 PORTRAIT natif : transform=rotate-90 donne
# le 1280x800 paysage attendu par le kiosk (TESTS_V10_P4a).
# NB : un weston.ini homonyme ne PEUT PAS gagner ici — meta-imx/meta-freescale
# precedent meta-local dans FILESPATH pour chaque sous-dossier d'override.
# D'ou : nom de fichier unique + ecrasement explicite a l'install.
FILESEXTRAPATHS:prepend := "${THISDIR}/weston-init:"
SRC_URI += "file://weston-project.ini"

do_install:append() {
    install -m 0644 ${WORKDIR}/weston-project.ini ${D}${sysconfdir}/xdg/weston/weston.ini
}
