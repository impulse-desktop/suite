#pragma once

namespace stl {
    class StringView;
}

[[noreturn]] void raiseError(stl::StringView message);
[[noreturn]] void fail(stl::StringView m, const char* file = __builtin_FILE(), int line = __builtin_LINE());
