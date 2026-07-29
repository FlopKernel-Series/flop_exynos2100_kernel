prepare_ak3() {
    AK3_MANAGED=0

    if [ "$DO_ZIP" != "1" ]; then
        export AK3_MANAGED
        return
    fi

    if [ -d "$AK3_DIR/.git" ]; then
        export AK3_MANAGED
        return
    fi

    if [ -e "$AK3_DIR" ]; then
        log_warn "AnyKernel3 directory exists at $AK3_DIR but is not a git checkout, skipping ZIP packaging"
        DO_ZIP=0
        export AK3_MANAGED
        return
    fi

    mkdir -p "$(dirname "$AK3_DIR")"

    ak3_clone_ok=0
    for ak3_try in 1 2 3 4 5; do
        if git clone -q -b "$AK3_BRANCH" --depth=1 "$AK3_URL" "$AK3_DIR"; then
            ak3_clone_ok=1
            AK3_MANAGED=1
            break
        fi

        rm -rf "$AK3_DIR"
        if [ "$ak3_try" -lt 5 ]; then
            log_warn "AnyKernel3 clone failed (attempt $ak3_try/5), retrying in 1 second..."
            sleep 1
        fi
    done

    if [ "$ak3_clone_ok" != "1" ]; then
        log_warn "Failed to fetch AnyKernel3 after 5 attempts, ZIP packaging will be skipped"
        DO_ZIP=0
    fi

    export AK3_MANAGED
}

packing() {
    if [ "$DO_ZIP" = "1" ]; then
        echo -e "\n$(log_info "Building AnyKernel ZIP...")"

        if [ ! -d "$AK3_DIR/.git" ]; then
            log_warn "AnyKernel3 directory not found at $AK3_DIR, skipping ZIP packaging"
        else

            cd "$AK3_DIR"

            rm -f boot.img dtb dtb.img Image* zImage* dtbo*.img

            cp -f "$OUT_VENDORBOOTIMG" vendor_boot.img
            cp -f "$OUT_KERNEL" .
            cp -f "$IMAGES_DIR"/dtbo_*.img . 2>/dev/null || true
            rm -f "$ZIP_PATH"
            zip -r9 -q "$ZIP_PATH" . -x .git\* .github\* README.md
            cd "$KDIR"

            PACKAGE_PATH="$ZIP_PATH"
            echo -e "$(log_info "Output: $ZIP_PATH")"
        fi
    fi

    if [ "$DO_TAR" = "1" ]; then
        echo -e "\n$(log_info "Building TAR packages...")"
        cd "$TMPDIR"

        # Individual DTBO TARs
        local COMMIT_HASH
        COMMIT_HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
        local codenames=("r9s" "o1s" "p3s" "t2s")
        local c
        for c in "${codenames[@]}"; do
            local dtbo_img="$IMAGES_DIR/dtbo_${c}.img"
            if [ -f "$dtbo_img" ]; then
                local dtbo_tar="$KDIR/build/Floppy-DTBO-${c}-${COMMIT_HASH}-${DATE}.tar"
                rm -f "$dtbo_tar"
                lz4 -c -12 -B6 --content-size "$dtbo_img" > dtbo.img.lz4 2>/dev/null
                tar -cf "$dtbo_tar" dtbo.img.lz4
                rm -f dtbo.img.lz4
                echo -e "$(log_info "Created DTBO TAR for ${c}: ${dtbo_tar}")"
            fi
        done

        # OneUI TAR
        echo -e "\n$(log_info "Creating OneUI TAR...")"
        rm -f "$TAR_PATH_ONEUI"
        lz4 -c -12 -B6 --content-size "$OUT_BOOTIMG_ONEUI" > boot.img.lz4 2>/dev/null
        lz4 -c -12 -B6 --content-size "$OUT_VENDORBOOTIMG" > vendor_boot.img.lz4 2>/dev/null
        tar -cf "$TAR_PATH_ONEUI" boot.img.lz4 vendor_boot.img.lz4
        rm -f boot.img.lz4 vendor_boot.img.lz4
        echo -e "$(log_info "Output: $TAR_PATH_ONEUI")"

        # AOSP TAR
        echo -e "$(log_info "Creating AOSP TAR...")"
        rm -f "$TAR_PATH_AOSP"
        lz4 -c -12 -B6 --content-size "$OUT_BOOTIMG_AOSP" > boot.img.lz4 2>/dev/null
        lz4 -c -12 -B6 --content-size "$OUT_VENDORBOOTIMG" > vendor_boot.img.lz4 2>/dev/null
        tar -cf "$TAR_PATH_AOSP" boot.img.lz4 vendor_boot.img.lz4
        rm -f boot.img.lz4 vendor_boot.img.lz4
        echo -e "$(log_info "Output: $TAR_PATH_AOSP")\n"

        cd "$KDIR"

        if [ -z "$PACKAGE_PATH" ]; then
            PACKAGE_PATH="$TAR_PATH_ONEUI"
        fi
    fi
}
