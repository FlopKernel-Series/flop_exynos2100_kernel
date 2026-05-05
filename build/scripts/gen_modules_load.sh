#!/usr/bin/env bash
# Generate modules.load with proper dependency ordering using depmod

set -e

# Use shared log helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "${SCRIPT_DIR}/../lib/log.sh" ]; then
    # shellcheck source=../lib/log.sh
    source "${SCRIPT_DIR}/../lib/log.sh"
fi

# Fallbacks if log helpers aren't available
command -v log_info >/dev/null 2>&1 || log_info() { echo "INFO: $*"; }
command -v log_warn >/dev/null 2>&1 || log_warn() { echo "WARNING: $*" >&2; }
command -v log_err  >/dev/null 2>&1 || log_err()  { echo "ERROR: $*" >&2; }

if [ -z "$1" ] || [ -z "$2" ]; then
    log_err "Usage: $0 <modules_dir> <output_file> [kernel_dir] [exclude_list_file]"
    exit 1
fi

MODULES_DIR="$1"
OUTPUT_FILE="$2"
KERNEL_DIR="${3:-.}"
EXCLUDE_LIST_FILE="${4:-}"
MODULES_ORDER="$MODULES_DIR/modules.order"

if [ ! -f "$MODULES_ORDER" ]; then
    log_err "modules.order not found at $MODULES_ORDER"
    exit 1
fi

KERNEL_VERSION=$(basename "$MODULES_DIR")

# Priority modules that must load early
PRIORITY_MODULES=(
    "exynos-chipid_v2.ko"
    "exynos-reboot.ko"
    "clk_exynos.ko"
    "exynos_mct.ko"
    "s3c2410_wdt.ko"
    "i2c-exynos5.ko"
    "acpm-mfd-bus.ko"
    "s2mps24_mfd.ko"
    "s2mps23_mfd.ko"
    "pmic_class.ko"
    "s2mps23-regulator.ko"
    "s2mps24-regulator.ko"
)

# Module load order exceptions (module -> load_after_module)
declare -A LOAD_AFTER=()

# Modules to exclude
EXCLUDE_MODULES=(
    # Nethunter-related external Wi-Fi stack modules.
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

# Optional file with exclusion patterns (one per line, '#' comments allowed)
if [ -n "$EXCLUDE_LIST_FILE" ]; then
    if [ -f "$EXCLUDE_LIST_FILE" ]; then
        while IFS= read -r pattern; do
            pattern="${pattern%%#*}"
            pattern="${pattern#"${pattern%%[![:space:]]*}"}"
            pattern="${pattern%"${pattern##*[![:space:]]}"}"
            [ -n "$pattern" ] && EXCLUDE_MODULES+=("$pattern")
        done < "$EXCLUDE_LIST_FILE"
    else
        log_warn "Exclude list file not found: $EXCLUDE_LIST_FILE"
    fi
fi

is_module_excluded() {
    local module="$1"
    local pattern

    for pattern in "${EXCLUDE_MODULES[@]}"; do
        if [[ "$module" == $pattern ]]; then
            return 0
        fi
    done

    return 1
}

if [ "${#EXCLUDE_MODULES[@]}" -gt 0 ]; then
    log_info "Applying ${#EXCLUDE_MODULES[@]} module exclusion pattern(s)"
fi

# Run depmod to generate dependency information
DEPMOD_BASE=$(dirname "$(dirname "$(dirname "$MODULES_DIR")")")
depmod -b "$DEPMOD_BASE" "$KERNEL_VERSION" 2>/dev/null || {
    log_warn "depmod failed, will use modules.order without dependency resolution"
}

# Extract module names from modules.order
ALL_MODULES=$(mktemp)
while IFS= read -r line; do
    basename "$line"
done < "$MODULES_ORDER" > "$ALL_MODULES"

# Remove excluded modules (match against basenames)
TEMP_MODULES=$(mktemp)
while IFS= read -r module; do
    if ! is_module_excluded "$module"; then
        echo "$module" >> "$TEMP_MODULES"
    fi
done < "$ALL_MODULES"

mv "$TEMP_MODULES" "$ALL_MODULES"

# Use depmod's modules.dep for dependency-ordered list
MODULES_DEP="$MODULES_DIR/modules.dep"
if [ -f "$MODULES_DEP" ]; then
    SORTED_MODULES=$(mktemp)
    declare -A PROCESSED

    # Recursive dependency resolver
    resolve_deps() {
        local modname="$1"

        [ "${PROCESSED[$modname]}" = "1" ] && return

        # Get dependencies from modules.dep
        local deps
        deps=$(grep "/${modname}:" "$MODULES_DEP" 2>/dev/null | head -1 | cut -d: -f2 || true)

        # Process dependencies first (depth-first)
        for dep in $deps; do
            local depname
            depname=$(basename "$dep")
            if grep -q "^${depname}$" "$ALL_MODULES"; then
                resolve_deps "$depname"
            fi
        done

        PROCESSED[$modname]=1
        echo "$modname" >> "$SORTED_MODULES"
    }

    # Process all modules
    while IFS= read -r module; do
        resolve_deps "$module"
    done < "$ALL_MODULES"

    # Build final modules.load with priority modules first
    : > "$OUTPUT_FILE"

    for module in "${PRIORITY_MODULES[@]}"; do
        if grep -q "^$module$" "$SORTED_MODULES"; then
            echo "$module" >> "$OUTPUT_FILE"
            sed -i "/^$module$/d" "$SORTED_MODULES"
        fi
    done

    cat "$SORTED_MODULES" >> "$OUTPUT_FILE"
    rm -f "$SORTED_MODULES"

    # Apply ordering exceptions
    for module in "${!LOAD_AFTER[@]}"; do
        after_module="${LOAD_AFTER[$module]}"

        # Check if both modules exist in the output file
        if grep -q "^$module$" "$OUTPUT_FILE" && grep -q "^$after_module$" "$OUTPUT_FILE"; then
            # If 'module' appears BEFORE 'after_module', move it
            mod_line=$(grep -n "^$module$" "$OUTPUT_FILE" | cut -d: -f1)
            after_line=$(grep -n "^$after_module$" "$OUTPUT_FILE" | cut -d: -f1)

            if [ "$mod_line" -lt "$after_line" ]; then
                # Remove the module from its current position
                sed -i "/^$module$/d" "$OUTPUT_FILE"

                # Insert 'module' immediately AFTER 'after_module'
                # We use sed's 'a' command (append after match)
                sed -i "/^$after_module$/a $module" "$OUTPUT_FILE"
            fi
        fi
    done
else
    # Fallback: use modules.order with priority modules first
    : > "$OUTPUT_FILE"

    for module in "${PRIORITY_MODULES[@]}"; do
        if grep -q "^$module$" "$ALL_MODULES"; then
            echo "$module" >> "$OUTPUT_FILE"
            sed -i "/^$module$/d" "$ALL_MODULES"
        fi
    done

    cat "$ALL_MODULES" >> "$OUTPUT_FILE"
fi

rm -f "$ALL_MODULES"

log_info "Generated modules.load with $(wc -l < "$OUTPUT_FILE") modules"
cp -f "$OUTPUT_FILE" "$KERNEL_DIR/modules.load.gen"
