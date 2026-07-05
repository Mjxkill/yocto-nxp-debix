FILESEXTRAPATHS:prepend := "${THISDIR}/:"

# Patch defconfig to set SPL FIT load address
SRC_URI:append = " file://0001-imx8mp_evk-set-spl-load-fit-address.patch"

# NB rebranding boot (V10-N6) : NE PAS recompiler u-boot — le build ne
# boote pas sur cette board. Le logo/bandeau A.L.A. sont appliqués par
# PATCH BINAIRE du flash.bin de référence (voir commit 8472cb1d :
# remplacements à tailles égales, FIT sans hash vérifié).
