#!/usr/bin/env bash
# Build libstd and plt with the same toolchain and instrumentation.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
mode=${1:-test}
std_source=${LIBSTD_SOURCE:-$root/.deps/libstd}
std_source=$(cd "$std_source" && pwd)
build_dir="$root/.build/ci-$mode"
jobs=${CI_JOBS:-$(getconf _NPROCESSORS_ONLN)}

case "$mode" in
    build|test|e2e) ;;
    asan|ubsan)
        "$CXX" --version | grep -qi clang
        sanitizer=address
        if [[ "$mode" == ubsan ]]; then sanitizer=undefined; fi
        export CXXFLAGS="${CXXFLAGS:-} -g -fsanitize=$sanitizer -fno-sanitize-recover=all -fno-omit-frame-pointer"
        export LDFLAGS="${LDFLAGS:-} -fsanitize=$sanitizer"
        export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
        export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
        ;;
    coverage)
        "$CXX" --version | grep -qi clang
        export CXXFLAGS="${CXXFLAGS:-} -g -fprofile-instr-generate -fcoverage-mapping"
        export CPPFLAGS="${CPPFLAGS:-} -D__LLVM_INSTR_PROFILE_GENERATE=1"
        export LDFLAGS="${LDFLAGS:-} -fprofile-instr-generate"
        mkdir -p "$build_dir/profiles"
        export LLVM_PROFILE_FILE="$build_dir/profiles/%m-%p.profraw"
        ;;
    *) echo "usage: $0 build|test|e2e|asan|ubsan|coverage" >&2; exit 2 ;;
esac

"$CXX" --version
(
    cd "$std_source"
    if [[ "$mode" == coverage ]]; then
        # libstd's assembly context entry reads a fixed register before any
        # compiler-generated code may use it. Keep profiling out of that ABI.
        export CXXFLAGS="$CXXFLAGS -fprofile-list=$root/dev/coverage-libstd.list"
    fi
    python3 ./build -B "$build_dir/std" -j "$jobs" libstd
)
export CPPFLAGS="${CPPFLAGS:-} -I$std_source"
export LDFLAGS="${LDFLAGS:-} -L$build_dir/std"
if [[ "$mode" == build ]]; then
    python3 ./build -B "$build_dir/plt" -j "$jobs" plt plt_wayland_integration_tests e2e-binaries
elif [[ "$mode" != e2e ]]; then
    test_targets=(test plt)
    if [[ "$(uname -s)" == Linux ]]; then
        test_targets+=(plt_wayland_integration_tests)
    else
        test_targets+=(plt_cocoa_tests)
    fi
    # Export the executable paths as well as the test stamp for llvm-cov.
    python3 ./build -B "$build_dir/plt" -j "$jobs" "${test_targets[@]}"
fi
if [[ "$(uname -s)" == Linux && "$mode" != build ]]; then
    python3 ./build -B "$build_dir/plt" -j "$jobs" e2e-binaries
    renderers="shm lavapipe"
    # Sway refuses to run as root. Containers build as root, then run clients
    # and the compositor as nobody, retaining all artifacts in the workspace.
    runner=()
    if [[ "$(id -u)" == 0 ]]; then
        chown -R nobody "$build_dir"
        runner=(runuser -u nobody --)
    fi
    e2e_status=0
    for renderer in $renderers; do
        "${runner[@]}" python3 tst/e2e/run.py --binary-dir "$build_dir/plt/e2e" \
            --artifacts "$build_dir/e2e-$renderer" --renderer "$renderer" || e2e_status=1
    done
    if [[ "$e2e_status" != 0 ]]; then exit "$e2e_status"; fi
fi
if [[ "$(uname -s)" == Darwin && "$mode" != build ]]; then
    python3 ./build -B "$build_dir/plt" -j "$jobs" e2e-binaries
    python3 tst/e2e/run.py --binary-dir "$build_dir/plt/e2e" \
        --artifacts "$build_dir/e2e-metal" --renderer metal --source-dir tst/e2e/cocoa
fi
if [[ "$mode" == coverage ]]; then
    python3 dev/ci_coverage.py "$build_dir/plt" "$build_dir/profiles" "$root/.build/coverage"
fi
