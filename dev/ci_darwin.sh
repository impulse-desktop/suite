#!/usr/bin/env bash
# Run on a macOS runner: install the libraries the tools want and LLVM 21,
# the clang the Linux jobs build with, from Homebrew, the runner's package
# manager as apt and apk are the containers'; then hand over to dev/ci.sh.
# The scenarios there need no compositor: tst/session.py starts none.
set -euo pipefail
mode=${1:-test}
brew install --quiet bash llvm@21 ffmpeg openal-soft wabt pkgconf
llvm=$(brew --prefix llvm@21)
export PATH="$llvm/bin:$(brew --prefix)/bin:$PATH"
export CC=clang CXX=clang++
OSX_SDK=$(xcrun --sdk macosx --show-sdk-path)
export OSX_SDK SDKROOT="$OSX_SDK"
# openal-soft stays out of the prefix, so its pkg-config file is named here
export PKG_CONFIG_PATH="$(brew --prefix openal-soft)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
# as on Ubuntu, the wasm2c runtime may come without the includes its
# sources want; they come from wabt's tree, at the installed version
rt="$(brew --prefix wabt)/share/wabt/wasm2c"
version=$(wasm2c --version)
for inc in $(grep -ho '"wasm-rt[^"]*\.inc"' "$rt"/*.c | tr -d '"' | sort -u); do
    if [ ! -f "$rt/$inc" ]; then
        curl -fsSL "https://raw.githubusercontent.com/WebAssembly/wabt/$version/wasm2c/$inc" -o "$rt/$inc"
    fi
done
exec bash dev/ci.sh "$mode"
