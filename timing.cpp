#include "timing.h"

#include <std/str/view.h>
#include <std/ios/out_zc.h>

using namespace stl;

template <>
void stl::output<ZeroCopyOutput, MS>(ZeroCopyOutput& out, MS duration) {
    u64 tenths = duration.ms / 100 + (duration.ms % 100 >= 50);

    out << tenths / 10 << StringView(u8".") << tenths % 10;
}
