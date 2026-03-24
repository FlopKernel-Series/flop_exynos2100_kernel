#!/usr/bin/env bash

set -euo pipefail

KDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [ -f "$KDIR/build/lib/log.sh" ]; then
    # shellcheck source=../lib/log.sh
    source "$KDIR/build/lib/log.sh"
fi

command -v log_info >/dev/null 2>&1 || log_info() { echo "INFO: $*"; }
command -v log_warn >/dev/null 2>&1 || log_warn() { echo "WARNING: $*" >&2; }
command -v log_err  >/dev/null 2>&1 || log_err()  { echo "ERROR: $*" >&2; }

DEFCONFIG="${DEFCONFIG:-exynos2100-r9sxxx_defconfig}"
OUTDIR="${OUTDIR:-$KDIR/out}"
IMAGE="${IMAGE:-$OUTDIR/arch/arm64/boot/Image}"
QEMU_MACHINE="${QEMU_MACHINE:-virt}"
QEMU_CPU="${QEMU_CPU:-cortex-a55}"
QEMU_MEM="${QEMU_MEM:-2048}"
QEMU_SMP="${QEMU_SMP:-4}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-45}"
QEMU_CONSOLE_MODE="${QEMU_CONSOLE_MODE:-pl011}"
QEMU_APPEND_BASE="${QEMU_APPEND_BASE:-}"
QEMU_BUSYBOX_AARCH64="${QEMU_BUSYBOX_AARCH64:-}"
QEMU_INITRAMFS="${QEMU_INITRAMFS:-}"
QEMU_TRACE="${QEMU_TRACE:-}"
QEMU_TRACE_LOG="${QEMU_TRACE_LOG:-$KDIR/build/qemu-smoketest.trace.log}"

if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    log_err "qemu-system-aarch64 is not installed"
    exit 1
fi

build_initramfs() {
    local root="$KDIR/build/qemu-initramfs/root"
    local output="$KDIR/build/qemu-initramfs/initramfs.cpio.gz"

    if [ -n "$QEMU_INITRAMFS" ] && [ -f "$QEMU_INITRAMFS" ]; then
        printf '%s\n' "$QEMU_INITRAMFS"
        return 0
    fi

    if [ -z "$QEMU_BUSYBOX_AARCH64" ]; then
        return 1
    fi

    if [ ! -f "$QEMU_BUSYBOX_AARCH64" ]; then
        log_err "QEMU_BUSYBOX_AARCH64 does not exist: $QEMU_BUSYBOX_AARCH64"
        exit 1
    fi

    rm -rf "$root"
    mkdir -p "$root"/{bin,dev,etc,proc,sys,tmp}
    cp -f "$QEMU_BUSYBOX_AARCH64" "$root/bin/busybox"
    chmod 0755 "$root/bin/busybox"

    (
        cd "$root/bin"
        ln -sf busybox sh
        ln -sf busybox mount
        ln -sf busybox uname
        ln -sf busybox dmesg
        ln -sf busybox ls
        ln -sf busybox cat
        ln -sf busybox insmod
    )

    cat > "$root/init" <<'EOF'
#!/bin/sh
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
echo "[qemu-smoketest] init reached"
uname -a
exec /bin/sh
EOF
    chmod 0755 "$root/init"

    (
        cd "$root"
        find . -print | cpio --quiet -o -H newc | gzip -9 > "$output"
    )

    printf '%s\n' "$output"
}

run_qemu() {
    local initramfs
    local append=
    local -a qemu_cmd
    local status=0

    if [ ! -f "$IMAGE" ]; then
        log_err "Missing kernel Image: $IMAGE"
        log_err "Build the QEMU test kernel first, then re-run this launcher."
        exit 1
    fi

    case "$QEMU_CONSOLE_MODE" in
        pl011)
            append="console=ttyAMA0 earlycon=pl011,0x9000000 ignore_loglevel loglevel=8 initcall_debug nokaslr panic=-1"
            ;;
        smh|semihost|semihosting)
            append="earlycon=smh ignore_loglevel loglevel=8 initcall_debug nokaslr panic=-1"
            ;;
        *)
            log_err "Unsupported QEMU_CONSOLE_MODE: $QEMU_CONSOLE_MODE"
            log_err "Supported values: pl011, smh"
            exit 1
            ;;
    esac

    if [ -n "$QEMU_APPEND_BASE" ]; then
        append="$QEMU_APPEND_BASE"
    fi

    initramfs="$(build_initramfs || true)"
    if [ -n "$initramfs" ] && [ -f "$initramfs" ]; then
        append="$append rdinit=/init"
        log_info "Using initramfs: $initramfs"
    else
        log_warn "No initramfs configured; expected result is a serial log followed by rootfs/init failure"
    fi

    qemu_cmd=(
        qemu-system-aarch64
        -machine "$QEMU_MACHINE"
        -cpu "$QEMU_CPU"
        -m "$QEMU_MEM"
        -smp "$QEMU_SMP"
        -nographic
        -no-reboot
        -monitor none
        -kernel "$IMAGE"
        -append "$append"
    )

    case "$QEMU_CONSOLE_MODE" in
        smh|semihost|semihosting)
            qemu_cmd+=(-semihosting-config enable=on,target=native)
            ;;
    esac

    if [ -n "$QEMU_TRACE" ]; then
        mkdir -p "$(dirname "$QEMU_TRACE_LOG")"
        rm -f "$QEMU_TRACE_LOG"
        qemu_cmd+=(-d "$QEMU_TRACE" -D "$QEMU_TRACE_LOG")
        log_info "QEMU trace enabled: $QEMU_TRACE"
        log_info "QEMU trace log: $QEMU_TRACE_LOG"
    fi

    if [ -n "$initramfs" ] && [ -f "$initramfs" ]; then
        qemu_cmd+=(-initrd "$initramfs")
    fi

    log_info "Launching QEMU smoke test"
    log_info "Console mode: $QEMU_CONSOLE_MODE"
    log_info "Command line: $append"
    set +e
    timeout --foreground "$QEMU_TIMEOUT" "${qemu_cmd[@]}"
    status=$?
    set -e

    case "$status" in
        0)
            ;;
        124)
            log_warn "QEMU reached timeout after ${QEMU_TIMEOUT}s"
            ;;
        130)
            log_warn "QEMU interrupted by user"
            return 130
            ;;
        *)
            log_warn "QEMU exited with status $status"
            return "$status"
            ;;
    esac
}

main() {
    if ! run_qemu; then
        return $?
    fi
}

main "$@"
