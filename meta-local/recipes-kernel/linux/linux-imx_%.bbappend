FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://0001-imx8mp-evk-add-audio-and-mipi-panel.patch"

# Allow linux-imx-src debug package to pass QA despite build path references
INSANE_SKIP:linux-imx-src += "buildpaths"
