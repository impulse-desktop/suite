#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/lib/buffer.h>
#include <std/str/builder.h>

inline stl::StringView operator""_sv(const char* s, size_t len) {
    return {(const u8*)s, len};
}

// the tool is single threaded: one shared scratch builder serves all
// transient formatting. sb() resets it on every acquire — never hold the
// reference across a call that may format too; overlapping lifetimes get
// their own local StringBuilder
stl::StringBuilder& sb();

inline stl::StringView sv(const stl::Buffer& b) {
    return {(const u8*)b.data(), b.used()};
}

double parseFloat(stl::StringView s);
