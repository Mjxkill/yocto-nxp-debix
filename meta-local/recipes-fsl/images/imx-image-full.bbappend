IMAGE_INSTALL:append = " git"

# Audio toolchain
IMAGE_INSTALL:append = " sox alsa-plugins alsa-tools"

# DSP firmware/tools
IMAGE_INSTALL:append = " imx-dsp imx-dsp-codec-ext imx-dspc-asrc speexdsp ladspa-sdk spandsp"

# SOF (Sound Open Firmware) for HiFi4 DSP audio processing
IMAGE_INSTALL:append = " sof-zephyr sof-tools"

# TAC5212 boot-time init (tac-reset script + systemd service)
IMAGE_INSTALL:append = " tac5212-service"

# Python audio/DSP
IMAGE_INSTALL:append = " python3-numpy python3-pyaudio"

# ML / NPU stack (TIM-VX, TFLite VX delegate, nnstreamer, etc.)
IMAGE_INSTALL:append = " packagegroup-imx-ml"
