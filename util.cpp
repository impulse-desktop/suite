#include "util.h"

#include <time.h>

using namespace stl;

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

void hex16(StringBuilder& out, u64 v) {
    char b[16];

    for (int i = 15; i >= 0; i--, v >>= 4) {
        b[i] = "0123456789abcdef"[v & 15];
    }

    out << StringView((const u8*)b, 16);
}

u32 nowMsec() {
    timespec ts{};

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

u64 nowUsec() {
    timespec ts{};

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (u64)ts.tv_sec * 1000000 + (u64)ts.tv_nsec / 1000;
}
