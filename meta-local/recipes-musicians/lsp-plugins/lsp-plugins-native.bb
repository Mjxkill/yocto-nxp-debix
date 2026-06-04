require ${BPN}.inc

FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}-native:"

# V9.2-step5g : pkgconfig auto-injecte pkg-config-native + PKG_CONFIG_PATH.
# Order matters : pkgconfig avant native (Yocto QA "native-last").
inherit pkgconfig native

DEPENDS += " \
    php-native \
    lv2-native \
    libsndfile1-native \
    cairo-native \
    libx11-native \
"

SRC_URI+= "file://0001-Adjust-native-build.patch"

EXTRA_OEMAKE += " \
    BUILD_PLATFORM=Linux \
    PREFIX=${prefix} \
    BUILD_MODULES=lv2 \
    GL_HEADERS= \
    GL_LIBS= \
    JACK_HEADERS= \
    JACK_LIBS= \
    LV2_UI=0 \
    VST_UI=0 \
"

# V9.2-step5g : Makefile lsp-plugins fork-bomb sur make récursif > -j 1.
PARALLEL_MAKE = "-j 1"

do_compile:prepend() {
    # Patch dynamique : retire jack_genmake/vst_genmake du src/utils/Makefile
    # (déclarés sans condition par 0001-Adjust-native-build.patch, nécessitent
    # jack-native/vst-native qu'on n'a pas).
    sed -i '/UTL_VSTMAKE\|vst_genmake/d' ${S}/src/utils/Makefile
    sed -i '/UTL_JACKMAKE\|jack_genmake/d' ${S}/src/utils/Makefile
    # Patch dynamique : retire les pkg-config gl/jack dans configure.mk
    # car nos overrides via EXTRA_OEMAKE ne préviennent pas l'évaluation des
    # `$(shell pkg-config ...)` dans le Makefile (make évalue avant que les
    # cmdline override s'appliquent → log spam infini "No package 'jack'").
    sed -i '/pkg-config --cflags jack\|pkg-config --libs jack/d' ${S}/scripts/make/configure.mk
    sed -i '/pkg-config --cflags gl\|pkg-config --libs gl/d' ${S}/scripts/make/configure.mk
}

do_install() {
    install -d ${D}${bindir}
    for f in ${B}/.build/*.exe ${B}/.build/utils/*.exe ${B}/.build/src/utils/*.exe; do
        if [ -e "$f" ]; then
            install -m 755 "$f" ${D}${bindir}
        fi
    done
    if [ -z "$(ls -A ${D}${bindir} 2>/dev/null)" ]; then
        echo "ERROR: no .exe produced — check ${B}/.build/ layout"
        find ${B}/.build/ -name "*.exe" -o -name "*_genttl" -o -name "gen_php" 2>/dev/null | head
        exit 1
    fi
}

