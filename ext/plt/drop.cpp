#include "drop.h"

#include <std/lib/buffer.h>

using namespace plt;
using namespace stl;

namespace {
    const StringView fileScheme(u8"file://");
    const StringView localhostName(u8"localhost");

    int hexValue(u8 digit) {
        if (digit >= '0' && digit <= '9') {
            return digit - '0';
        }
        if (digit >= 'a' && digit <= 'f') {
            return digit - 'a' + 10;
        }
        if (digit >= 'A' && digit <= 'F') {
            return digit - 'A' + 10;
        }
        return -1;
    }

}

bool plt::nextUriListEntry(StringView& payload, StringView& entry) {
    const u8* const bytes = payload.data();
    const size_t length = payload.length();
    size_t index = 0;
    while (index != length) {
        const size_t lineStart = index;
        while (index != length && bytes[index] != '\n') {
            ++index;
        }
        size_t lineEnd = index;
        if (index != length) {
            ++index;
        }
        if (lineEnd != lineStart && bytes[lineEnd - 1] == '\r') {
            --lineEnd;
        }
        if (lineEnd == lineStart || bytes[lineStart] == '#') {
            continue;
        }
        entry = StringView(bytes + lineStart, lineEnd - lineStart);
        payload = StringView(bytes + index, length - index);
        return true;
    }
    payload = {};
    return false;
}

bool plt::fileUriToPath(StringView uri, Buffer& path) {
    if (!uri.startsWith(fileScheme)) {
        return false;
    }
    const StringView rest = uri.suffix(uri.length() - fileScheme.length());
    size_t slash = 0;
    while (slash != rest.length() && rest[slash] != '/') {
        ++slash;
    }
    if (slash == rest.length()) {
        return false;
    }
    const StringView host = rest.prefix(slash);
    if (!host.empty() && !(host == localhostName)) {
        return false;
    }
    const StringView encoded = rest.suffix(rest.length() - slash);
    for (size_t index = 0; index != encoded.length();) {
        u8 byte = encoded[index];
        if (byte == '%') {
            if (encoded.length() - index < 3) {
                return false;
            }
            const int high = hexValue(encoded[index + 1]);
            const int low = hexValue(encoded[index + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            byte = (u8)(high * 16 + low);
            index += 3;
        } else {
            ++index;
        }
        path.append(&byte, 1);
    }
    return true;
}
