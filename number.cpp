#include "number.h"

#include <std/str/view.h>

using namespace stl;

double parseFloat(StringView s) {
    bool neg = s.startsWith(StringView(u8"-"));

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
