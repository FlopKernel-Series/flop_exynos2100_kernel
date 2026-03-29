#!/bin/bash

apply_kpm_patch() {
    if [[ "$DO_SUKI" != "1" ]]; then
        return 0
    fi

    log_info "Applying KPM patch..."

    local KPM_URL="https://raw.githubusercontent.com/ShirkNeko/SukiSU_patch/refs/heads/main/kpm/patch_linux"
    local MAGISKBOOT="$KDIR/build/bin/magiskboot"
    local WORK_DIR="$WP/kpm_work"
    local FULL_OUT_IMAGE="$OUT_KERNEL"

    if [[ ! -f "$MAGISKBOOT" ]] || [[ ! -x "$MAGISKBOOT" ]]; then
        log_err "magiskboot not found or not executable at $MAGISKBOOT"
        return 1
    fi

    mkdir -p "$WORK_DIR"
    cd "$WORK_DIR"

    if ! curl -LSs "$KPM_URL" -o patch; then
        log_err "Failed to download KPM patch script"
        cd "$KDIR"
        rm -rf "$WORK_DIR"
        return 1
    fi
    chmod +x patch

    local img_file=""
    if [[ -f "$FULL_OUT_IMAGE" ]]; then
        cp "$FULL_OUT_IMAGE" .
        img_file=$(basename "$FULL_OUT_IMAGE")
    else
        log_err "Kernel image not found at $FULL_OUT_IMAGE"
        cd "$KDIR"
        rm -rf "$WORK_DIR"
        return 1
    fi

    if [[ "$img_file" == *"Image.gz-dtb" ]]; then
        log_info "Extracting kernel from Image.gz-dtb..."
        if ! "$MAGISKBOOT" split "$img_file" || [[ ! -f kernel ]]; then
            log_err "Failed to split $img_file"
            cd "$KDIR"
            rm -rf "$WORK_DIR"
            return 1
        fi
        cp kernel Image
    elif [[ "$img_file" == *"Image.gz" ]]; then
        log_info "Decompressing Image.gz..."
        if ! "$MAGISKBOOT" decompress "$img_file" Image 2>/dev/null && ! gunzip -c "$img_file" > Image 2>/dev/null; then
            log_err "Failed to decompress $img_file"
            cd "$KDIR"
            rm -rf "$WORK_DIR"
            return 1
        fi
    elif [[ "$img_file" == *"Image" ]]; then
        log_info "Using uncompressed Image directly"
    else
        log_err "Unsupported kernel image format: $img_file"
        cd "$KDIR"
        rm -rf "$WORK_DIR"
        return 1
    fi

    if [[ ! -f Image ]]; then
        log_err "Image file not found after extraction"
        cd "$KDIR"
        rm -rf "$WORK_DIR"
        return 1
    fi

    log_info "Patching kernel..."
    if ./patch 2>&1; then
        if [[ -f oImage ]]; then
            mv oImage Image
        fi
    else
        log_err "KPM patch script failed"
        cd "$KDIR"
        rm -rf "$WORK_DIR"
        return 1
    fi

    log_info "Repacking kernel image..."
    if [[ "$img_file" == *"Image.gz-dtb" ]]; then
        if ! "$MAGISKBOOT" compress=gzip Image kernel_new && ! gzip -c Image > kernel_new; then
            log_err "Failed to compress patched Image"
            cd "$KDIR"
            rm -rf "$WORK_DIR"
            return 1
        fi

        if [[ ! -f kernel_dtb ]]; then
            log_err "kernel_dtb not found, cannot recreate Image.gz-dtb"
            cd "$KDIR"
            rm -rf "$WORK_DIR"
            return 1
        fi

        cat kernel_new kernel_dtb > Image.gz-dtb
        cp Image.gz-dtb "$FULL_OUT_IMAGE"
    elif [[ "$img_file" == *"Image.gz" ]]; then
        if ! gzip -c Image > Image.gz; then
            log_err "Failed to compress patched Image"
            cd "$KDIR"
            rm -rf "$WORK_DIR"
            return 1
        fi
        cp Image.gz "$FULL_OUT_IMAGE"
    else
        cp Image "$FULL_OUT_IMAGE"
    fi

    log_info "KPM patching completed successfully"
    cd "$KDIR"
    rm -rf "$WORK_DIR"
    return 0
}
