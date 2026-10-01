#include "ui.h"
#include "view.h"
#include "ui_demo.h"

#if !defined(__APPLE__)
    #include "screenshot.h"
#endif

#include <std/ios/sys.h>
#include <std/str/view.h>
#include <std/sys/throw.h>

#include <string.h>

#if defined(IM_FOR_TESTS) && defined(__linux__)
    #include <sys/prctl.h>
#endif

using namespace stl;

namespace {
    struct Tool {
        const char* name;
        int (*run)(int argc, char** argv);
    };

#if !defined(__APPLE__)
    int runScreenshot(int argc, char** argv) {
        if (argc < 2) {
            sysE << StringView(u8"usage: im screenshot <path|fd:N>") << endL;

            return 2;
        }

        return runTool(StringView(u8"screenshot"), mainScreenshot, argc, argv);
    }
#endif

    int runView(int argc, char** argv) {
        return runTool(StringView(u8"view"), mainView, argc, argv);
    }

    int runUiDemo(int argc, char** argv) {
        return runTool(StringView(u8"ui"), mainUiDemo, argc, argv);
    }

    constexpr Tool tools[] = {
#if !defined(__APPLE__)
        {"screenshot", runScreenshot},
#endif
        {"view", runView},
        {"ui", runUiDemo},
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

    try {
        return tool->run(argc, argv);
    } catch (...) {
        sysE << StringView(u8"im ") << StringView(tool->name) << StringView(u8": ") << Exception::current() << endL;

        return 1;
    }
}
