#!/bin/sh
set -u
cd "$(dirname "$0")"
xcrun -sdk macosx metal --version > version.txt 2>&1
for name in probe_kernel probe_layer probe_host; do
    rm -f "$name.air" "$name.metallib" "$name.ll" "$name.log"
    xcrun -sdk macosx metal -std=metal3.0 -mmacosx-version-min=14.0 -c "$name.metal" -o "$name.air" > "$name.log" 2>&1
    xcrun -sdk macosx metal -std=metal3.0 -mmacosx-version-min=14.0 -S -emit-llvm "$name.metal" -o "$name.ll" >> "$name.log" 2>&1
    if [ -f "$name.air" ]; then
        xcrun -sdk macosx metallib "$name.air" -o "$name.metallib" >> "$name.log" 2>&1
    fi
done
ls -l
