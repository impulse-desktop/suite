#include "ui.h"

#include "util.h"
#include "pooled.h"

#include <std/ios/sys.h>
#include <std/alg/minmax.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <time.h>
#include <float.h>
#include <imgui.h>
#include <stdlib.h>
#include <string.h>
#include <plt/fiber.h>
#include <plt/input.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <imgui_internal.h>

using namespace stl;

namespace {
    constexpr ImGuiKey namedKeys[] = {
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_Space,
        ImGuiKey_Escape,
        ImGuiKey_Enter,
        ImGuiKey_Backspace,
        ImGuiKey_Tab,
        ImGuiKey_Insert,
        ImGuiKey_Delete,
        ImGuiKey_Home,
        ImGuiKey_End,
        ImGuiKey_UpArrow,
        ImGuiKey_DownArrow,
        ImGuiKey_LeftArrow,
        ImGuiKey_RightArrow,
        ImGuiKey_PageUp,
        ImGuiKey_PageDown,
        ImGuiKey_None,
        ImGuiKey_F1,
        ImGuiKey_F2,
        ImGuiKey_F3,
        ImGuiKey_F4,
        ImGuiKey_F5,
        ImGuiKey_F6,
        ImGuiKey_F7,
        ImGuiKey_F8,
        ImGuiKey_F9,
        ImGuiKey_F10,
        ImGuiKey_F11,
        ImGuiKey_F12,
        ImGuiKey_F13,
        ImGuiKey_F14,
        ImGuiKey_F15,
        ImGuiKey_F16,
        ImGuiKey_F17,
        ImGuiKey_F18,
        ImGuiKey_F19,
        ImGuiKey_F20,
        ImGuiKey_F21,
        ImGuiKey_F22,
        ImGuiKey_F23,
        ImGuiKey_F24,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_Keypad0,
        ImGuiKey_Keypad1,
        ImGuiKey_Keypad2,
        ImGuiKey_Keypad3,
        ImGuiKey_Keypad4,
        ImGuiKey_Keypad5,
        ImGuiKey_Keypad6,
        ImGuiKey_Keypad7,
        ImGuiKey_Keypad8,
        ImGuiKey_Keypad9,
        ImGuiKey_KeypadDecimal,
        ImGuiKey_KeypadDivide,
        ImGuiKey_KeypadMultiply,
        ImGuiKey_KeypadSubtract,
        ImGuiKey_KeypadAdd,
        ImGuiKey_KeypadEnter,
        ImGuiKey_KeypadEqual,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_Insert,
        ImGuiKey_Delete,
        ImGuiKey_UpArrow,
        ImGuiKey_DownArrow,
        ImGuiKey_LeftArrow,
        ImGuiKey_RightArrow,
        ImGuiKey_Home,
        ImGuiKey_End,
        ImGuiKey_PageUp,
        ImGuiKey_PageDown,
        ImGuiKey_None,
        ImGuiKey_Space,
        ImGuiKey_Tab,
        ImGuiKey_CapsLock,
        ImGuiKey_ScrollLock,
        ImGuiKey_NumLock,
        ImGuiKey_PrintScreen,
        ImGuiKey_Pause,
        ImGuiKey_Menu,
        ImGuiKey_LeftShift,
        ImGuiKey_LeftCtrl,
        ImGuiKey_LeftAlt,
        ImGuiKey_LeftSuper,
        ImGuiKey_RightShift,
        ImGuiKey_RightCtrl,
        ImGuiKey_RightAlt,
        ImGuiKey_RightSuper,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
        ImGuiKey_None,
    };

    static_assert(sizeof(namedKeys) / sizeof(namedKeys[0]) == (size_t)plt::InputKey::Count, "the key table tracks plt::InputKey");

    static ImGuiKey printableKey(u32 codepoint);
    static int mouseButtonIndex(plt::PointerButton button);
    static plt::PointerIcon pointerIcon(ImGuiMouseCursor cursor);

    static ImGuiKey printableKey(u32 codepoint) {
        u32 folded = codepoint | 0x20;

        if (folded >= 'a' && folded <= 'z') {
            return (ImGuiKey)(ImGuiKey_A + (int)(folded - 'a'));
        }

        if (codepoint >= '0' && codepoint <= '9') {
            return (ImGuiKey)(ImGuiKey_0 + (int)(codepoint - '0'));
        }

        switch (codepoint) {
            case ' ':
                return ImGuiKey_Space;
            case '\'':
                return ImGuiKey_Apostrophe;
            case ',':
                return ImGuiKey_Comma;
            case '-':
                return ImGuiKey_Minus;
            case '.':
                return ImGuiKey_Period;
            case '/':
                return ImGuiKey_Slash;
            case ';':
                return ImGuiKey_Semicolon;
            case '=':
                return ImGuiKey_Equal;
            case '[':
                return ImGuiKey_LeftBracket;
            case '\\':
                return ImGuiKey_Backslash;
            case ']':
                return ImGuiKey_RightBracket;
            case '`':
                return ImGuiKey_GraveAccent;
            default:
                return ImGuiKey_None;
        }
    }

