#include "ui.h"

#include "util.h"
#include "pooled.h"
#include "imgui_plt.h"

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
    constexpr Design mouseThreshold = 6_d;

    static float scaleFromEnv() {
        if (const char* s = getenv("IM_SCALE")) {
            double v = parseFloat(StringView(s));

            if (v > 0.0) {
                return (float)v;
            }
        }

        return 1.f;
    }

    static float scaledPx(Design d, float scale) {
        float v = floorf(d.value * scale + .5f);

        return d.value > 0.f && v < 1.f ? 1.f : v;
    }

    static ImGuiStyle scaledStyle(float scale) {
        ImGuiStyle style;

        style.ScaleAllSizes(scale);
        style.FontScaleMain = scale;

        return style;
    }

    static void setupImGuiContext(ObjPool& pool, float scale) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        pooledGuard(pool, [] {
            ImGui::DestroyContext();
        });

        ImGuiIO& io = ImGui::GetIO();

        io.IniFilename = nullptr;
        ImGui::GetStyle() = scaledStyle(scale);
        io.MouseDragThreshold = scaledPx(mouseThreshold, scale);
        io.MouseDoubleClickMaxDist = scaledPx(mouseThreshold, scale);
    }

    static int drawToolErrorPanel(StringView tool, float scale, StringView msg) {
        ImGuiViewport* vp = ImGui::GetMainViewport();

        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);

        int result = 0;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(28, 28, 32, 255));
        ImGui::Begin("##err", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        float pad = scaledPx(24_d, scale);

        ImGui::SetCursorPos(ImVec2(pad, pad));
        ImGui::BeginGroup();

        StringBuilder heading;

        heading << "im "_sv << tool;
        ImGui::TextDisabled("%s", heading.cStr());
        ImGui::Spacing();
        ImGui::PushTextWrapPos(vp->Size.x - pad);
        ImGui::TextUnformatted((const char*)msg.data(), (const char*)msg.data() + msg.length());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Spacing();

        if (ImGui::Button("Exit", ImVec2(scaledPx(120_d, scale), 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            result = -1;
        }

        ImGui::EndGroup();
        ImGui::End();
        ImGui::PopStyleColor();

        return result;
    }

    static void clampWindowSize(const plt::WindowInfo& info, int& w, int& h) {
        int maxW = (int)info.screenPixelWidth * 9 / 10;
        int maxH = (int)info.screenPixelHeight * 9 / 10;

        if (w > maxW) {
            w = maxW;
        }

        if (h > maxH) {
            h = maxH;
        }
    }

    constexpr size_t toolStack = 16u << 20;

    struct UiImpl final: Ui, plt::FrameCallback, plt::WindowEvents, Runable {
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
        ImGuiPlt* imgui = nullptr;
        bool shown = false;
        u32 presentedWidth = 0;
        u32 presentedHeight = 0;
        u64 frameBegan = 0;
        u64 frameCount = 0;
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
        void requestResize(u32 width, u32 height) override;
        RenderImage* uploadImage(ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) override;
        RenderImage* importImage(ObjPool& pool, SharedImage& source, bool hdr) override;
        ImTextureRef loadTexture(u32 width, u32 height, const void* rgba) override;
        void releaseTexture(ImTextureRef texture) override;
        u32 maxTextureSide() override;
        bool drawErrorPanel(StringView message) override;
        void trace(StringView what) override;
        void timing(StringView line) override;

        void run() override;
        bool frame(const plt::WindowInfo& info) override;
        int drawFrame();
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

    imgui = ImGuiPlt::create(*pool, scale, traceFrames);
    int width = (int)px(options.width);
    int height = (int)px(options.height);

    appId << "im-"_sv << name;
    title << "im "_sv << name;

    plt::WindowOptions made;

    made.appId = sv(appId);
    made.title = sv(title);
    made.width = (u32)width;
    made.height = (u32)height;
    made.input = imgui->sink();
    made.events = this;
    made.frame = this;

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
    renderer = Renderer::create(*pool, shown, options.renderer);

    window = &shown;
}

bool UiImpl::next(UiEvent& event) {
    if (!window) {
        fail("no window to take events from: open it first"_sv);
    }

    if (!shown) {
        shown = true;
        window->requestShow();
        window->requestFrame();
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

void UiImpl::requestResize(u32 width, u32 height) {
    if (!window || !width || !height || width > 0x7fffffffu || height > 0x7fffffffu) {
        fail("invalid window resize"_sv);
    }
    int w = (int)width, h = (int)height;
    clampWindowSize(window->info(), w, h);
    window->requestResize((u32)w, (u32)h);
}

RenderImage* UiImpl::uploadImage(ObjPool& owner, u32 width, u32 height, const void* rgba, bool hdr) {
    return renderer->upload(owner, width, height, rgba, hdr);
}

RenderImage* UiImpl::importImage(ObjPool& owner, SharedImage& source, bool hdr) {
    return renderer->import(owner, source, hdr);
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
    return drawToolErrorPanel(name, scale, message) != 0;
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

bool UiImpl::frame(const plt::WindowInfo& info) {
    u64 began = nowNs();
    u64 gap = frameBegan ? began - frameBegan : 0;

    frameBegan = began;
    renderer->beginFrame(info.width, info.height);

    if (info.width != presentedWidth || info.height != presentedHeight) {
        presentedWidth = info.width;
        presentedHeight = info.height;
        traceTool(name, sv(StringBuilder() << "presenting "_sv << (i64)info.width << "x"_sv << (i64)info.height));
    }

    u64 begun = nowNs();

    imgui->newFrame(*window);
    ImGui::NewFrame();

    u64 opened = nowNs();
    int action = drawFrame();
    u64 drew = nowNs();

    ImGui::Render();

    u64 rendered = nowNs();
    bool presented = renderer->endFrame(ImGui::GetDrawData());

    if (traceFrames) {
        StringBuilder text;

        text << "im frame "_sv << (i64)frameCount++ << ": gap "_sv;
        appendMs(text, gap);
        text << " begin "_sv;
        appendMs(text, begun - began);
        text << " new "_sv;
        appendMs(text, opened - begun);
        text << " tool "_sv;
        appendMs(text, drew - opened);
        text << " render "_sv;
        appendMs(text, rendered - drew);
        text << " end "_sv;
        appendMs(text, nowNs() - rendered);

        if (!presented) {
            text << " unpresented"_sv;
        }

        sysE << sv(text) << endL;
    }

    if (action != 0) {
        platform->stop();
    } else {
        window->requestFrame();
    }

    return presented;
}

int UiImpl::drawFrame() {
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
