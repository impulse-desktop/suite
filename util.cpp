#include "util.h"

#include <std/ios/sys.h>

#include <time.h>
#include <string.h>

using namespace stl;

ToolError::ToolError(Buffer m)
    : msg((Buffer&&)m)
{
}

ExceptionKind ToolError::kind() const noexcept {
    return ExceptionKind::Verify;
}

StringView ToolError::description() {
    return sv(msg);
}

void fail(StringView m, const char* file, int line) {
    const char* base = strrchr(file, '/');

    throw ToolError(Buffer(sv(StringBuilder() << StringView(base ? base + 1 : file) << ":"_sv << (long)line << ": "_sv << m)));
}

double parseFloat(StringView s) {
    bool neg = s.startsWith("-"_sv);

    if (neg) {
        s = {s.begin() + 1, s.end()};
    }

    StringView ip, fp;

    if (!s.split('.', ip, fp)) {
        ip = s;
        fp = {};
    }

    double r = (double)ip.stou();

    if (!fp.empty()) {
        double f = (double)fp.stou();

        for (size_t i = 0; i < fp.length(); i++) {
            f /= 10.0;
        }

        r += f;
    }

    return neg ? -r : r;
}

void traceTool(StringView tool, StringView what) {
#ifdef IM_FOR_TESTS
    sysO << "im "_sv << tool << ": "_sv << what << endL;
#else
    (void)tool;
    (void)what;
#endif
}

u64 nowNs() {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

void appendMs(StringBuilder& text, u64 ns) {
    u64 tenths = (ns + 50000) / 100000;

    text << (i64)(tenths / 10) << "."_sv << (i64)(tenths % 10);
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
