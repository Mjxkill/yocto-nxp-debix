IMAGE_INSTALL:append = " git"

# Audio toolchain
IMAGE_INSTALL:append = " sox alsa-plugins alsa-tools"

# DSP firmware/tools
IMAGE_INSTALL:append = " imx-dsp imx-dsp-codec-ext imx-dspc-asrc speexdsp ladspa-sdk spandsp"

# SOF (Sound Open Firmware) for HiFi4 DSP audio processing
IMAGE_INSTALL:append = " sof-zephyr sof-tools"

# V3.2.2 NPU audio tap — kernel module exposing /dev/imx-audio-tap (mmap shared mem)
IMAGE_INSTALL:append = " kernel-module-imx-audio-tap"

# TAC5212 boot-time init (tac-reset script + systemd service)
IMAGE_INSTALL:append = " tac5212-service"

# V7.0-E6.a — USB UAC2 8x8 gadget (expose board as USB sound card)
IMAGE_INSTALL:append = " usb-uac2-gadget"

# V7.0-E6.b — Phone simulated 2x2 PCM via snd-aloop (placeholder VoIP)
IMAGE_INSTALL:append = " snd-aloop-phone"

# V7.0-E6.c — ALSA route bridge (matrice N×M déclarative, disabled by default)
IMAGE_INSTALL:append = " alsa-route-bridge"

# V7.0-E6.d — mixer-pro daemon console DAW SW (26 in / 4 bus FX / 18 out)
IMAGE_INSTALL:append = " mixer-pro"

# V7.0-E7 — mixer-gui-http : GUI HTTP (libmicrohttpd + Alpine.js/Tailwind CDN)
IMAGE_INSTALL:append = " mixer-gui-http"

# Python audio/DSP
IMAGE_INSTALL:append = " python3-numpy python3-pyaudio"

# ML / NPU stack (TIM-VX, TFLite VX delegate, nnstreamer, etc.)
IMAGE_INSTALL:append = " packagegroup-imx-ml"
