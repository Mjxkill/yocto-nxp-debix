# V7.0 — remove the vendor sof-imx8m.ri so our sof-firmware-custom recipe
# can ship the project-built firmware (TAC5212 + multiband_drc multi-config
# + NPU dual-tap patches) at the same path without a file-conflict QA error.
#
# The vendor sof-zephyr ships the WM8960/WM8962 topologies and a stock .ri
# that does not bind our TAC5212 + custom audio components. We keep its
# other files (other .tplg, .ldc debug data, etc.) but evict the .ri our
# kernel actually loads at boot.

do_install:append() {
    rm -f ${D}${nonarch_base_libdir}/firmware/imx/sof-zephyr-gcc/sof-imx8m.ri
    rm -f ${D}${nonarch_base_libdir}/firmware/imx/sof-zephyr-xcc/sof-imx8m.ri
    rm -f ${D}${nonarch_base_libdir}/firmware/imx/sof/sof-imx8m.ri || true
}
