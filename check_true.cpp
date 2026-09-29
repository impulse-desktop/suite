#include "check_true.h"

#include "util.h"

#include <std/ios/sys.h>

using namespace stl;

void checkTrue(bool value) {
    if (!value) {
        sysE << "im: a condition that always holds did not"_sv << endL;
    }
}
