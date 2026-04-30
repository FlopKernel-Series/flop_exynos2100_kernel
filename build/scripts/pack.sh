packing() {
    if [ "$DO_ZIP" = "1" ]; then
        echo -e "\n$(log_info "Building AnyKernel ZIP...")"

        if [ ! -d "$AK3_DIR/.git" ]; then
            log_warn "AnyKernel3 directory not found at $AK3_DIR, skipping ZIP packaging"
        else

            cd "$AK3_DIR"

            rm -f boot.img dtb dtb.img Image* zImage*

            cp -f "$OUT_VENDORBOOTIMG" vendor_boot.img
            cp -f "$OUT_KERNEL" .
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

        # OneUI TAR
        echo -e "$(log_info "Creating OneUI TAR...")"
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
