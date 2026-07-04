# V10-P4b — weston.ini du projet : copie exacte du fichier board validé
# (base imx-nxp-bsp : use-g2d=true etc.) + rotation de l'écran DSI :
# le panneau est un 800x1280 PORTRAIT natif -> transform=rotate-90 donne
# le 1280x800 paysage attendu par le kiosk (TESTS_V10_P4a).
FILESEXTRAPATHS:prepend := "${THISDIR}/weston-init:"
# NB : le fichier DOIT être dans le sous-dossier d'override imx-nxp-bsp/ —
# meta-freescale fournit weston-init/imx-nxp-bsp/weston.ini et les overrides
# priment sur l'ordre des chemins FILESPATH (un weston.ini plat perdait).
