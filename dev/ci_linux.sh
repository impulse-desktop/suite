#!/usr/bin/env sh
# Run inside the stock distribution container CI selected: install the
# toolchain, the libraries the tools want and, for a test mode, the
# compositor the scenarios run under; then hand over to dev/ci.sh.
set -eu
mode=${1:-test}
toolchain=${2:-clang}
if [ -f /etc/alpine-release ]; then
    # compiler-rt carries clang's profile runtime, llvm21 its profdata and
    # cov tools, for the coverage mode
    apk add --no-cache bash binutils clang21 compiler-rt llvm21 g++ linux-headers lld python3 pkgconf glslang \
        wayland-dev wayland-protocols libxkbcommon-dev cairo-dev fontconfig-dev \
        vulkan-headers vulkan-loader-dev libpng-dev libjxl-dev
    export CC=clang-21 CXX=clang++-21
    export LDFLAGS="${LDFLAGS:-} -fuse-ld=lld"
    export PATH="/usr/lib/llvm21/bin:$PATH"
else
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    # cairo and fontconfig are the vendored plt's own e2e clients' wants: its
    # graph names them when it is imported, whether they build or not
    apt-get install --yes --no-install-recommends python3 pkg-config glslang-tools \
        libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libcairo2-dev libfontconfig-dev \
        libvulkan-dev libpng-dev libjxl-dev
    if [ "$toolchain" = gcc ]; then
        export CC=gcc CXX=g++
    else
        apt-get install --yes --no-install-recommends clang libclang-rt-dev lld llvm
        export CC=clang CXX=clang++
        export LDFLAGS="${LDFLAGS:-} -fuse-ld=lld"
    fi
fi
if [ "$mode" != build ]; then
    # the scenarios' compositor, drawn by pixman, with grim for its pixels;
    # the tool's own Vulkan is lavapipe, under the validation layer the
    # scenarios' fixture reads for errors; gdb reads a hung tool's stacks
    if [ -f /etc/alpine-release ]; then
        apk add --no-cache sway swaybg grim font-dejavu gdb \
            mesa-vulkan-swrast vulkan-validation-layers runuser libcap-utils
        # The packaged DRM capabilities cannot be granted inside Docker; the
        # headless compositor needs none of them.
        if [ -n "$(getcap /usr/bin/sway)" ]; then setcap -r /usr/bin/sway; fi
    else
        apt-get install --yes --no-install-recommends sway swaybg grim gdb \
            fonts-dejavu-core mesa-vulkan-drivers vulkan-validationlayers util-linux
    fi
    export VK_DRIVER_FILES=$(find /usr/share/vulkan/icd.d -name 'lvp_icd*.json' -print -quit)
    test -n "$VK_DRIVER_FILES"
    export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
    export LP_NUM_THREADS=2
    export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
    # Alpine's sway (1.12) offers colour management, and with it the HDR10
    # swapchain the HDR scenarios want, only under wlroots' Vulkan renderer;
    # the compositor then draws through lavapipe as the tool does: without
    # a DRM node to match, wlroots takes the CPU device when told to, and
    # allocates the headless output's buffers through udmabuf
    if [ -f /etc/alpine-release ]; then
        export IM_E2E_RENDERER=vulkan
        export WLR_RENDERER_FORCE_SOFTWARE=1
    fi
fi
exec bash dev/ci.sh "$mode"
