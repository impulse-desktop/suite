#pragma once

#include <std/str/view.h>
#include <std/sys/throw.h>
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

// the tool's name, as its trace lines and panels say it: `im <tool>`
extern stl::StringView gTool;

// the test build's account of a step, for the scenarios to wait on; in
// the ordinary build nothing
void traceText(stl::StringView what);
void traceSize(stl::StringView what, int w, int h);

// IM_TRACE_FRAMES: a line per frame on stderr with the gap since the last
// one and the time of each phase, and one per decode; to see where a
// jerk comes from
extern bool gTraceFrames;
u64 nowNs();
// "12.3", milliseconds to a tenth
void appendMs(stl::StringBuilder& text, u64 ns);

float clampf(float v, float lo, float hi);

// a tool's own failure, carrying a human message shown verbatim on the
// error panel; raised by fail() with its call site in front. Derives
// stl::Exception so Exception::current() surfaces it in a generic catch,
// like the rest of the suite
struct ToolError: stl::Exception {
    stl::Buffer msg;

    explicit ToolError(stl::Buffer m);

    stl::ExceptionKind kind() const noexcept override;
    stl::StringView description() override;
};

[[noreturn]] void fail(stl::StringView m, const char* file = __builtin_FILE(), int line = __builtin_LINE());
