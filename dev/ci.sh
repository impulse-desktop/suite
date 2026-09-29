#!/usr/bin/env bash
# One CI job: build the tools, or build them and run every scenario, under
# the toolchain CC/CXX name and the instrumentation the mode asks for. The
# container recipe (dev/ci_linux.sh) installs the packages and sets CC/CXX
# first; on a developer's box the same modes run with the host's tools.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
mode=${1:-test}
build_dir="$root/.build/ci-$mode"
jobs=${CI_JOBS:-$(getconf _NPROCESSORS_ONLN)}

case "$mode" in
    build|test) ;;
    asan|ubsan)
        "$CXX" --version | grep -qi clang
        sanitizer=address
        if [[ "$mode" == ubsan ]]; then sanitizer=undefined; fi
        export CFLAGS="${CFLAGS:-} -g -fsanitize=$sanitizer -fno-sanitize-recover=all -fno-omit-frame-pointer"
        export CXXFLAGS="${CXXFLAGS:-} -g -fsanitize=$sanitizer -fno-sanitize-recover=all -fno-omit-frame-pointer"
        export LDFLAGS="${LDFLAGS:-} -fsanitize=$sanitizer"
        export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
        export LSAN_OPTIONS="suppressions=$root/dev/lsan.supp"
        export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
        # lavapipe compiles shaders through LLVM on worker threads of its
        # own, whose stacks asan's inflated frames overrun: no extra threads
        export LP_NUM_THREADS=0
        ;;
    coverage)
        "$CXX" --version | grep -qi clang
        # the profile list keeps the vendored ext/ out of the instrumentation
        instrument="-g -fprofile-instr-generate -fcoverage-mapping -fprofile-list=$root/dev/coverage.list"
        export CFLAGS="${CFLAGS:-} $instrument"
        export CXXFLAGS="${CXXFLAGS:-} $instrument"
        export LDFLAGS="${LDFLAGS:-} -fprofile-instr-generate"
        # every instrumented process drops a profile here; the scenarios run
        # as nobody in a container, so the directory is open to them
        mkdir -p "$build_dir/profiles"
        chmod 1777 "$build_dir/profiles"
        export LLVM_PROFILE_FILE="$build_dir/profiles/%p.profraw"
        ;;
    *) echo "usage: $0 build|test|asan|ubsan|coverage" >&2; exit 2 ;;
esac

"$CXX" --version
python3 ./build -B "$build_dir" -j "$jobs" im im_test links devices
# Test controls must never ship in the production binary.
python3 - "$build_dir/im" "$build_dir/im_test" <<'PY_CHECK'
from pathlib import Path
import sys
production, testing = (Path(name).read_bytes() for name in sys.argv[1:])
assert b"IM_CHAOS" not in production, "test fault controls in the production binary"
assert b"IM_CHAOS" in testing, "the test binary lacks its fault controls"
PY_CHECK
if [[ "$mode" == build ]]; then
    exit 0
fi
# -k: a scenario node never fails, but a build error in one must not stop
# the others; the final test node reads every verdict. The scenarios'
# Wayland sockets need a short path; what a failed one captured is kept
# for the job's artifacts.
status=0
python3 ./build -B "$build_dir" -j "$jobs" -k -Druntime=/tmp/im-e2e -Devidence="$build_dir/evidence" test || status=1
if [[ "$mode" == coverage ]]; then
    bash dev/ci_coverage.sh "$build_dir" "$build_dir/profiles"
fi
exit "$status"
