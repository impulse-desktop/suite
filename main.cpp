#include "ui.h"
#include "util.h"
#include "view.h"
#include "ui_demo.h"
#include "screenshot.h"

#include <std/ios/sys.h>
#include <std/str/view.h>

#include <string.h>

#if defined(IM_FOR_TESTS) && defined(__linux__)
    #include <sys/prctl.h>
#endif

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

    int runView(int argc, char** argv) {
        return mainView(argc, argv);
    }

    int runUiDemo(int argc, char** argv) {
        return runTool("ui"_sv, mainUiDemo, argc, argv);
    }

    constexpr Tool tools[] = {
        {"screenshot", runScreenshot},
        {"view", runView},
        {"ui", runUiDemo},
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
    // the scenario that started this tool may read its stacks when it fails
    // with the tool alive; Yama lets only an ancestor attach otherwise
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif

    // a link named after a tool runs it: imscreenshot ARGS is im screenshot
    // ARGS; any other name (im, im_test) takes the tool from the first word
    StringView self = baseName(argv[0]);

    if (self.length() > 2 && self.startsWith("im"_sv)) {
        if (const Tool* tool = find(StringView(self.begin() + 2, self.end()))) {
            return tool->run(argc, argv);
        }
    }

    if (argc >= 2) {
        if (const Tool* tool = find(StringView(argv[1]))) {
            return tool->run(argc - 1, argv + 1);
        }
    }

    return usage();
}
