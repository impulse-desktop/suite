#pragma once

#include <std/str/view.h>
#include <std/sys/throw.h>
#include <std/sys/types.h>
#include <std/lib/buffer.h>
#include <std/str/builder.h>

inline stl::StringView operator""_sv(const char* s, size_t len) {
    return {(const u8*)s, len};
}

inline stl::StringView sv(const stl::Buffer& b) {
    return {(const u8*)b.data(), b.used()};
}

double parseFloat(stl::StringView s);

void traceTool(stl::StringView tool, stl::StringView what);

u64 nowNs();
void appendMs(stl::StringBuilder& text, u64 ns);

float clampf(float v, float lo, float hi);

struct ToolError: stl::Exception {
    stl::Buffer msg;

    explicit ToolError(stl::Buffer m);

    stl::ExceptionKind kind() const noexcept override;
    stl::StringView description() override;
};

[[noreturn]] void fail(stl::StringView m, const char* file = __builtin_FILE(), int line = __builtin_LINE());
