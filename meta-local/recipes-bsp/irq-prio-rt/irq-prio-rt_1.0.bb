SUMMARY = "V9.1 — RT priority boost for audio-critical IRQ kthreads"
DESCRIPTION = "Service systemd one-shot qui passe les IRQ kthreads critiques \
audio (mailbox A53↔DSP, SDMA1/2 audio, USB DWC3 x2) de la prio default 50 \
à la prio 90 sous PREEMPT_RT. Empêche la priority inversion entre IRQ \
kthread et audio_thread (prio 80) qui causait des pics 3-5 ms occasionnels \
sur snd_pcm_readi cap_dsp. Test empirique : -82% de pics."
HOMEPAGE = "https://github.com/Mjxkill/yocto-nxp-debix"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://irq-prio-rt.sh;beginline=1;endline=1;md5=3e2b31c72181b87149ff995e7202c0e3"

SRC_URI = " \
    file://irq-prio-rt.sh \
    file://irq-prio-rt.service \
"

S = "${WORKDIR}"

inherit systemd

SYSTEMD_SERVICE:${PN} = "irq-prio-rt.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

# util-linux fournit chrt
RDEPENDS:${PN} = "util-linux-chrt procps"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/irq-prio-rt.sh ${D}${bindir}/irq-prio-rt.sh

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/irq-prio-rt.service \
        ${D}${systemd_system_unitdir}/irq-prio-rt.service
}

FILES:${PN} = " \
    ${bindir}/irq-prio-rt.sh \
    ${systemd_system_unitdir}/irq-prio-rt.service \
"

COMPATIBLE_MACHINE = "(imx8mpevk|imx8mp-debix-model-ab)"
