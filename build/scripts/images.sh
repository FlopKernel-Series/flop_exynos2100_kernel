resolve_dtb_input() {
    if [ -f "$IN_DTB" ]; then
        printf '%s\n' "$IN_DTB"
        return 0
    fi

    local dtb
    dtb=$(find "$OUTDIR/arch/arm64/boot/dts" \
        \( -name 'exynos2100*.dtb' -o -name 'r9s*.dtb' \) \
        -type f | sort | head -n 1)

    if [ -n "$dtb" ]; then
        printf '%s\n' "$dtb"
        return 0
    fi

    return 1
}

build_images() {
    local DTB_INPUT
    local MONTH

    MONTH="$(date +%Y-%m)"
    DTB_INPUT="$(resolve_dtb_input)" || {
        log_err "Unable to locate a built DTB for Exynos 2100"
        exit 1
    }

    local BOOT_OS_VERSION="${BOOT_OS_VERSION:-12.0.0}"
    local BOOT_OS_PATCH_LEVEL="${BOOT_OS_PATCH_LEVEL:-$MONTH}"
    local BOOT_HEADER_VERSION="${BOOT_HEADER_VERSION:-3}"
    local BOOT_PAGESIZE="${BOOT_PAGESIZE:-4096}"
    local VENDOR_CMDLINE="${VENDOR_CMDLINE:-loop.max_part=7}"
    local VENDOR_BOARD="${VENDOR_BOARD:-SRPUG16A011KU}"
    local VENDOR_PAGESIZE="${VENDOR_PAGESIZE:-2048}"
    local DTB_OFFSET="${DTB_OFFSET:-0x101f00000}"
    local KERNEL_OFFSET="${KERNEL_OFFSET:-0x00008000}"
    local RAMDISK_OFFSET="${RAMDISK_OFFSET:-0x01000000}"
    local TAGS_OFFSET="${TAGS_OFFSET:-0x00000100}"
    local BASE_ADDR="${BASE_ADDR:-0x10000000}"

    echo -e "\n$(log_info "Building dtb image...")"
    python3 "$MKDTBOIMG" create "$OUT_DTBIMAGE" --custom0=0x00000000 --custom1=0xff000000 --version=0 --page_size=2048 "$DTB_INPUT" || exit 1

    echo -e "\n$(log_info "Building boot image...")"
    python3 "$MKBOOTIMG" --header_version "$BOOT_HEADER_VERSION" \
        --kernel "$OUT_KERNEL" \
        --output "$OUT_BOOTIMG" \
        --ramdisk "$PREBUILT_RAMDISK" \
        --pagesize "$BOOT_PAGESIZE" \
        --os_version "$BOOT_OS_VERSION" \
        --os_patch_level "$BOOT_OS_PATCH_LEVEL" || exit 1
    echo -e "$(log_info "boot.img created!")"

    echo -e "\n$(log_info "Building vendor_boot image...")"
    cd "$RAMDISK_DIR"
    find . | cpio --quiet -o -H newc -R root:root | gzip -9 > "../ramdisk.cpio.gz"
    cd ..

    python3 "$MKBOOTIMG" --header_version "$BOOT_HEADER_VERSION" \
        --vendor_boot "$OUT_VENDORBOOTIMG" \
        --vendor_cmdline "$VENDOR_CMDLINE" \
        --dtb "$OUT_DTBIMAGE" \
        --vendor_ramdisk "$(pwd)/ramdisk.cpio.gz" \
        --os_version "$BOOT_OS_VERSION" \
        --os_patch_level "$BOOT_OS_PATCH_LEVEL" \
        --board "$VENDOR_BOARD" \
        --dtb_offset "$DTB_OFFSET" \
        --kernel_offset "$KERNEL_OFFSET" \
        --ramdisk_offset "$RAMDISK_OFFSET" \
        --tags_offset "$TAGS_OFFSET" \
        --base "$BASE_ADDR" \
        --pagesize "$VENDOR_PAGESIZE" || exit 1

    cd "$KDIR"
    echo -e "$(log_info "Done!")"
}
