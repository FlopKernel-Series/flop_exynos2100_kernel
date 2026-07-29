#!/bin/bash
#
# Build script for FlopKernel (Exynos 2100).
# Based on build script for Quicksilver, by Ghostrider.
# Copyright (C) 2020-2021 Adithya R. (original version)
# Copyright (C) 2022-2025 Flopster101 (rewrite)
#
# Additional credits:
# * Gabriel2392: Logic previously used for module packaging.
# * ExtremeXT: Logic for generating modules.load on the fly.
#

source "$(pwd)/build/lib/log.sh"

DEFAULT_DEFCONFIG="${DEFAULT_DEFCONFIG:-exynos2100-unified_defconfig}"
KERNEL_URL="${KERNEL_URL:-https://github.com/FlopKernel-Series/flop_exynos2100_kernel}"
AK3_URL="${AK3_URL:-https://github.com/FlopKernel-Series/AnyKernel3-exynos2100}"
SECONDS=0
DATE="$(date '+%Y%m%d-%H%M')"
BUILD_HOST="$USER@$(hostname)"
SCRIPTS_DIR="build/scripts"
WP="${WP:-$(realpath "$PWD/../")}"

if [ -z "$WP" ]; then
    echo -e "\n$(log_err "Please set the WP env var.")\n"
    exit 1
fi

if [ ! -d drivers ]; then
    echo -e "\n$(log_err "Please execute from top-level kernel tree")\n"
    exit 1
fi

export KBUILD_BUILD_TIMESTAMP="$(LC_ALL=C date)"
export PATH="$(pwd)/build/bin:$PATH"

AK3_BRANCH="${AK3_BRANCH:-floppy-unity}"
KDIR="$(readlink -f .)"
USE_GCC_BINUTILS="0"
LINKER="${LINKER:-ld.lld}"

OUTDIR="$KDIR/out"
MOD_OUTDIR="$KDIR/modules_out"
TMPDIR="$KDIR/build/tmp"
AK3_DIR="${AK3_DIR:-$TMPDIR/AK3-2100}"
IN_VBOOT="$KDIR/build/vboot"
IN_DTB="${IN_DTB:-$OUTDIR/arch/arm64/boot/dts/exynos/exynos2100.dtb}"
RAMDISK_DIR="$TMPDIR/vendor_ramdisk"
PREBUILT_RAMDISK="$KDIR/build/boot/ramdisk"
MODULES_DIR="$RAMDISK_DIR/lib/modules"
OUT_KERNEL="$OUTDIR/arch/arm64/boot/Image"
IMAGES_DIR="$KDIR/build/images"
OUT_BOOTIMG_ONEUI="$IMAGES_DIR/boot_oneui.img"
OUT_BOOTIMG_AOSP="$IMAGES_DIR/boot_aosp.img"
OUT_VENDORBOOTIMG="$IMAGES_DIR/vendor_boot.img"
OUT_DTBIMAGE="$TMPDIR/dtb.img"

MKBOOTIMG="$(pwd)/build/mkbootimg/mkbootimg.py"
MKDTBOIMG="$(pwd)/build/dtb/mkdtboimg.py"

FK_VER="v1.1.2"
USE_CCACHE="${USE_CCACHE:-1}"
DO_TAR="${DO_TAR:-1}"
DO_ZIP="${DO_ZIP:-1}"

DEVICE="${DEVICE:-Exynos 2100 Family}"
CODENAME="${CODENAME:-exynos2100}"

if [ -f "../chat_ci" ] && [ -f "../bot_token" ]; then
    TELEGRAM_CHAT_ID="$(<../chat_ci)"
    TELEGRAM_BOT_TOKEN="$(<../bot_token)"
    SECRETS="1"
else
    SECRETS="0"
fi

# Droidspaces support
DROIDSPACES=1

# Other variables
DO_KSU=0
DO_SUKI=0
DO_XXKSU=0
DO_CLEAN=0
DO_MENUCONFIG=0
IS_RELEASE=0
DO_TG=0
DO_BASHUP=0
DO_REGEN=0
DO_FLTO=0
DO_QUIET=0
DO_NHMOD=0
DO_PERM=0
DEFCONFIG=$DEFAULT_DEFCONFIG

for arg in "$@"; do
    if [[ "$arg" == *m* ]]; then
        log_info "menuconfig argument passed, kernel configuration menu will be shown"
        DO_MENUCONFIG=1
    fi
    if [[ "$arg" == *k* ]]; then
        log_info "KernelSU argument passed, a KernelSU build will be made"
        DO_KSU=1
    fi
    if [[ "$arg" == *s* ]]; then
        log_info "ReSukiSU argument passed, a ReSukiSU build will be made"
        DO_SUKI=1
    fi
    if [[ "$arg" == *x* ]]; then
        log_info "XXKSU argument passed, an XXKSU build will be made"
        DO_XXKSU=1
    fi
    if [[ "$arg" == *c* ]]; then
        log_info "clean argument passed, output directory will be wiped"
        DO_CLEAN=1
    fi
    if [[ "$arg" == *R* ]]; then
        log_info "Release argument passed, build marked as release"
        IS_RELEASE=1
    fi
    if [[ "$arg" == *t* ]]; then
        if [ "$SECRETS" = "0" ]; then
            log_warn "Telegram argument was passed, but secrets were not found. Skipping Telegram Upload"
        else
            log_info "Telegram argument passed, build will be uploaded to CI"
            DO_TG=1
        fi
    fi
    if [[ "$arg" == *b* ]]; then
        log_info "bashupload.com argument passed, build will be uploaded to bashupload.com"
        DO_BASHUP=1
    fi
    if [[ "$arg" == *r* ]]; then
        log_info "config regeneration mode"
        DO_REGEN=1
    fi
    if [[ "$arg" == *l* ]]; then
        log_info "Full-LTO argument passed"
        log_warn "Full-LTO is VERY resource heavy and may take a long time to compile"
        DO_FLTO=1
    fi
    if [[ "$arg" == *q* ]]; then
        log_info "Quiet argument passed"
        log_warn "Only errors and warnings will be shown"
        DO_QUIET=1
    fi
    if [[ "$arg" == *n* ]]; then
        log_info "NetHunter module package argument passed"
        DO_NHMOD=1
    fi
    if [[ "$arg" == *p* ]]; then
        log_warn "Permissive packaging is not implemented in this basic Exynos 2100 build port. Ignoring 'p'."
    fi
