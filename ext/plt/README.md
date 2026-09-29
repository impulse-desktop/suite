# plt

[![CI](https://github.com/pg83/plt/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/pg83/plt/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/pg83/plt/branch/master/graph/badge.svg)](https://app.codecov.io/gh/pg83/plt/tree/master)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-informational)](STYLE.md)
[![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS-8a8a8a)](#ci)

Small native desktop platform layer built on libstd.

The library provides native Linux/Wayland and macOS/Cocoa backends. Its public
surface covers the event loop, generic keyboard and pointer input, clipboard
selections, frame requests and window state. A renderer receives only an
opaque native context; public headers contain no Vulkan, Metal, Wayland or
operating-system headers. Objects are allocated in caller-owned `stl::ObjPool`
instances. The platform owns the event loop; clients register one-shot
file-descriptor callbacks and replaceable timer callbacks through `Poller`.

Build with:

```sh
./build
```

## CI

The initial `build` job gates every test and coverage job. The suite runs
with GCC/glibc, Clang/Alpine musl, Clang ASan, Clang UBSan, and Clang on
Darwin (including Cocoa tests). Each job builds a pinned libstd revision
with the same compiler and instrumentation as plt.

Linux and Darwin coverage runs export separate LLVM tracefiles. One
`coverage` job merges them and uploads the combined report to Codecov
using OIDC. Tests, dependencies and generated sources are excluded.

To reproduce a job with the required compiler and dependencies installed:

```sh
CC=clang CXX=clang++ LIBSTD_SOURCE=../std bash dev/ci.sh test
CC=clang CXX=clang++ LIBSTD_SOURCE=../std bash dev/ci.sh asan
CC=clang CXX=clang++ LIBSTD_SOURCE=../std bash dev/ci.sh ubsan
CC=clang CXX=clang++ LIBSTD_SOURCE=../std bash dev/ci.sh coverage
```

Coverage additionally requires `llvm-profdata` and `llvm-cov` from the
compiler's LLVM version. The workflow records the container images and
libstd revision used by CI; `dev/ci_linux.sh` installs their dependencies.

## End-to-end tests

[Real compositor scenarios](tst/e2e/README.md) pair C++ applications with Python
drivers. Every Linux test job (GCC, musl, ASan, UBSan and coverage) runs all scenarios
under Sway with both shm and lavapipe, checks captured pixels, and uploads PNG
screenshots and logs. Linux coverage
includes both rendering paths. All CI configurations use explicit jobs.

```sh
./build e2e-binaries
./build e2e
```
