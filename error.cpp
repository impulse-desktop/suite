#include "error.h"

#include <std/str/view.h>
#include <std/sys/throw.h>
#include <std/lib/buffer.h>
#include <std/str/builder.h>

#include <string.h>

using namespace stl;

namespace {
    struct ToolError: Exception {
        Buffer msg;

        explicit ToolError(Buffer m);

        ExceptionKind kind() const noexcept override;
        StringView description() override;
    };
}

ToolError::ToolError(Buffer m)
    : msg((Buffer&&)m)
{
}

ExceptionKind ToolError::kind() const noexcept {
    return ExceptionKind::Verify;
}

StringView ToolError::description() {
    return StringView(msg);
}

void raiseError(StringView message) {
    throw ToolError(Buffer(message));
}

void fail(StringView m, const char* file, int line) {
    const char* base = strrchr(file, '/');

    raiseError(StringView(StringBuilder() << StringView(base ? base + 1 : file) << StringView(u8":") << (long)line << StringView(u8": ") << m));
}
