#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/dbg/verify.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>

#include <string.h>

inline stl::StringView operator""_sv(const char* s, size_t len) {
    return {(const u8*)s, len};
}

// the compositor is single threaded: one shared scratch builder serves all
// transient formatting. sb() resets it on every acquire — never hold the
// reference across a call that may format too; overlapping lifetimes get
// their own local StringBuilder
stl::StringBuilder& sb();

inline stl::StringView sv(const stl::Buffer& b) {
    return {(const u8*)b.data(), b.used()};
}

double parseFloat(stl::StringView s);

// zero-padded formatting the libstd streams do not carry: sixteen
// lowercase hex digits (token halves)
void hex16(stl::StringBuilder& out, u64 v);

// CLOCK_MONOTONIC in milliseconds, wrapping round zero every 49.7 days:
// age a stamp by the unsigned difference (u32)(now - then), never by
// comparing the two
u32 nowMsec();
// the same clock in microseconds, 64 bits wide: it never wraps
u64 nowUsec();

// i32 addition that clamps instead of overflowing: hostile clients feed
// INT32_MIN/MAX into accumulating protocol values (attach offsets)
inline i32 satAddI32(i32 a, i32 b) {
    constexpr i64 lo = -0x80000000ll, hi = 0x7fffffff;
    i64 s = (i64)a + b;

    return (i32)(s < lo ? lo : s > hi ? hi : s);
}

template <typename T>
bool removeOne(stl::Vector<T>& v, const T& t) {
    for (size_t i = 0; i < v.length(); i++) {
        if (v[i] == t) {
            memmove(v.mutData() + i, v.data() + i + 1, (v.length() - i - 1) * sizeof(T));
            v.popBack();

            return true;
        }
    }

    return false;
}

template <typename T>
void insertAt(stl::Vector<T>& v, size_t idx, const T& t) {
    v.pushBack(t);
    memmove(v.mutData() + idx + 1, v.data() + idx, (v.length() - 1 - idx) * sizeof(T));
    v.mut(idx) = t;
}

template <typename T>
bool contains(const stl::Vector<T>& v, const T& t) {
    for (size_t i = 0; i < v.length(); i++) {
        if (v[i] == t) {
            return true;
        }
    }

    return false;
}

template <typename T>
long indexOf(const stl::Vector<T>& v, const T& t) {
    for (size_t i = 0; i < v.length(); i++) {
        if (v[i] == t) {
            return (long)i;
        }
    }

    return -1;
}
