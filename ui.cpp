#include "ui.h"

#include "gpu.h"
#include "util.h"
#include "imgui_plt.h"
#include "chaos_monkey.h"

#include <std/ios/sys.h>
#include <std/str/builder.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <imgui.h>
#include <stdlib.h>
#include <plt/fiber.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <imgui_internal.h>

using namespace stl;

namespace {
    float gScale = 1.f;
    // ImGui's drag and double-click thresholds, as a design length: its
    // ScaleAllSizes leaves them alone
    constexpr Design mouseThreshold = 6_d;
}

float uiScale() {
    return gScale;
}

void initUiScale() {
    if (const char* s = getenv("IM_SCALE")) {
        double v = parseFloat(StringView(s));

        if (v > 0.0) {
            gScale = (float)v;
        }
    }
}

float px(Design d) {
    float v = floorf(d.value * gScale + .5f);

    return d.value > 0.f && v < 1.f ? 1.f : v;
}

int pxi(Design d) {
    return (int)px(d);
}

ImVec2 px(Design w, Design h) {
    return ImVec2(px(w), px(h));
}

// from a fresh default every time: ScaleAllSizes compounds
ImGuiStyle uiStyle() {
    ImGuiStyle style;

    style.ScaleAllSizes(gScale);
    style.FontScaleMain = gScale;

    return style;
}

void applyUiStyle() {
    ImGui::GetStyle() = uiStyle();

    ImGuiIO& io = ImGui::GetIO();

    io.MouseDragThreshold = px(mouseThreshold);
    io.MouseDoubleClickMaxDist = px(mouseThreshold);
}

// The runtime behind Ui. The tool is a fiber of the platform's scheduler:
// it runs until it asks for an event there is not, and sleeps in next().
// The platform's loop runs on the main stack; its callbacks put the event
// there and wake the tool, which runs inside the callback until it asks
// again. So a frame is the driver's, as for any tool: the swapchain, the
// ImGui frame begun, the tool's turn, the frame rendered and presented,
// all within plt's frame callback; the tool only sees its turn.

namespace {
    // the tool's stack: ImGui's frames, a driver's shader compiler as the
    // device comes up, a decoder
    constexpr size_t toolStack = 16u << 20;

    struct ToolWindow;

    // the process's one tool on its fiber: what it is, what it returned,
    // whether it has
    struct Running final: Runable {
        ObjPool* pool = nullptr;
        plt::Platform* platform = nullptr;
        plt::Fiber* fiber = nullptr;
        int (*main)(int argc, char** argv) = nullptr;
        int argc = 0;
        char** argv = nullptr;
        int result = 1;
        bool finished = false;
        ToolWindow* window = nullptr;

        void run() override;
    };

    Running* gRunning = nullptr;

    // the tool's window: plt's window and the frames drawn into it on the
    // main stack, the events the tool takes on its fiber
    struct ToolWindow final: Ui, UiFrame, plt::WindowEvents {
        Running* running = nullptr;
        FrameDriver driver;
        StringBuilder appId;
        StringBuilder title;
        // what waits for the tool: the frame begun, the window asked to go
        bool framePending = false;
        bool closePending = false;
        // the close was handed over, or the loop is over: nothing more
        bool gone = false;
        // the tool sleeps in next(), and only there is it woken
        bool parked = false;

        bool next(UiEvent& event) override;
        int frame() override;
        void close() override;
    };

    void Running::run() {
        try {
            result = main(argc, argv);
        } catch (...) {
            sysE << "im "_sv << gTool << ": "_sv << Exception::current() << endL;
            result = 1;
        }

        finished = true;
        platform->stop();
    }

    bool ToolWindow::next(UiEvent& event) {
        // the frame the tool held, if any, is done: the driver renders it
        // once the tool sleeps
        for (;;) {
            if (closePending) {
                closePending = false;
                gone = true;
                event.kind = UiEvent::Kind::Close;

                return true;
            }

            if (gone) {
                return false;
            }

            if (framePending) {
                framePending = false;
                event.kind = UiEvent::Kind::Frame;

                return true;
            }

            parked = true;
            running->platform->scheduler()->current()->park();
            parked = false;
        }
    }

