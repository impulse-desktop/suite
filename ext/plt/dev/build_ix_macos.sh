#!/bin/sh

set -ue

# Cross-compiles the Cocoa backend from Linux. lib/std does not cross-build
# for arm64 yet, but the library archive only needs the libstd headers, which
# come straight from the monorepo checkout. A separate build directory keeps
# the native build cache intact.
BUILD_EXTRA_CPPFLAGS="-I$HOME/monorepo/std ${BUILD_EXTRA_CPPFLAGS-}" \
exec "$HOME/monorepo/ix/ix" run lib/c --target=arm64 -- ./build --target=aarch64-apple-darwin -B .build-darwin "$@"
