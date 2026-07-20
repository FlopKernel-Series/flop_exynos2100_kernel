build() {
    rm -f log.txt
    : > log.txt

    : "${SKIP_FIPS_CRYPTO_INTEGRITY:=1}"
    : "${SKIP_EXYNOS_FMP_INTEGRITY:=1}"
    export SKIP_FIPS_CRYPTO_INTEGRITY
    export SKIP_EXYNOS_FMP_INTEGRITY

    if [ "$USE_CCACHE" == "1" ]; then
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

    rm -rf "$MOD_OUTDIR" 2>/dev/null

    FRAGMENTS=""
    [ "$DO_KSU" = "1" ] && FRAGMENTS="$FRAGMENTS ksu.config"
    [ "$DO_SUKI" = "1" ] && FRAGMENTS="$FRAGMENTS sukisu.config"
    [ "$DO_XXKSU" = "1" ] && FRAGMENTS="$FRAGMENTS xxksu.config"
    [ "$DROIDSPACES" = "1" ] && [ "$DO_REGEN" != "1" ] && FRAGMENTS="$FRAGMENTS droidspaces.config"

    MAKE_JOBS="-j$(nproc --all)"
    MAKE_COMMON_ARGS=(
        O="$OUTDIR"
        CC="$CC"
        LD="$LINKER"
        LLVM=1
        LLVM_IAS=1
        ARCH=arm64
        CROSS_COMPILE="${CCARM64_PREFIX:-aarch64-linux-gnu-}"
        CROSS_COMPILE_ARM32="${CCARM32_PREFIX:-arm-linux-gnueabi-}"
    )

    run_make() {
        if [ "$DO_QUIET" = "1" ]; then
            make "$@" >> log.txt 2>&1
        else
            make "$@" 2>&1 | tee -a log.txt
        fi
    }

    run_make $MAKE_JOBS "${MAKE_COMMON_ARGS[@]}" "$DEFCONFIG" $FRAGMENTS

    if [ "$IS_RELEASE" = "1" ]; then
        VERSION_STR="-Floppy-$FK_VER-$FK_TYPE_SHORT-release"
        VERSION_NOAUTO=1
    else
        VERSION_STR="-Floppy-$FK_VER-$FK_TYPE_SHORT"
        VERSION_NOAUTO=0
    fi

    rm -f "$OUT_KERNEL"

    if [ "$DO_REGEN" = "1" ]; then
        if [ "$DO_KSU" = "1" ] || [ "$DO_SUKI" = "1" ] || [ "$DO_XXKSU" = "1" ]; then
            log_err "Can't regenerate with SU variant argument"
            exit 1
        fi
        cp -f "$OUTDIR/.config" "arch/arm64/configs/$DEFCONFIG"
        log_info "Configuration regenerated. Check the changes!"
        exit 0
    fi

    scripts/config --file "$OUTDIR/.config" --set-str LOCALVERSION "$VERSION_STR"

    if [ "$VERSION_NOAUTO" = "1" ]; then
        scripts/config --file "$OUTDIR/.config" --disable LOCALVERSION_AUTO
    fi

    if [ "$DO_MENUCONFIG" = "1" ]; then
        run_make "${MAKE_COMMON_ARGS[@]}" menuconfig
    fi

    if [ "$DO_FLTO" = "1" ]; then
        scripts/config --file "$OUTDIR/.config" --enable CONFIG_LTO_CLANG_FULL
        scripts/config --file "$OUTDIR/.config" --disable CONFIG_LTO_CLANG_THIN
    fi

    echo -e "\n$(log_info "Starting compilation...")\n"

    run_make $MAKE_JOBS "${MAKE_COMMON_ARGS[@]}" dtbs
    run_make $MAKE_JOBS "${MAKE_COMMON_ARGS[@]}"
    run_make $MAKE_JOBS "${MAKE_COMMON_ARGS[@]}" \
        INSTALL_MOD_STRIP="--strip-debug --keep-section=.ARM.attributes" \
        INSTALL_MOD_PATH="$MOD_OUTDIR" modules_install
}
