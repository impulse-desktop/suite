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

#include <imgui.h>
#include <stdlib.h>
#include <string.h>
#include <plt/fiber.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <imgui_internal.h>

using namespace stl;

// The runtime behind Ui. The tool is a fiber of the platform's scheduler:
// it runs until it asks for an event there is not, and sleeps in next().
// The platform's loop runs on the main stack; its callbacks put the event
// there and wake the tool, which runs inside the callback until it asks
// again. So a frame is the driver's, as for any tool: the swapchain, the
// ImGui frame begun, the tool's turn, the frame rendered and presented,
// all within plt's frame callback; the tool only sees its turn. All of it
// is one object, made by runTool in the pool it hands the tool.

namespace {
    // the tool's stack: ImGui's frames, a driver's shader compiler as the
    // device comes up, a decoder
    constexpr size_t toolStack = 16u << 20;
    // the texture side when the renderer names no limit
    constexpr u32 defaultTextureSide = 4096;
    // the textures a window holds at once, the released ones still in
    // flight among them: the renderer's descriptor pool is made for them
    // up front
    constexpr u32 maxTextures = 16384;

    struct UiImpl final: Ui, UiFrame, plt::WindowEvents, Runable {
        // the pool all of it lives in, the platform, the fault seam
        ObjPool* pool = nullptr;
        plt::Platform* platform = nullptr;
        ChaosMonkey* chaos = nullptr;
        // the tool: its name, its main and arguments, its fiber, what it
        // returned, whether it has
        StringView name;
        int (*main)(ObjPool& pool, Ui& ui, int argc, char** argv) = nullptr;
        int argc = 0;
        char** argv = nullptr;
        plt::Fiber* fiber = nullptr;
        int result = 1;
        bool finished = false;
        // IM_SCALE, IM_TRACE_FRAMES
        float scale = 1.f;
        bool traceFrames = false;
        // the window, once open, and what draws into it
        plt::Window* window = nullptr;
        Gpu* gpu = nullptr;
        FrameDriver driver;
        StringBuilder appId;
        StringBuilder title;
        // the textures the tool loaded, ImGui's own objects, until the
        // renderer has torn down those it released
        Vector<ImTextureData*> textures;
        // what waits for the tool: the frame begun, the window asked to go
        bool framePending = false;
        bool closePending = false;
        // the close was handed over, or the loop is over: nothing more
        bool gone = false;
        // the tool sleeps in next(), and only there is it woken
        bool parked = false;

        using Ui::px;
        float px(Design d) override;
        void open(const UiOptions& options) override;
        bool next(UiEvent& event) override;
        void requestFullscreen(bool on) override;
        ImTextureRef loadTexture(u32 width, u32 height, const void* rgba) override;
        void releaseTexture(ImTextureRef texture) override;
        u32 maxTextureSide() override;
        bool drawErrorPanel(StringView message) override;
        void trace(StringView what) override;
        void timing(StringView line) override;

        // the fiber's entry: the tool, then the loop's end
        void run() override;
        // the driver's call between ImGui's NewFrame and Render
        int frame() override;
        // the window asked to go
        void close() override;

        void tendTextures();
        void dropTextures();
    };

    float UiImpl::px(Design d) {
        return scaledPx(d, scale);
    }

    void UiImpl::open(const UiOptions& options) {
        if (platform->scheduler()->current() == nullptr) {
            fail("a window opens from the tool's own fiber"_sv);
        }

        if (window) {
            fail("one window per tool"_sv);
        }

        ImGuiPlt& imgui = *ImGuiPlt::create(*pool, scale, traceFrames);
        int width = (int)px(options.width);
        int height = (int)px(options.height);

        appId << "im-"_sv << name;
        title << "im "_sv << name;

        plt::WindowOptions made;

        made.appId = sv(appId);
        made.title = sv(title);
        made.width = (u32)width;
        made.height = (u32)height;
        made.input = imgui.sink();
        made.events = this;
        made.frame = &driver;

        plt::Window& shown = *platform->createWindow(*pool, made);
        // asked for in the platform's logical units, it says what pixels it
        // made of them: not the design's on an output that scales logical
        // units by itself, or over the screen; a resize is in pixels
        plt::WindowInfo info = shown.info();

        clampWindowSize(info, width, height);

        if (width != (int)info.width || height != (int)info.height) {
            shown.requestResize((u32)width, (u32)height);
        }

        GpuOptions wants;

        wants.tool = name;
        wants.scale = scale;
        wants.chaos = chaos;
        wants.traceFrames = traceFrames;
        wants.textures = maxTextures;
        // the textures are ImGui's objects and the tool's: they go after
        // ImGui and its renderer, which tear their device side down
        pooledGuard(*pool, [this] {
            dropTextures();
        });
        gpu = Gpu::createFor(*pool, shown, wants);

        driver.platform = platform;
        driver.window = &shown;
        driver.imgui = &imgui;
        driver.gpu = gpu;
        driver.ui = this;
        window = &shown;
        shown.requestShow();
        shown.requestFrame();
    }

