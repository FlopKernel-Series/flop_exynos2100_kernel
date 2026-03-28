#!/usr/bin/env bash

if [ -f /etc/doas.conf ] && command -v "doas" &>/dev/null; then
	  ROOT="doas"
elif command -v "sudo" &>/dev/null; then
	  ROOT="sudo"
else
	  log_err "neither doas nor sudo found."
	  return 1
fi

AK3_MANAGED=0

if [ "$DO_ZIP" = "1" ]; then
    rm -rf "$AK3_DIR"
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
fi

export AK3_MANAGED

DEPS=( lz4 brotli flex bc cpio kmod zip binutils-aarch64-linux-gnu ccache )

UBUNTU() {
    local DEPS=( lz4 brotli flex bc cpio kmod zip ccache binutils-aarch64-linux-gnu )
    local MISSING=()

    # Check command presence
    for d in "${DEPS[@]}"; do
        if ! command -v "$d" >/dev/null 2>&1; then
            MISSING+=("$d")
        fi
    done

    if [ ${#MISSING[@]} -gt 0 ]; then
        $ROOT apt-get update -qq || true
        $ROOT apt-get install -y "${MISSING[@]}"
    fi
}

ARCH(){
    local DEPS=( lz4 brotli flex bc cpio kmod zip aarch64-linux-gnu-binutils ccache )
    local MISSING=$(pacman -T "${DEPS[@]}" 2>/dev/null)

    if [ -n "$MISSING" ]; then
		    $ROOT pacman -Syyuu --needed --noconfirm $MISSING
	  fi
}

GENTOO() {
    local DEPS=( lz4 brotli flex bc cpio kmod ccache zip )
    local PKGS=( app-arch/lz4 app-arch/brotli sys-devel/flex sys-devel/bc app-arch/cpio sys-apps/kmod dev-util/ccache app-arch/zip )
    local MISSING=()

    for i in "${!DEPS[@]}"; do
        if ! command -v "${DEPS[i]}" >/dev/null 2>&1; then
            MISSING+=("${PKGS[i]}")
        fi
    done

    if [ ${#MISSING[@]} -gt 0 ]; then
        $ROOT emerge -nvq "${MISSING[@]}"
    fi
}

source "/etc/os-release"
DISTRO_IDS="$ID $ID_LIKE"

if echo "$DISTRO_IDS" | grep -Eq 'ubuntu|debian'; then
    UBUNTU
elif echo "$DISTRO_IDS" | grep -Eq 'arch'; then
    ARCH
elif echo "$DISTRO_IDS" | grep -Eq 'gentoo'; then
    GENTOO
else
    echo ""
    log_info "distro not supported, install manually: ${DEPS[*]}"
    echo ""
fi
