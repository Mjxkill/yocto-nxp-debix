IMAGE_INSTALL:append = " git"

# Audio toolchain
IMAGE_INSTALL:append = " sox alsa-plugins alsa-tools"

# DSP firmware/tools
IMAGE_INSTALL:append = " imx-dsp imx-dsp-codec-ext imx-dspc-asrc speexdsp ladspa-sdk spandsp"

# SOF (Sound Open Firmware) for HiFi4 DSP audio processing
IMAGE_INSTALL:append = " sof-zephyr sof-tools"

# V7.0 — custom SOF firmware (.ri) + TAC5212 topology (.tplg) — overrides
# the vendor sof-zephyr binary which lacks TAC5212 codec support and the
# project patches (multiband_drc multi-config, NPU dual-tap, DRC D3,
# SAI TX FIFO align). See ARCHI §15 for build procedure to refresh from
# the local sof/ source tree if firmware code changes.
IMAGE_INSTALL:append = " sof-firmware-custom"

# V3.2.2 NPU audio tap — kernel module exposing /dev/imx-audio-tap (mmap shared mem)
# Note : on liste le recipe `imx-audio-tap` (meta-package créé par `inherit module`
# via KERNEL_MODULES_META_PACKAGE) qui RDEPENDS sur kernel-module-imx-audio-tap-${KV}.
# Lister directement le nom de package versionné n'est pas résolu au task-graph.
IMAGE_INSTALL:append = " imx-audio-tap"

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

# V8.33 — boot.scr U-Boot avec isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 :
# isole les cores 2 et 3 pour les threads RT du mixer-pro (CPUAffinity=2 3).
IMAGE_INSTALL:append = " boot-script-rt"

# V9.1 — bump IRQ kthreads critiques (mailbox DSP, SDMA audio, USB) à RT prio 90
# pour éliminer priority inversion sous PREEMPT_RT. Service systemd one-shot
# qui run après boot avant mixer-pro.service.
IMAGE_INSTALL:append = " irq-prio-rt"

# V9.2 — LV2 plugin host stack (mixer-pro lien lilv via DEPENDS).
# lilv + core LV2 spec runtime.
IMAGE_INSTALL:append = " lilv lv2"

# V9.2 step 5 — pack de plugins LV2 (effets uniquement, pas de synths/DAW).
# Recettes recopiées de meta-musicians (incompat Scarthgap, donc port manuel)
# vers meta-local/recipes-musicians/. Stratégie itérative : on ajoute ici au
# fur et à mesure que la recette parse + build + deploy OK sur la cible.
#
# Familles cibles (cf ARCHI §10) :
#   - mda-lv2          : MDA classics (16 effets simples + bons)
#   - dragonfly-reverb : 4 reverbs (early-ref, hall, plate, room)
#   - calf             : 40+ Calf (mb-comp, exciter, vintage delay, EQ, reverb…)
#   - lsp-plugins      : 200+ plugins LSP qualité broadcast
#   - zam-plugins      : ZamAudio mastering (EQ, comp, gate, tube, GEQ31)
#   - x42 family       : fil4 EQ, darc DRC, dpl limiter, meters LUFS, fat1, sisco
#   - noise-repellent  : denoise voix
#   - ir.lv2           : convolution reverb
#   - gxplugins.lv2    : amp/cab sims Guitarix
#   - sjaehn B-series  : choppr/slizr/sequencer/shapr/jumblr/spacr/oops…
# P1 mda-lv2 — 25+ effets MDA validés sur board V9.2 (Dynamics/Leslie/Ambience/DubDelay)
IMAGE_INSTALL:append = " mda-lv2"

# P3 x42 family — broadcast pro (fil4 EQ + darc DRC + dpl limiter + meters LUFS + fat1 autotune)
# Validés board V9.2c avec AtomPort support dans mixer-pro (commits e6a6363f + 2a8e6719 + V9.2c)
IMAGE_INSTALL:append = " fil4.lv2 darc.lv2 dpl.lv2 meters.lv2 fat1.lv2"

