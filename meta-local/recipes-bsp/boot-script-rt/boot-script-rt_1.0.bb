SUMMARY = "V8.33 U-Boot boot.scr — bootargs RT (isolcpus=2,3) pour mixer-pro"
DESCRIPTION = "Compile boot.cmd → boot.scr via mkimage et l'installe dans /boot. \
U-Boot charge ce script avant booti et ajoute isolcpus=2,3 nohz_full=2,3 \
rcu_nocbs=2,3 aux bootargs, ce qui réserve les cores 2 et 3 pour mixer-pro \
(configurés via CPUAffinity dans le .service)."
HOMEPAGE = "https://github.com/Mjxkill/yocto-nxp-debix"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://boot.cmd;beginline=1;endline=1;md5=8ebef8cbb1d0893d1336ce149c97b068"

SRC_URI = "file://boot.cmd"

S = "${WORKDIR}"

DEPENDS = "u-boot-tools-native"

inherit deploy

# Compile boot.cmd → boot.scr et l'installe directement sur la partition
# /boot (FAT mmcblk1p1) au runtime via package install scripts.
do_compile() {
    mkimage -A arm -O linux -T script -C none -n "Debix RT boot" \
        -d ${WORKDIR}/boot.cmd ${B}/boot.scr
}

do_install() {
    install -d ${D}/boot
    install -m 0644 ${B}/boot.scr ${D}/boot/boot.scr
}

# Aussi déployé pour permettre la copie manuelle si besoin
do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${B}/boot.scr ${DEPLOYDIR}/boot.scr
}
addtask deploy after do_compile before do_build

FILES:${PN} = "/boot/boot.scr"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
