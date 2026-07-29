# shellcheck shell=bash
#
# Build stage for DTBO images
#

build_dtbo_images() {
    local codenames=("r9s" "o1s" "p3s" "t2s")
    local c

    echo

    for c in "${codenames[@]}"; do
        local dtbo_dir="$OUTDIR/arch/arm64/boot/dts/exynos/samsung/$c"
        local cfg_file="$KDIR/build/dtconfigs/${c}.cfg"
        local out_img="$IMAGES_DIR/dtbo_${c}.img"

        if [ ! -d "$dtbo_dir" ]; then
            echo -e "\n$(log_err "DTBO output directory for $c not found at $dtbo_dir!")\n"
            exit 1
        fi

        if [ ! -f "$cfg_file" ]; then
            echo -e "\n$(log_err "DTBO config file for $c not found at $cfg_file!")\n"
            exit 1
        fi

        echo -e "$(log_info "Building DTBO image for $c -> dtbo_${c}.img...")"
        python3 "$MKDTBOIMG" cfg_create "$out_img" "$cfg_file" -d "$dtbo_dir" || {
            echo -e "\n$(log_err "Failed to build DTBO image for $c!")\n"
            exit 1
        }
    done
}
