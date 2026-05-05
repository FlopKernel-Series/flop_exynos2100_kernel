#!/usr/bin/env bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "${SCRIPT_DIR}/../lib/log.sh" ]; then
    # shellcheck source=../lib/log.sh
    source "${SCRIPT_DIR}/../lib/log.sh"
fi

command -v log_info >/dev/null 2>&1 || log_info() { echo "INFO: $*"; }
command -v log_warn >/dev/null 2>&1 || log_warn() { echo "WARNING: $*" >&2; }
command -v log_err  >/dev/null 2>&1 || log_err()  { echo "ERROR: $*" >&2; }

if [ -z "$1" ] || [ -z "$2" ] || [ -z "$3" ]; then
    log_err "Usage: $0 <modules_dir> <kernel_dir> <fk_ver> [firmware_src_dir]"
    exit 1
fi

MODULES_DIR="$1"
KERNEL_DIR="$2"
FK_VER="$3"
FIRMWARE_SRC_DIR="${4:-$KERNEL_DIR/build/nh/firmware}"
MODULES_ORDER="$MODULES_DIR/modules.order"

NETHUNTER_MODULES=(
    "ar5523.ko"
    "ath.ko"
    "ath10k_core.ko"
    "ath10k_usb.ko"
    "ath6kl_core.ko"
    "ath6kl_usb.ko"
    "ath9k_common.ko"
    "ath9k_htc.ko"
    "ath9k_hw.ko"
    "carl9170.ko"
    "mt76-usb.ko"
    "mt76.ko"
    "mt76x0-common.ko"
    "mt76x0u.ko"
    "mt76x02-lib.ko"
    "mt76x02-usb.ko"
    "mt76x2-common.ko"
    "mt76x2u.ko"
    "mt7601u.ko"
    "rt2500usb.ko"
    "rt2800lib.ko"
    "rt2800usb.ko"
    "rt2x00lib.ko"
    "rt2x00usb.ko"
    "rt73usb.ko"
    "rtl8187.ko"
    "rtl8xxxu_git.ko"
    "rtl8192c-common.ko"
    "rtl8192cu.ko"
    "rtl_usb.ko"
    "rtlwifi.ko"
    "rtw_8821c.ko"
    "rtw_8821cu.ko"
    "rtw_8822b.ko"
    "rtw_8822bu.ko"
    "rtw_8822c.ko"
    "rtw_8822cu.ko"
    "rtw_core.ko"
    "rtw_usb.ko"
    "mac80211.ko"
)

NETHUNTER_FIRMWARE_FILES=(
    "ar5523.bin"
    "carl9170-1.fw"
    "ath9k_htc/htc_9271-1.4.0.fw"
    "ath9k_htc/htc_7010-1.4.0.fw"
    "ath9k_htc/htc_9271.fw"
    "ath9k_htc/htc_7010.fw"
    "ath6k/AR6004/hw1.0/fw.ram.bin"
    "ath6k/AR6004/hw1.0/bdata.bin"
    "ath6k/AR6004/hw1.0/bdata.DB132.bin"
    "ath6k/AR6004/hw1.1/fw.ram.bin"
    "ath6k/AR6004/hw1.1/bdata.bin"
    "ath6k/AR6004/hw1.1/bdata.DB132.bin"
    "ath6k/AR6004/hw1.2/fw.ram.bin"
    "ath6k/AR6004/hw1.2/bdata.bin"
    "ath6k/AR6004/hw1.3/fw.ram.bin"
    "ath6k/AR6004/hw1.3/bdata.bin"
    "ath10k/QCA9377/hw1.0/firmware-5.bin"
    "ath10k/QCA9377/hw1.0/firmware-6.bin"
    "ath10k/QCA9377/hw1.0/board.bin"
    "ath10k/QCA9377/hw1.0/board-2.bin"
    "mt7601u.bin"
    "mediatek/mt7610e.bin"
    "mediatek/mt7610u.bin"
    "mt7662.bin"
    "mt7662_rom_patch.bin"
    "rt73.bin"
    "rt2870.bin"
    "rtlwifi/rtl8192cufw.bin"
    "rtlwifi/rtl8192cufw_A.bin"
    "rtlwifi/rtl8192cufw_B.bin"
    "rtlwifi/rtl8192cufw_TMSC.bin"
    "rtlwifi/rtl8723aufw_A.bin"
    "rtlwifi/rtl8723aufw_B.bin"
    "rtlwifi/rtl8723aufw_B_NoBT.bin"
    "rtlwifi/rtl8188eufw.bin"
    "rtlwifi/rtl8192eu_nic.bin"
    "rtlwifi/rtl8723bu_nic.bin"
    "rtlwifi/rtl8723bu_bt.bin"
    "rtlwifi/rtl8188fufw.bin"
    "rtlwifi/rtl8710bufw_SMIC.bin"
    "rtlwifi/rtl8710bufw_UMC.bin"
    "rtlwifi/rtl8192fufw.bin"
    "rtw88/rtw8821c_fw.bin"
    "rtw88/rtw8822b_fw.bin"
    "rtw88/rtw8822c_fw.bin"
    "rtw88/rtw8822c_wow_fw.bin"
)

is_nethunter_module() {
    local module="$1"
    local pattern

    for pattern in "${NETHUNTER_MODULES[@]}"; do
        if [ "$module" = "$pattern" ]; then
            return 0
        fi
    done

    return 1
}

