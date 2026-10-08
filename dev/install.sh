#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 PREFIX" >&2
    exit 2
fi

prefix=$1

install -Dm755 im "$prefix/bin/im"

for tool in screenshot view play read edit choose ui; do
    ln -sf im "$prefix/bin/im$tool"
done

for file in app/applications/*.desktop app/icons/hicolor/scalable/apps/*.svg; do
    install -Dm644 "$file" "$prefix/share/${file#app/}"
done
