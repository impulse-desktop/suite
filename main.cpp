#include "edit.h"
#include "play.h"
#include "read.h"
#include "view.h"
#include "ui_demo.h"

#if !defined(__APPLE__)
    #include "screenshot.h"
#endif

#include <std/ios/sys.h>
#include <std/str/view.h>
#include <std/sys/throw.h>
#include <std/mem/obj_pool.h>

#include <string.h>
#include <stdlib.h>

#if defined(IM_FOR_TESTS) && defined(__linux__)
    #include <sys/prctl.h>
#endif

using namespace stl;

namespace {
    struct Tool {
        const char* name;
        int (*run)(ObjPool& pool, int argc, char** argv);
    };

    constexpr Tool tools[] = {
#if !defined(__APPLE__)
        {"screenshot", mainScreenshot},
#endif
        {"view", mainView},
        {"play", mainPlay},
        {"read", mainRead},
        {"edit", mainEdit},
        {"ui", mainUiDemo},
    };

    StringView baseName(const char* path) {
        const char* slash = strrchr(path, '/');

        return StringView(slash ? slash + 1 : path);
    }

    int usage() {
        sysE << StringView(u8"usage: im <tool> [args...]; tools:");

        for (const Tool& tool : tools) {
            sysE << StringView(u8" ") << StringView(tool.name);
        }

        sysE << endL;

        return 2;
    }
}

namespace {
    const Tool* find(StringView name) {
        for (const Tool& tool : tools) {
            if (name == StringView(tool.name)) {
                return &tool;
            }
        }

        return nullptr;
    }
}

int main(int argc, char** argv) {
#if defined(IM_FOR_TESTS) && defined(__linux__)
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif

    StringView self = baseName(argv[0]);
    const Tool* tool = nullptr;

    if (self.length() > 2 && self.startsWith(StringView(u8"im"))) {
        tool = find(StringView(self.begin() + 2, self.end()));
    }

    if (!tool && argc >= 2) {
        tool = find(StringView(argv[1]));

        if (tool) {
            argc--;
            argv++;
        }
    }

    if (!tool) {
        return usage();
    }

    ObjPool* pool = ObjPool::fromMemoryRaw();
    int result;

    try {
        result = tool->run(*pool, argc, argv);
    } catch (...) {
        sysE << StringView(u8"im ") << StringView(tool->name) << StringView(u8": ") << Exception::current() << endL;

        result = 1;
    }

#if defined(IM_FOR_TESTS)
    delete pool;
#endif

    exit(result);
}
