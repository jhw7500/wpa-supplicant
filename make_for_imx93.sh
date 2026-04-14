#!/bin/bash
TARGET=imx93
[ "$SDK_LOC" ] || SDK_LOC=/shared/fsl-imx-wayland/6.6-nanbield
[ "$SDK_NAME" ] || SDK_NAME=armv8a-poky-linux

[ ! -e ${SDK_LOC}/environment-setup-${SDK_NAME} ] && {
    echo "Sorry, please verify: ${SDK_LOC}/environment-setup-${SDK_NAME}"
    exit 1
}

. ${SDK_LOC}/environment-setup-${SDK_NAME}

SYSROOT=${SDK_LOC}/sysroots/${SDK_NAME}
export PKG_CONFIG="pkg-config"
export PKG_CONFIG_SYSROOT_DIR="${SYSROOT}"
export PKG_CONFIG_PATH="${SYSROOT}/usr/lib/pkgconfig:${SYSROOT}/usr/share/pkgconfig"

cd wpa_supplicant

if [ "$1" = "clean" ]; then
    make clean
    rm -f .build_target
    exit $?
fi

# Auto-clean on target switch
if [ -f .build_target ] && [ "$(cat .build_target)" != "$TARGET" ]; then
    echo "Target changed from $(cat .build_target) to ${TARGET}, cleaning..."
    make clean
fi

[ ! -f .config ] && cp defconfig .config

make \
    CC="$CC" \
    EXTRA_CFLAGS="$CFLAGS" \
    LDFLAGS="$LDFLAGS" \
    -j$(nproc) \
    "$@"

if [ $? -eq 0 ]; then
    echo "$TARGET" > .build_target
    echo ""
    echo "Build successful (${TARGET}):"
    file wpa_supplicant wpa_cli wpa_passphrase 2>/dev/null
fi