# V9.2-step7 lv2-ttl-generator (target + native) — utilisé par postinst de plugins
# avec UI .so (genre dragonfly) pour rafraîchir TTL si modifié.
IMAGE_INSTALL:append = " lv2-ttl-generator"

# V9.2-step5d dragonfly-reverb-lv2 — 4 reverbs (Hall + early + plate + room).
# Worker + options requis (cf effects.c V9.2d). DragonflyReverb-vst skippé.
IMAGE_INSTALL:append = " dragonfly-reverb-lv2"

# V9.2-step5e calf studio gear — 50 plugins (mb-comp, exciter, vintage delay,
# reverbs, EQ12/EQ8/EQ5, analyzer, etc.). Validés V9.2d worker support.
IMAGE_INSTALL:append = " calf"

# V9.2-step5e zam-plugins-lv2 — 15 plugins ZamAudio (mastering : EQ2, GEQ31,
# Comp, MultiComp, MaximX2, Tube, Verb, DynamicEQ, Phono).
# zam-plugins-ladspa/vst/standalone packages skippés.
IMAGE_INSTALL:append = " zam-plugins-lv2"

# V9.2-step5f sjaehn B-series — 10 plugins créatifs LV2 (sequencer/chopper/
# slizer/jumblr/spacer/shaper/oops/angr/blow/schaffl). ~3 hardRTCapable
# chargeables dans mixer-pro, les autres skippés (non hardRT).
IMAGE_INSTALL:append = " bsequencer bchoppr bslizr bjumblr bspacr bshapr boops blow bangr bschaffl"

# V9.2-step5f gxplugins.lv2 — 43 plugins Guitarix (amp/cab sims, distorsions,
# boost/overdrive, modulation, delays, reverbs). Application principale :
# chaînes guitare électrique mais utilisables aussi pour synthés/vocodage.
IMAGE_INSTALL:append = " gxplugins.lv2"

# V9.2-step5f arty-fx — 11 plugins OpenAV modular (filter, lfo, etc.)
IMAGE_INSTALL:append = " arty-fx"

# V9.2-step5g lsp-plugins-lv2 — ~120 plugins LSP broadcast pro v1.1.31.
# Mono+stéréo+LR+M/S variantes : compressor, gate, limiter, expander,
# dynaproc, crossover, EQ paramétrique 4/8/16/32 bandes, art_delay,
# comp_delay, FIR/IIR filters, oscilloscope, spectrum_analyzer, etc.
# 6 fixes Yocto Scarthgap successifs (cf TESTS_V9.2_lsp_plugins.md).
IMAGE_INSTALL:append = " lsp-plugins-lv2"

# Python audio/DSP
IMAGE_INSTALL:append = " python3-numpy python3-pyaudio"

# ML / NPU stack (TIM-VX, TFLite VX delegate, nnstreamer, etc.)
IMAGE_INSTALL:append = " packagegroup-imx-ml"

# V10-P4b — Kiosk console sur écran DSI (GO valide, fiche TESTS_V10_P4a).
# PRÉREQUIS bblayers.conf (fichier de build NON versionné) :
#   BBLAYERS += "${BSPDIR}/sources/meta-browser/meta-chromium"
# (couche clonée dans sources/meta-browser, branche compatible scarthgap)
# chromium-ozone-wayland tire ses RDEPENDS (libcxx, nspr, nss, upower).
IMAGE_INSTALL:append = " chromium-ozone-wayland mixer-kiosk"

# V10-N4.4 — calibration tactile GT911 (générée par l'écran 5 mires)
IMAGE_INSTALL:append = " goodix-calibration"

# V10-NATIVE — console écran native Qt6/eglfs (remplace le kiosk chromium
# au boot ; chromium reste installé en fallback debug pour l'instant)
IMAGE_INSTALL:append = " mixer-console"
