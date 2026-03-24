packing() {
    if [ "$DO_ZIP" = "1" ]; then
        echo -e "\n$(log_info "Building AnyKernel ZIP...")"

        if [ -d "$AK3_DIR/.git" ]; then
            if [ "${AK3_MANAGED:-0}" = "1" ]; then
                log_info "Using auto-cloned AnyKernel3 checkout at $AK3_DIR"
            else
                log_info "Using existing AnyKernel3 checkout at $AK3_DIR"
            fi
        elif [ -d "$AK3_DIR" ]; then
            log_info "Using existing AnyKernel3 directory at $AK3_DIR"
        else
            log_err "AnyKernel3 directory not found at $AK3_DIR"
            exit 1
        fi

        cd "$AK3_DIR"

        rm -f boot.img dtb dtb.img Image* zImage*

        cp -f "$OUT_VENDORBOOTIMG" vendor_boot.img
        cp -f "$OUT_KERNEL" .
        rm -f "$ZIP_PATH"
        zip -r9 -q "$ZIP_PATH" . -x .git\* .github\* README.md
        cd "$KDIR"

        PACKAGE_PATH="$ZIP_PATH"
        echo -e "$(log_info "Output: $ZIP_PATH")"

        if [ "${AK3_MANAGED:-0}" = "1" ]; then
            rm -rf "$AK3_DIR"
        fi
    fi

    if [ "$DO_TAR" = "1" ]; then
        echo -e "\n$(log_info "Building TAR package...")"
        cd "$TMPDIR"
        rm -f "$TAR_PATH"
        lz4 -c -12 -B6 --content-size "$OUT_BOOTIMG" > boot.img.lz4 2>/dev/null
        lz4 -c -12 -B6 --content-size "$OUT_VENDORBOOTIMG" > vendor_boot.img.lz4 2>/dev/null
        tar -cf "$TAR_PATH" boot.img.lz4 vendor_boot.img.lz4
        rm -f boot.img.lz4 vendor_boot.img.lz4
        cd "$KDIR"

        if [ -z "$PACKAGE_PATH" ]; then
            PACKAGE_PATH="$TAR_PATH"
        fi
        echo -e "$(log_info "Output: $TAR_PATH")\n"
    fi
}
