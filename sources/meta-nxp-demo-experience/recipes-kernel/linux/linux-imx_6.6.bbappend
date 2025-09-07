# Apply local f_uac2.diff (at repo root) onto the kernel source tree

apply_f_uac2_patch() {
    # Try common locations for the patch relative to the build TOPDIR
    for CAND in "${TOPDIR}/../../f_uac2.diff" "${TOPDIR}/../f_uac2.diff" "${TOPDIR}/f_uac2.diff"; do
        if [ -f "$CAND" ]; then
            PATCH_FILE="$CAND"
            break
        fi
    done

    if [ -z "${PATCH_FILE:-}" ]; then
        bbwarn "f_uac2.diff introuvable près de TOPDIR; patch noyau sauté"
        return 0
    fi

    bbnote "Tentative d'application de ${PATCH_FILE} dans ${S}"
    # Test d'applicabilité; si déjà appliqué, ne rien faire
    if patch -p1 -d "${S}" --dry-run < "${PATCH_FILE}" >/dev/null 2>&1; then
        patch -p1 -d "${S}" < "${PATCH_FILE}"
        bbnote "Patch f_uac2.diff appliqué avec succès"
    else
        bbnote "Patch f_uac2.diff déjà appliqué ou non applicable; saut"
    fi
}

do_apply_f_uac2_patch() {
    apply_f_uac2_patch
}

# Place cette tâche juste après do_patch pour que les sources soient prêtes,
# mais avant la configuration/compilation
addtask apply_f_uac2_patch after do_patch before do_configure

