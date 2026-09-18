#!/usr/bin/env bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "${SCRIPT_DIR}/../lib/log.sh" ]; then
    # shellcheck source=../lib/log.sh
    source "${SCRIPT_DIR}/../lib/log.sh"
fi

command -v log_info >/dev/null 2>&1 || log_info() { echo "INFO: $*"; }
command -v log_err >/dev/null 2>&1 || log_err() { echo "ERROR: $*" >&2; }

KDIR="$(readlink -f "$SCRIPT_DIR/../..")"
cd "$KDIR"

if [ -d /workspace ]; then
    WP="/workspace"
    export CCACHE_DIR="$WP/.ccache"
fi

export WP=${WP:-$(realpath "$KDIR/../")}

if [ ! -d drivers ]; then
    log_err "Please execute from top-level kernel tree"
    exit 1
fi

export PATH="$KDIR/build/bin:$PATH"

SCRIPTS_DIR="build/scripts"
source "$SCRIPTS_DIR/tc.sh"

: "${SKIP_FIPS_CRYPTO_INTEGRITY:=1}"
: "${SKIP_EXYNOS_FMP_INTEGRITY:=1}"
export SKIP_FIPS_CRYPTO_INTEGRITY
export SKIP_EXYNOS_FMP_INTEGRITY

USE_CCACHE="${USE_CCACHE:-1}"
if [ "$USE_CCACHE" = "1" ]; then
    export CC="ccache clang"
else
    export CC="clang"
fi

export PLATFORM_VERSION="${PLATFORM_VERSION:-12}"
export ANDROID_MAJOR_VERSION="${ANDROID_MAJOR_VERSION:-s}"
export TARGET_SOC="${TARGET_SOC:-universal2100}"
export LLVM=1
export LLVM_IAS=1
export ARCH=arm64
: "${USE_THINLTO_CACHE:=0}"
export USE_THINLTO_CACHE
LINKER="${LINKER:-ld.lld}"

DEFCONFIG="${DEFCONFIG:-exynos2100-unified_defconfig}"
FK_VER="$(grep -oP '^FK_VER="\K[^"]+' build/ckbuild.sh | head -n 1)"
[ -n "$FK_VER" ] || FK_VER="v1.1.2"
OUTDIR="$KDIR/out"
NH_MOD_OUTDIR="$KDIR/build/tmp/nh_modules_out"

if [ ! -f "$OUTDIR/.config" ]; then
    log_err "No existing build tree at $OUTDIR, run a full build first"
    exit 1
fi

if [ ! -f "arch/arm64/configs/nethunter.config" ]; then
    log_err "nethunter.config not found"
    exit 1
fi

LOCALVERSION_SAVED="$(sed -n 's/^CONFIG_LOCALVERSION=\(.*\)/\1/p' "$OUTDIR/.config" | head -n 1)"
LOCALVERSION_SAVED="${LOCALVERSION_SAVED%\"}"
LOCALVERSION_SAVED="${LOCALVERSION_SAVED#\"}"

: "${DROIDSPACES:=1}"
NH_FRAGMENTS="nethunter.config"
[ "$DROIDSPACES" = "1" ] && NH_FRAGMENTS="$NH_FRAGMENTS droidspaces.config"

log_info "Merging nethunter.config onto existing build tree..."
make -j"$(nproc --all)" O="$OUTDIR" CC="$CC" "$DEFCONFIG" $NH_FRAGMENTS

if [ -n "$LOCALVERSION_SAVED" ]; then
    scripts/config --file "$OUTDIR/.config" --set-str LOCALVERSION "$LOCALVERSION_SAVED"
fi

log_info "Building NetHunter modules..."
make -j"$(nproc --all)" O="$OUTDIR" CC="$CC" LD="$LINKER" \
    LLVM=1 LLVM_IAS=1 ARCH=arm64 USE_THINLTO_CACHE="$USE_THINLTO_CACHE" \
    CROSS_COMPILE="${CCARM64_PREFIX:-aarch64-linux-gnu-}" \
    CROSS_COMPILE_ARM32="${CCARM32_PREFIX:-arm-linux-gnueabi-}" modules

rm -rf "$NH_MOD_OUTDIR"
make -j"$(nproc --all)" O="$OUTDIR" CC="$CC" LD="$LINKER" \
    LLVM=1 LLVM_IAS=1 ARCH=arm64 USE_THINLTO_CACHE="$USE_THINLTO_CACHE" \
    CROSS_COMPILE="${CCARM64_PREFIX:-aarch64-linux-gnu-}" \
    CROSS_COMPILE_ARM32="${CCARM32_PREFIX:-arm-linux-gnueabi-}" \
    INSTALL_MOD_STRIP="--strip-debug --keep-section=.ARM.attributes" \
    INSTALL_MOD_PATH="$NH_MOD_OUTDIR" modules_install

KMOD_DIR="$(find "$NH_MOD_OUTDIR/lib/modules" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
if [ -z "$KMOD_DIR" ]; then
    log_err "No installed modules found in $NH_MOD_OUTDIR/lib/modules"
    exit 1
fi

bash "$SCRIPTS_DIR/gen_nh_module.sh" \
    "$KMOD_DIR" \
    "$KDIR" \
    "$FK_VER" \
    "$KDIR/build/nh/firmware"

rm -rf "$NH_MOD_OUTDIR"

log_info "NetHunter extras build complete"
