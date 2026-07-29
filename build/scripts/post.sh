# shellcheck shell=bash
#
# Module packaging logic
#

kernel_modules() {
    local i name

    rm -rf "$RAMDISK_DIR"
    rm -f "$TMPDIR/modules.load"
    rm -f "$OUT_BOOTIMG_ONEUI" "$OUT_BOOTIMG_AOSP" "$OUT_VENDORBOOTIMG"
    mkdir -p "$TMPDIR" "$RAMDISK_DIR" "$MODULES_DIR/0.0"

    cp -a "$IN_VBOOT/." "$RAMDISK_DIR/"
    mkdir -p "$MODULES_DIR/0.0"

    if [ ! -d "$MOD_OUTDIR/lib/modules" ] || [ -z "$(find "$MOD_OUTDIR/lib/modules" -mindepth 1 -maxdepth 1 -type d 2>/dev/null)" ]; then
        echo -e "\n$(log_err "No installed modules found in $MOD_OUTDIR/lib/modules!")\n"
        exit 1
    fi

    local kmod_dir
    kmod_dir=$(find "$MOD_OUTDIR/lib/modules" -mindepth 1 -maxdepth 1 -type d | head -n 1)

    log_info "Generating modules.load..."
    "$SCRIPTS_DIR/gen_modules_load.sh" "$kmod_dir" "$TMPDIR/modules.load" "$KDIR"

    if [ ! -f "$TMPDIR/modules.load" ]; then
         log_err "Failed to generate modules.load"
         exit 1
    fi

    declare -A MODULE_MAP
    while IFS= read -r path; do
        name=$(basename "$path")
        MODULE_MAP["$name"]="$path"
    done < <(find "$MOD_OUTDIR/lib/modules" -name "*.ko" -type f)

    for module in $(cat "$TMPDIR/modules.load"); do
        local src="${MODULE_MAP[$module]}"
        if [ -n "$src" ] && [ -f "$src" ]; then
            cp -f "$src" "$MODULES_DIR/0.0/$module"
        fi
    done

    depmod 0.0 -b "$RAMDISK_DIR"
    sed -i 's/\([^ ]\+\)/\/lib\/modules\/\1/g' "$MODULES_DIR/0.0/modules.dep"
    cd "$MODULES_DIR/0.0" || return 1
    while IFS= read -r i; do
        if [[ "$(basename "$i")" != "modules.dep" && "$(basename "$i")" != "modules.softdep" && "$(basename "$i")" != "modules.alias" ]]; then
            rm -f "$i"
        fi
    done < <(find . -name "modules.*" -type f)

    cd "$KDIR" || return 1

    cp -f "$TMPDIR/modules.load" "$MODULES_DIR/0.0/modules.load"
    mv "$MODULES_DIR/0.0"/* "$MODULES_DIR/"
    rm -rf "$MODULES_DIR/0.0"

    if [ "${DO_NHMOD:-0}" = "1" ]; then
        echo
        log_info "Packaging Nethunter Extras zip..."
        if ! bash "$SCRIPTS_DIR/gen_nh_module.sh" \
            "$kmod_dir" \
            "$KDIR" \
            "$FK_VER" \
            "$KDIR/build/nh/firmware"; then
            log_warn "Failed to generate NetHunter module package, continuing build"
        else
            # shellcheck disable=SC2034
            NH_MODULE_PATH=$(ls -t "$KDIR"/build/nh/out/floppy2100_nethunter-extras-*.zip 2>/dev/null | head -n 1)
        fi
    fi
}

clean_tmp() {
    log_info "Cleaning after build..."
    rm -rf "$TMPDIR" "$MOD_OUTDIR"
    echo
}
