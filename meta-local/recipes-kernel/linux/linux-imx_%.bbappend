FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Apply board audio/MIPI updates and enable SPDIF DIR/DIT support
SRC_URI += "file://0001-imx8mp-evk-audio-mipi.patch"
SRC_URI += "file://spdif.cfg"

# Allow linux-imx-src debug package to pass QA despite build path references
INSANE_SKIP:linux-imx-src += "buildpaths"