done

KSU_COUNT=0
[ "$DO_KSU" = "1" ] && KSU_COUNT=$((KSU_COUNT + 1))
[ "$DO_SUKI" = "1" ] && KSU_COUNT=$((KSU_COUNT + 1))
[ "$DO_XXKSU" = "1" ] && KSU_COUNT=$((KSU_COUNT + 1))

if [ "$KSU_COUNT" -gt 1 ]; then
    log_err "Multiple SU variants are mutually exclusive. Please select only one."
    exit 1
fi

if [ "$IS_RELEASE" == "1" ]; then
    BUILD_TYPE="Release"
else
    BUILD_TYPE="Testing"
fi

LINUX_VER=$(make kernelversion 2>/dev/null || echo "unknown")

if [ "$DO_KSU" == "1" ]; then
    FK_TYPE="KSUNext-SUSFS"
    FK_TYPE_SHORT="KN"
elif [ "$DO_SUKI" == "1" ]; then
    FK_TYPE="ReSukiSU-SUSFS"
    FK_TYPE_SHORT="RESKS"
elif [ "$DO_XXKSU" == "1" ]; then
    FK_TYPE="XXKSU"
    FK_TYPE_SHORT="XXK"
else
    FK_TYPE="Vanilla"
    FK_TYPE_SHORT="V"
fi

ZIP_PATH="$KDIR/build/Floppy_$FK_VER-$FK_TYPE-$CODENAME-$DATE.zip"
TAR_PATH_ONEUI="$KDIR/build/FloppyOneUI_$FK_VER-$FK_TYPE-$CODENAME-$DATE.tar"
TAR_PATH_AOSP="$KDIR/build/FloppyAOSP_$FK_VER-$FK_TYPE-$CODENAME-$DATE.tar"
PACKAGE_PATH=""
NH_MODULE_PATH=""
export NH_MODULE_PATH

echo -e "\n$(log_info "Build info:")
- Device: $DEVICE ($CODENAME)
- Addons: $FK_TYPE
- FloppyKernel version: $FK_VER
- Linux version: $LINUX_VER
- Defconfig: $DEFCONFIG
- Build date: $DATE
- Build type: $BUILD_TYPE
- Clean build: $([ "$DO_CLEAN" -eq 1 ] && echo "Yes" || echo "No")
- Package ZIP: $([ "$DO_ZIP" -eq 1 ] && echo "Yes" || echo "No")
- Package TAR: $([ "$DO_TAR" -eq 1 ] && echo "Yes" || echo "No")
- Package NetHunter module: $([ "$DO_NHMOD" -eq 1 ] && echo "Yes" || echo "No")
"

[ -d "$IMAGES_DIR" ] && rm -rf "$IMAGES_DIR" || true
mkdir -p "$IMAGES_DIR"

source "$SCRIPTS_DIR/tc.sh"
source "$SCRIPTS_DIR/build.sh"
source "$SCRIPTS_DIR/post.sh"
source "$SCRIPTS_DIR/kpm.sh"
source "$SCRIPTS_DIR/images.sh"
source "$SCRIPTS_DIR/pack.sh"
source "$SCRIPTS_DIR/upload.sh"

cleanup() {
    if [ "${AK3_MANAGED:-0}" = "1" ]; then
        rm -rf "$AK3_DIR"
    fi
}

trap cleanup EXIT

prep_build() {
    if [ "$USE_CCACHE" == "1" ]; then
        log_info "Using ccache"
    fi

    echo -e "$(log_info "Compiler: $KBUILD_COMPILER_STRING")\n"
}

clean() {
    make O="$OUTDIR" clean >/dev/null 2>&1 || true
    make O="$OUTDIR" mrproper >/dev/null 2>&1 || true
    rm -rf "$MOD_OUTDIR" "$TMPDIR"
}

if [ "$DO_CLEAN" = "1" ]; then
    clean
fi

source "$SCRIPTS_DIR/deps.sh"

prep_build
build

if [ ! -f "$OUT_KERNEL" ]; then
    echo -e "\n$(log_err "Kernel files not found! Compilation failed?")"
    exit 1
fi

# apply_kpm_patch
kernel_modules
build_images
prepare_ak3
packing
echo -e "\n$(log_info "Completed in $((SECONDS / 60)) minute(s) and $((SECONDS % 60)) second(s) !")\n"
clean_tmp
upload

unset AC_DIR PC_DIR LZ_DIR SL_DIR GC_DIR ZC_DIR RV_DIR CUST_DIR KBUILD_COMPILER_STRING
