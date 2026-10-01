#include "ui.h"

#include "gpu.h"
#include "util.h"
#include "pooled.h"
#include "imgui_plt.h"
#include "chaos_monkey.h"

#include <std/ios/sys.h>
#include <std/alg/minmax.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <imgui.h>
#include <stdlib.h>
#include <string.h>
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
    // the texture side when the renderer names no limit
    constexpr u32 defaultTextureSide = 4096;
    // released textures linger the frames in flight: room for a few
    // released frame after frame over the tool's own budget
    constexpr u32 releasedSlack = 16;

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
        plt::Window* window = nullptr;
        FrameDriver driver;
        // the textures the tool loaded, ImGui's own objects, until the
        // renderer has torn down those it released
        Vector<ImTextureData*> textures;
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
        ImTextureRef loadTexture(u32 width, u32 height, const void* rgba) override;
        void releaseTexture(ImTextureRef texture) override;
        u32 maxTextureSide() override;
        void requestFullscreen(bool on) override;
        int frame() override;
        void close() override;

        void tendTextures();
        void dropTextures();
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

    // registered with ImGui, the renderer makes it on the next render
    ImTextureRef ToolWindow::loadTexture(u32 width, u32 height, const void* rgba) {
        ImTextureData* texture = IM_NEW(ImTextureData)();

        texture->Create(ImTextureFormat_RGBA32, (int)width, (int)height);
        memcpy(texture->GetPixels(), rgba, (size_t)width * height * 4);
        ImGui::RegisterUserTexture(texture);
        textures.pushBack(texture);

        return texture->GetTexRef();
    }

    // told to go: the renderer tears it down once it has gone unused for
    // as many frames as there are in flight, counted from here
    void ToolWindow::releaseTexture(ImTextureRef ref) {
        ImTextureData* texture = ref._TexData;

        if (!texture || texture->WantDestroyNextFrame) {
            return;
        }

        texture->WantDestroyNextFrame = true;
        texture->SetStatus(ImTextureStatus_WantDestroy);
        texture->UnusedFrames = 0;
    }

    u32 ToolWindow::maxTextureSide() {
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();

        if (pio.Renderer_TextureMaxWidth > 0 && pio.Renderer_TextureMaxHeight > 0) {
            return (u32)min(pio.Renderer_TextureMaxWidth, pio.Renderer_TextureMaxHeight);
        }

        return defaultTextureSide;
    }

    void ToolWindow::requestFullscreen(bool on) {
        window->requestFullscreen(on);
    }

    // once a frame, before the tool's turn: a copy of the pixels the
    // renderer has taken goes, a texture it has torn down goes, the rest
    // of the released count one more unused frame
    void ToolWindow::tendTextures() {
        Vector<ImTextureData*> kept;

        for (ImTextureData* texture : textures) {
            if (texture->WantDestroyNextFrame && texture->Status == ImTextureStatus_Destroyed) {
                ImGui::UnregisterUserTexture(texture);
                IM_DELETE(texture);

                continue;
            }

            if (texture->Status == ImTextureStatus_WantDestroy) {
                texture->UnusedFrames++;
            } else if (texture->Status == ImTextureStatus_OK && texture->Pixels) {
                texture->DestroyPixels();
            }

            kept.pushBack(texture);
        }

        textures.xchg(kept);
    }

    // at the end, once ImGui and its renderer are gone and the device
    // side of every texture with them: the objects themselves
    void ToolWindow::dropTextures() {
        for (ImTextureData* texture : textures) {
            IM_DELETE(texture);
        }

        textures.clear();
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

        tendTextures();
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

    VulkanWants wants;

    wants.textures = 2 * options.textures + releasedSlack;
    // the textures are ImGui's objects and the tool's: they go after
    // ImGui and its renderer, which tear their device side down
    pooledGuard(pool, [w = &self] {
        w->dropTextures();
    });
    setupGpu(pool, window, wants);

    self.window = &window;
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

int drawErrorPanel(StringView msg) {
    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);

    int result = 0;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(28, 28, 32, 255));

    ImGui::Begin("##err", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    float pad = px(24_d);

    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImGui::BeginGroup();
    auto& heading = sb();

    heading << "im "_sv << gTool;
    ImGui::TextDisabled("%s", heading.cStr());
    ImGui::Spacing();
    ImGui::PushTextWrapPos(vp->Size.x - pad);
    ImGui::TextUnformatted((const char*)msg.data(), (const char*)msg.data() + msg.length());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Spacing();

    if (ImGui::Button("Exit", px(120_d, 0_d)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        result = -1;
    }

    ImGui::EndGroup();

    ImGui::End();
    ImGui::PopStyleColor();

    return result;
}
