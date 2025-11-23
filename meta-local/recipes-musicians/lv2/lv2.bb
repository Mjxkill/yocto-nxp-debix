require ${BPN}.inc

DEPENDS = "gtk+ libsndfile1"

# Meson honors standard libdir; no extra args needed

FILES:${PN} += " \
    ${datadir} \
"