copy_required_nethunter_firmware_from_repo() {
    local src_root="$1"
    local dst_root="$2"
    local fw_path src_path dst_path
    local missing_count=0

    firmware_count=0
    for fw_path in "${NETHUNTER_FIRMWARE_FILES[@]}"; do
        src_path="$src_root/$fw_path"
        if [ ! -f "$src_path" ]; then
            missing_count=$((missing_count + 1))
            continue
        fi

        dst_path="$dst_root/$fw_path"
        mkdir -p "$(dirname "$dst_path")"
        cp -f "$src_path" "$dst_path"
        firmware_count=$((firmware_count + 1))
    done

    if [ "$firmware_count" -gt 0 ]; then
        log_info "Included $firmware_count firmware file(s) from linux-firmware"
    else
        log_info "No required NetHunter firmware files found in $src_root"
    fi

    if [ "$missing_count" -gt 0 ]; then
        log_warn "$missing_count required firmware file(s) were not found in linux-firmware"
    fi
}

if [ ! -d "$MODULES_DIR" ]; then
    log_err "Modules directory not found: $MODULES_DIR"
    exit 1
fi

if [ ! -f "$MODULES_ORDER" ]; then
    log_err "modules.order not found at $MODULES_ORDER"
    exit 1
fi

git_hash=$(git -C "$KERNEL_DIR" rev-parse --short HEAD 2>/dev/null || true)
[ -n "$git_hash" ] || git_hash="unknown"

version_digits=$(printf '%s' "$FK_VER" | tr -cd '0-9')
[ -n "$version_digits" ] || version_digits="0"
version_code="${version_digits}000"
version_string="${FK_VER}-${git_hash}"

OUT_DIR="$KERNEL_DIR/build/nh/out"
STAGE_DIR="$OUT_DIR/floppy2100_nethunter-extras"
MODULE_DEST_DIR="$STAGE_DIR/system/lib/modules"
FIRMWARE_DEST_DIR="$STAGE_DIR/system/vendor/firmware"
ZIP_PATH="$OUT_DIR/floppy2100_nethunter-extras-${version_string}.zip"
TMP_EXCLUDED_DIR="$(mktemp -d)"

cleanup() {
    rm -rf "$TMP_EXCLUDED_DIR"
}
trap cleanup EXIT

mkdir -p "$OUT_DIR"
rm -rf "$STAGE_DIR"
mkdir -p "$MODULE_DEST_DIR"

declare -A BUILT_MODULES
declare -A EXCLUDED_MODULES

while IFS= read -r path; do
    name=$(basename "$path")
    BUILT_MODULES["$name"]="$path"
done < <(find "$MODULES_DIR" -name "*.ko" -type f)

excluded_count=0
while IFS= read -r module_path; do
    module=$(basename "$module_path")
    [ -n "$module" ] || continue

    if ! is_nethunter_module "$module"; then
        continue
    fi

    if [ -z "${BUILT_MODULES[$module]:-}" ]; then
        continue
    fi

    if [ -n "${EXCLUDED_MODULES[$module]+x}" ]; then
        continue
    fi

    cp -f "${BUILT_MODULES[$module]}" "$TMP_EXCLUDED_DIR/$module"
    EXCLUDED_MODULES["$module"]=1
    excluded_count=$((excluded_count + 1))
done < "$MODULES_ORDER"

if [ "$excluded_count" -eq 0 ]; then
    log_warn "No NetHunter modules were detected for packaging"
else
    cp -f "$TMP_EXCLUDED_DIR"/*.ko "$MODULE_DEST_DIR/"
    log_info "Collected $excluded_count NetHunter module(s)"
fi

firmware_count=0
if [ -d "$FIRMWARE_SRC_DIR" ]; then
    if [ -f "$FIRMWARE_SRC_DIR/WHENCE" ]; then
        copy_required_nethunter_firmware_from_repo "$FIRMWARE_SRC_DIR" "$FIRMWARE_DEST_DIR"
    else
        while IFS= read -r firmware_file; do
            rel_path="${firmware_file#"$FIRMWARE_SRC_DIR"/}"
            dst_path="$FIRMWARE_DEST_DIR/$rel_path"
            mkdir -p "$(dirname "$dst_path")"
            cp -f "$firmware_file" "$dst_path"
            firmware_count=$((firmware_count + 1))
        done < <(find "$FIRMWARE_SRC_DIR" -type f ! -name ".gitkeep" ! -name "README.md")

        if [ "$firmware_count" -gt 0 ]; then
            log_info "Included $firmware_count firmware file(s)"
        else
            log_info "No firmware payload found in $FIRMWARE_SRC_DIR, skipping firmware copy"
        fi
    fi
else
    log_info "Firmware source directory not present, skipping firmware copy: $FIRMWARE_SRC_DIR"
fi

cat > "$STAGE_DIR/module.prop" << EOF
id=floppy2100_nh_extras
name=Floppy2100 Nethunter Extras
version=${version_string}
versionCode=${version_code}
author=Flopster101
description=Floppy2100 NetHunter extras package with external Wi-Fi kernel modules and optional firmware payload
EOF

rm -f "$ZIP_PATH"
(
    cd "$STAGE_DIR"
    zip -r9 -q "$ZIP_PATH" .
)

log_info "Generated NetHunter module package: $ZIP_PATH"
