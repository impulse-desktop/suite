#!/usr/bin/env sh
# Run inside the stock distribution container selected by CI.
set -eu
mode=${1:-test}
toolchain=${2:-clang}
if [ -f /etc/alpine-release ]; then
    apk add --no-cache bash binutils clang21 g++ linux-headers lld python3 pkgconf \
        wayland-dev wayland-protocols libxkbcommon-dev cairo-dev fontconfig-dev vulkan-headers vulkan-loader-dev
    export CC=clang-21 CXX=clang++-21
    export LDFLAGS="${LDFLAGS:-} -fuse-ld=lld"
else
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install --yes --no-install-recommends python3 pkg-config \
        libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libcairo2-dev libfontconfig-dev libvulkan-dev
    if [ "$toolchain" = gcc ]; then
        export CC=gcc CXX=g++
    else
        apt-get install --yes --no-install-recommends clang libclang-rt-dev lld llvm
        export CC=clang CXX=clang++
        export LDFLAGS="${LDFLAGS:-} -fuse-ld=lld"
    fi
fi
if [ "$mode" != build ]; then
    if [ -f /etc/alpine-release ]; then
        apk add --no-cache sway swaybg grim wtype wl-clipboard font-dejavu \
            mesa-vulkan-swrast vulkan-validation-layers runuser libcap-utils
        # The packaged DRM capabilities cannot be granted inside Docker; the
        # headless compositor needs none of them.
        if [ -n "$(getcap /usr/bin/sway)" ]; then setcap -r /usr/bin/sway; fi
    else
        apt-get install --yes --no-install-recommends sway swaybg grim wtype wl-clipboard \
            fonts-dejavu-core mesa-vulkan-drivers vulkan-validationlayers util-linux
    fi
    export VK_DRIVER_FILES=$(find /usr/share/vulkan/icd.d -name 'lvp_icd*.json' -print -quit)
    test -n "$VK_DRIVER_FILES"
    export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
    export LP_NUM_THREADS=2
    export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
fi
exec bash dev/ci.sh "$mode"