    bool UiImpl::next(UiEvent& event) {
        if (!window) {
            fail("no window to take events from: open it first"_sv);
        }

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
            platform->scheduler()->current()->park();
            parked = false;
        }
    }

    void UiImpl::requestFullscreen(bool on) {
        window->requestFullscreen(on);
    }

    // registered with ImGui, the renderer makes it on the next render
    ImTextureRef UiImpl::loadTexture(u32 width, u32 height, const void* rgba) {
        if (textures.length() >= maxTextures) {
            fail(sv(StringBuilder() << "more than "_sv << (i64)maxTextures << " textures at once"_sv));
        }

        ImTextureData* texture = IM_NEW(ImTextureData)();

        texture->Create(ImTextureFormat_RGBA32, (int)width, (int)height);
        memcpy(texture->GetPixels(), rgba, (size_t)width * height * 4);
        ImGui::RegisterUserTexture(texture);
        textures.pushBack(texture);

        return texture->GetTexRef();
    }

    // told to go: the renderer tears it down once it has gone unused for
    // as many frames as there are in flight, counted from here
    void UiImpl::releaseTexture(ImTextureRef ref) {
        ImTextureData* texture = ref._TexData;

        if (!texture || texture->WantDestroyNextFrame) {
            return;
        }

        texture->WantDestroyNextFrame = true;
        texture->SetStatus(ImTextureStatus_WantDestroy);
        texture->UnusedFrames = 0;
    }

    u32 UiImpl::maxTextureSide() {
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();

        if (pio.Renderer_TextureMaxWidth > 0 && pio.Renderer_TextureMaxHeight > 0) {
            return (u32)min(pio.Renderer_TextureMaxWidth, pio.Renderer_TextureMaxHeight);
        }

        return defaultTextureSide;
    }

    bool UiImpl::drawErrorPanel(StringView message) {
        return ::drawErrorPanel(name, scale, message) != 0;
    }

    void UiImpl::trace(StringView what) {
        traceTool(name, what);
    }

    void UiImpl::timing(StringView line) {
        if (traceFrames) {
            sysE << line << endL;
        }
    }

    void UiImpl::run() {
        try {
            result = main(*pool, *this, argc, argv);
        } catch (...) {
            sysE << "im "_sv << name << ": "_sv << Exception::current() << endL;
            result = 1;
        }

        finished = true;
        platform->stop();
    }

    // the tool's turn. A tool that is not asleep in next() (a platform
    // call of its own that drew) gets none, a tool that is done neither
    int UiImpl::frame() {
        if (finished) {
            return -1;
        }

        if (!parked || gone) {
            return 0;
        }

        ImGuiErrorRecoveryState state;

        tendTextures();
        ImGui::ErrorRecoveryStoreState(&state);
        framePending = true;
        fiber->wake();
        framePending = false;

        if (!finished) {
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

    void UiImpl::close() {
        trace("closed"_sv);

        if (gone) {
            return;
        }

        closePending = true;

        if (parked) {
            fiber->wake();
        }
    }

    // once a frame, before the tool's turn: a copy of the pixels the
    // renderer has taken goes, a texture it has torn down goes, the rest
    // of the released count one more unused frame
    void UiImpl::tendTextures() {
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
    void UiImpl::dropTextures() {
        for (ImTextureData* texture : textures) {
            IM_DELETE(texture);
        }

        textures.clear();
    }
}

int runTool(StringView name, int (*main)(ObjPool& pool, Ui& ui, int argc, char** argv), int argc, char** argv) {
    try {
        ObjPool::Ref pool = ObjPool::fromMemory();
        // made first, gone last: the window and the fiber call into it
        UiImpl& ui = *pool->make<UiImpl>();

        ui.pool = &*pool;
        ui.name = name;
        ui.main = main;
        ui.argc = argc;
        ui.argv = argv;
        ui.scale = scaleFromEnv();
        ui.traceFrames = getenv("IM_TRACE_FRAMES") != nullptr;
        ui.chaos = ChaosMonkey::create(*pool);
        ui.platform = plt::Platform::create(*pool);
        // the tool runs from here until it first sleeps
        ui.fiber = ui.platform->scheduler()->create(*pool, ui, toolStack);

        if (!ui.finished) {
            ui.platform->run();
        }

        // the loop is over with the tool still asleep: its window is gone,
        // and its last next() says so
        if (!ui.finished && ui.window) {
            ui.gone = true;
            ui.fiber->wake();
        }

        return ui.result;
    } catch (...) {
        sysE << "im "_sv << name << ": "_sv << Exception::current() << endL;

        return 1;
    }
}
