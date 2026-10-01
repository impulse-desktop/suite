#include "ui.h"

#include "util.h"
#include "frame.h"
#include "pooled.h"
#include "imgui_plt.h"

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

namespace {
    constexpr size_t toolStack = 16u << 20;

    struct UiImpl final: Ui, UiFrame, plt::WindowEvents, Runable {
        ObjPool* pool = nullptr;
        plt::Platform* platform = nullptr;
        StringView name;
        int (*main)(ObjPool& pool, Ui& ui, int argc, char** argv) = nullptr;
        int argc = 0;
        char** argv = nullptr;
        plt::Fiber* fiber = nullptr;
        int result = 1;
        Buffer error;
        bool finished = false;
        float scale = 1.f;
        bool traceFrames = false;
        plt::Window* window = nullptr;
        Renderer* renderer = nullptr;
        FrameDriver driver;
        StringBuilder appId;
        StringBuilder title;
        Vector<ImTextureData*> textures;
        bool framePending = false;
        bool closePending = false;
        bool gone = false;
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

        void run() override;
        int frame() override;
        void close() override;

        void tendTextures();
        void dropTextures();
    };
}

ImVec2 Ui::px(Design w, Design h) {
    return ImVec2(px(w), px(h));
}

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
    plt::WindowInfo info = shown.info();

    clampWindowSize(info, width, height);

    if (width != (int)info.width || height != (int)info.height) {
        shown.requestResize((u32)width, (u32)height);
    }

    pooledGuard(*pool, [this] {
        dropTextures();
    });
    setupImGuiContext(*pool, scale);
    renderer = Renderer::create(*pool, shown);

    driver.platform = platform;
    driver.window = &shown;
    driver.imgui = &imgui;
    driver.renderer = renderer;
    driver.ui = this;
    driver.tool = name;
    driver.traceFrames = traceFrames;
    window = &shown;
    shown.requestShow();
    shown.requestFrame();
}

bool UiImpl::next(UiEvent& event) {
    if (!window) {
        fail("no window to take events from: open it first"_sv);
    }

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

ImTextureRef UiImpl::loadTexture(u32 width, u32 height, const void* rgba) {
    if (textures.length() >= renderer->maxTextures()) {
        fail(sv(StringBuilder() << "more than "_sv << (i64)renderer->maxTextures() << " textures at once"_sv));
    }

    ImTextureData* texture = IM_NEW(ImTextureData)();

    texture->Create(ImTextureFormat_RGBA32, (int)width, (int)height);
    memcpy(texture->GetPixels(), rgba, (size_t)width * height * 4);
    ImGui::RegisterUserTexture(texture);
    textures.pushBack(texture);

    return texture->GetTexRef();
}

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
    return renderer->maxTextureSide();
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
        error = Buffer(Exception::current());
    }

    finished = true;
    platform->stop();
}

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

void UiImpl::dropTextures() {
    for (ImTextureData* texture : textures) {
        IM_DELETE(texture);
    }

    textures.clear();
}

int runTool(StringView name, int (*main)(ObjPool& pool, Ui& ui, int argc, char** argv), int argc, char** argv) {
    ObjPool::Ref pool = ObjPool::fromMemory();
    UiImpl& ui = *pool->make<UiImpl>();

    ui.pool = &*pool;
    ui.name = name;
    ui.main = main;
    ui.argc = argc;
    ui.argv = argv;
    ui.scale = scaleFromEnv();
    ui.traceFrames = getenv("IM_TRACE_FRAMES") != nullptr;
    ui.platform = plt::Platform::create(*pool);
    ui.fiber = ui.platform->scheduler()->create(*pool, ui, toolStack);

    if (!ui.finished) {
        ui.platform->run();
    }

    if (!ui.finished && ui.window) {
        ui.gone = true;
        ui.fiber->wake();
    }

    if (!ui.error.empty()) {
        throw ToolError(Buffer(sv(ui.error)));
    }

    return ui.result;
}
