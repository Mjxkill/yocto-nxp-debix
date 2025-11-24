do_configure:prepend() {
    mkdir -p ${B}
    if [ -f "${STAGING_KERNEL_BUILDDIR}/Module.symvers" ]; then
        install -m 0644 ${STAGING_KERNEL_BUILDDIR}/Module.symvers ${B}/
    fi
}

MODULES_MODULE_SYMVERS_LOCATION = "."