    static plt::PointerIcon pointerIcon(ImGuiMouseCursor cursor) {
        switch (cursor) {
            case ImGuiMouseCursor_TextInput:
                return plt::PointerIcon::Text;
            default:
                return plt::PointerIcon::Default;
        }
    }

    static int mouseButtonIndex(plt::PointerButton button) {
        switch (button) {
            case plt::PointerButton::Primary:
                return 0;
            case plt::PointerButton::Secondary:
                return 1;
            case plt::PointerButton::Middle:
                return 2;
            case plt::PointerButton::Auxiliary1:
                return 3;
            case plt::PointerButton::Auxiliary2:
                return 4;
            default:
                return -1;
        }
    }

    static void traceInput(StringView what, i32 x, i32 y) {
#ifdef IM_FOR_TESTS
        sysO << "im input: "_sv << what << " "_sv << x << " "_sv << y << endL;
#else
        (void)what;
        (void)x;
        (void)y;
#endif
    }

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

    struct UiImpl final: Ui, plt::InputSink, plt::FrameCallback, plt::WindowEvents, Runable {
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
        u64 frameNs = 0;
        plt::PointerIcon icon = plt::PointerIcon::Text;
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

        void beginInputFrame();
        void key(const plt::KeyInput& input) override;
        void text(const plt::TextInput& input) override;
        void preedit(StringView text, i32 cursorBegin, i32 cursorEnd) override;
        void pointerMotion(const plt::PointerMotionInput& input) override;
        void pointerButton(const plt::PointerButtonInput& input) override;
        void scroll(const plt::ScrollInput& input) override;
        void focus(bool focused) override;
        void pointerPresence(bool present) override;
        void flush() override;

        void tendTextures();
        void dropTextures();
    };
}

void UiImpl::beginInputFrame() {
    ImGuiIO& io = ImGui::GetIO();
    plt::WindowInfo info = window->info();

    io.BackendPlatformName = "imgui_plt";
    io.DisplaySize = ImVec2((float)info.width, (float)info.height);

    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    u64 now = (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;

    io.DeltaTime = frameNs && now > frameNs ? (float)(now - frameNs) / 1e9f : 1.f / 60.f;
    frameNs = now;

    plt::PointerIcon wanted = pointerIcon(ImGui::GetMouseCursor());

    if (wanted != icon) {
        icon = wanted;
        window->requestPointerIcon(icon);
    }
}

void UiImpl::key(const plt::KeyInput& input) {
    ImGuiIO& io = ImGui::GetIO();

    io.AddKeyEvent(ImGuiMod_Ctrl, (input.modifiers & plt::InputControl) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (input.modifiers & plt::InputShift) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (input.modifiers & plt::InputAlt) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (input.modifiers & plt::InputSuper) != 0);

    if (input.action == plt::InputAction::Repeat) {
        return;
    }

    ImGuiKey key = input.key == plt::InputKey::Printable ? printableKey(input.baseCodepoint) : namedKeys[(int)input.key];

    if (key != ImGuiKey_None) {
        io.AddKeyEvent(key, input.action == plt::InputAction::Press);
    }
}

void UiImpl::text(const plt::TextInput& input) {
    ImGui::GetIO().AddInputCharacter(input.codepoint);
}

void UiImpl::preedit(StringView, i32, i32) {
}

void UiImpl::pointerMotion(const plt::PointerMotionInput& input) {
    traceInput("pointer"_sv, input.pixelX, input.pixelY);

    if (traceFrames) {
        sysE << "im pointer: "_sv << (i64)input.pixelX << " "_sv << (i64)input.pixelY << endL;
    }

    ImGui::GetIO().AddMousePosEvent((float)input.pixelX, (float)input.pixelY);
}

void UiImpl::pointerButton(const plt::PointerButtonInput& input) {
    int button = mouseButtonIndex(input.button);

    if (button < 0) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    io.AddMousePosEvent((float)input.pixelX, (float)input.pixelY);
    io.AddMouseButtonEvent(button, input.pressed);
}

void UiImpl::scroll(const plt::ScrollInput& input) {
    if (traceFrames) {
        sysE << "im wheel: y/100 "_sv << (i64)(input.y * 100.0) << " precise "_sv << (i64)input.precise << " momentum "_sv << (i64)input.momentum << " phase "_sv << (i64)input.phase << endL;
    }

    float x = (float)input.x;
    float y = (float)input.y;

    if (input.precise && ImGui::GetFontSize() > 0.f) {
        float unit = 10.f * scale / (5.f * ImGui::GetFontSize());

        x *= unit;
        y *= unit;
    }

    ImGui::GetIO().AddMouseWheelEvent(x, y);
}

void UiImpl::focus(bool focused) {
    ImGui::GetIO().AddFocusEvent(focused);
}

void UiImpl::pointerPresence(bool present) {
    traceInput(present ? "pointer present"_sv : "pointer absent"_sv, 0, 0);

    if (!present) {
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
}

void UiImpl::flush() {
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

    int width = (int)px(options.width);
    int height = (int)px(options.height);

    appId << "im-"_sv << name;
    title << "im "_sv << name;

    plt::WindowOptions made;

    made.appId = sv(appId);
    made.title = sv(title);
    made.width = (u32)width;
    made.height = (u32)height;
    made.input = this;
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

    beginInputFrame();
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