    // the driver's call between ImGui's NewFrame and Render: the tool's
    // turn. A tool that is not asleep in next() (a platform call of its
    // own that drew) gets none, a tool that is done neither
    int ToolWindow::frame() {
        if (running->finished) {
            return -1;
        }

        if (!parked || gone) {
            return 0;
        }

        ImGuiErrorRecoveryState state;

        ImGui::ErrorRecoveryStoreState(&state);
        framePending = true;
        running->fiber->wake();
        framePending = false;

        if (!running->finished) {
            return 0;
        }

        // a tool that ended inside its frame, by a return or an exception,
        // may have left its windows open: closed for it, the frame its last
        ImGuiIO& io = ImGui::GetIO();
        bool asserting = io.ConfigErrorRecoveryEnableAssert;

        io.ConfigErrorRecoveryEnableAssert = false;
        ImGui::ErrorRecoveryTryToRecoverState(&state);
        io.ConfigErrorRecoveryEnableAssert = asserting;

        return -1;
    }

    void ToolWindow::close() {
        traceText("closed"_sv);

        if (gone) {
            return;
        }

        closePending = true;

        if (parked) {
            running->fiber->wake();
        }
    }
}

Ui& Ui::open(const UiOptions& options) {
    Running* running = gRunning;

    if (!running || running->platform->scheduler()->current() == nullptr) {
        fail("a window opens from the tool's own fiber"_sv);
    }

    if (running->window) {
        fail("one window per tool"_sv);
    }

    ObjPool& pool = *running->pool;
    // made before the window, gone after it: the window calls into it
    ToolWindow& self = *pool.make<ToolWindow>();
    ImGuiPlt& imgui = *ImGuiPlt::create(pool);
    int width = pxi(options.width);
    int height = pxi(options.height);

    self.running = running;
    self.appId << "im-"_sv << gTool;
    self.title << "im "_sv << gTool;

    plt::WindowOptions made;

    made.appId = sv(self.appId);
    made.title = sv(self.title);
    made.width = (u32)width;
    made.height = (u32)height;
    made.input = imgui.sink();
    made.events = &self;
    made.frame = &self.driver;

    plt::Window& window = *running->platform->createWindow(pool, made);
    // asked for in the platform's logical units, it says what pixels it
    // made of them: not the design's on an output that scales logical
    // units by itself, or over the screen; a resize is in pixels
    plt::WindowInfo info = window.info();

    clampWindowSize(info, width, height);

    if (width != (int)info.width || height != (int)info.height) {
        window.requestResize((u32)width, (u32)height);
    }

    setupGpu(pool, window, VulkanWants());

    self.driver.platform = running->platform;
    self.driver.window = &window;
    self.driver.imgui = &imgui;
    self.driver.ui = &self;
    running->window = &self;
    window.requestShow();
    window.requestFrame();

    return self;
}

int runTool(StringView name, int (*main)(int argc, char** argv), int argc, char** argv) {
    gTool = name;
    gTraceFrames = getenv("IM_TRACE_FRAMES") != nullptr;
    initUiScale();

    try {
        ObjPool::Ref pool = ObjPool::fromMemory();

        gChaos = ChaosMonkey::create(*pool);

        Running& running = *pool->make<Running>();

        running.pool = &*pool;
        running.main = main;
        running.argc = argc;
        running.argv = argv;
        running.platform = plt::Platform::create(*pool);
        gRunning = &running;
        // the tool runs from here until it first sleeps
        running.fiber = running.platform->scheduler()->create(*pool, running, toolStack);

        if (!running.finished) {
            running.platform->run();
        }

        // the loop is over with the tool still asleep: its window is gone,
        // and its last next() says so
        if (!running.finished && running.window) {
            running.window->gone = true;
            running.fiber->wake();
        }

        gRunning = nullptr;

        return running.result;
    } catch (...) {
        gRunning = nullptr;
        sysE << "im "_sv << gTool << ": "_sv << Exception::current() << endL;

        return 1;
    }
}
