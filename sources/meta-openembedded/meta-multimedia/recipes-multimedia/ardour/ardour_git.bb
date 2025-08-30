SUMMARY = "Ardour Digital Audio Workstation"
DESCRIPTION = "Ardour is a full-featured digital audio workstation (DAW) for recording, editing, and mixing audio and MIDI."
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://COPYING;md5=2681db49ca015cb999ecc504529a2875"

SRC_URI = "git://git.ardour.org/ardour/ardour.git;branch=master;protocol=https"
SRCREV = "${AUTOREV}"

S = "${WORKDIR}/git"

DEPENDS = "glib-2.0 atk cairo pango gtk+3 alsa-lib jack"

inherit waf pkgconfig

EXTRA_OECONF = "--with-backends=jack,alsa"
