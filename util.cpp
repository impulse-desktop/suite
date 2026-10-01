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

StringBuilder& sb() {
    static StringBuilder b(512);

    b.reset();

    return b;
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

StringView gTool = "im"_sv;

void traceText(StringView what) {
#ifdef IM_FOR_TESTS
    sysO << "im "_sv << gTool << ": "_sv << what << endL;
#else
    (void)what;
#endif
}

void traceSize(StringView what, int w, int h) {
#ifdef IM_FOR_TESTS
    sysO << "im "_sv << gTool << ": "_sv << what << " "_sv << w << "x"_sv << h << endL;
#else
    (void)what;
    (void)w;
    (void)h;
#endif
}

bool gTraceFrames = false;

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
