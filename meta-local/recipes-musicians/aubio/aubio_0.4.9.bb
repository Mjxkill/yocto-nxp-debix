SUMMARY = "aubio is designed for the extraction of annotations from audio signals"
HOMEPAGE = "https://aubio.org/"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=d32239bcb673463ab874e80d47fae504"

inherit waf pkgconfig

DEPENDS += " \
    libsndfile1 \
    libsamplerate0 \
"

SRC_URI = " \
    https://aubio.org/pub/${BPN}-${PV}.tar.bz2 \
    file://0001-do-not-build-tests.patch \
    file://0002-Fix-build-for-python3-only-environments.patch \
    file://0003-define_FF_API_LAVF_AVCTX_for_libavcodec_59.patch \
"
SRC_URI[sha256sum] = "d48282ae4dab83b3dc94c16cf011bcb63835c1c02b515490e1883049c3d1f3da"

EXTRA_OECONF = " \
    --prefix=${prefix} \
    --sysconfdir=${sysconfdir} \
    --libdir=${libdir} \
"

do_configure:prepend() {
    # Python 3.12: imp.new_module is gone; use types.ModuleType instead
    sed -i "s/module=imp.new_module(WSCRIPT_FILE)/from types import ModuleType\n\tmodule=ModuleType(WSCRIPT_FILE)/" ${S}/waflib/Context.py || true
    # Replace deprecated imp import with importlib alias
    sed -i "s/^import os,re,imp,sys$/import os,re,sys\nimport importlib as imp/" ${S}/waflib/Context.py || true
    # Python 3.12: strip universal newlines flag 'U' from file modes
    sed -i "s/^def readf(fname,m='r',encoding='latin-1'):/def readf(fname,m='r',encoding='latin-1'):\n\tm=m.replace('U','')/" ${S}/waflib/Utils.py || true
}
