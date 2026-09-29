#include "util.h"
#include "screenshot.h"

#include <std/ios/sys.h>
#include <std/str/view.h>

#include <string.h>

using namespace stl;

namespace {
    // One binary, many tools: `im NAME ARGS...` runs the tool NAME, and a
    // link named imNAME (imscreenshot, imview) runs the same tool with the
    // link's own arguments. Each tool takes its arguments as argv[1..].
    struct Tool {
        const char* name;
        int (*run)(int argc, char** argv);
    };

    int runScreenshot(int argc, char** argv) {
        if (argc < 2) {
            sysE << "usage: im screenshot <path|fd:N>"_sv << endL;

            return 2;
        }

        return mainScreenshot(StringView(argv[1]));
    }

    constexpr Tool tools[] = {
        {"screenshot", runScreenshot},
    };

    StringView baseName(const char* path) {
        const char* slash = strrchr(path, '/');

        return StringView(slash ? slash + 1 : path);
    }

    int usage() {
        sysE << "usage: im <tool> [args...]; tools:"_sv;

        for (const Tool& tool : tools) {
            sysE << " "_sv << StringView(tool.name);
        }

        sysE << endL;

        return 2;
    }
}

int main(int argc, char** argv) {
    // the link's name carries the tool: imscreenshot ARGS is im screenshot ARGS
    StringView self = baseName(argv[0]);
    StringView name;
    int skip = 0;

    if (self.length() > 2 && self.startsWith("im"_sv)) {
        name = StringView(self.begin() + 2, self.end());
    } else if (argc >= 2) {
        name = StringView(argv[1]);
        skip = 1;
    } else {
        return usage();
    }

    for (const Tool& tool : tools) {
        if (name == StringView(tool.name)) {
            return tool.run(argc - skip, argv + skip);
        }
    }

    return usage();
}
