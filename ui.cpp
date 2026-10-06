#include "ui.h"

#include "error.h"
#include "number.h"
#include "pooled.h"
#include "timing.h"

#include <std/ios/sys.h>
#include <std/sys/crt.h>
#include <std/sys/throw.h>
#include <std/alg/minmax.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <float.h>
#include <imgui.h>
#include <stdlib.h>
#include <string.h>
#include <plt/fiber.h>
#include <plt/input.h>
#include <plt/poller.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <plt/loop_wake.h>
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
        sysO << StringView(u8"im input: ") << what << StringView(u8" ") << x << StringView(u8" ") << y << endL;
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

        return 2.f;
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
            ImGui::GetIO().BackendPlatformUserData = nullptr;
            ImGui::GetPlatformIO().ClearPlatformHandlers();
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

        heading << StringView(u8"im ") << tool;
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

    struct UiImpl;

    struct CallFrame final: public plt::TimerCallback {
        UiImpl* ui;

        explicit CallFrame(UiImpl* ui);
        ~CallFrame() noexcept;
        void ready() override;
        void schedule(float seconds);
        void cancel();
    };

    struct UiImpl final: Ui, plt::InputSink, plt::FrameCallback, plt::WindowEvents, Runable {
        ObjPool* pool = nullptr;
        plt::Platform* platform_ = nullptr;
        StringView name;
        Runable* body = nullptr;
        plt::Fiber* fiber = nullptr;
        int result = 1;
        Buffer error;
        bool finished = false;
        float scale = 1.f;
        bool traceFrames = false;
        plt::Window* window = nullptr;
        plt::LoopWake* wake = nullptr;
        Renderer* renderer = nullptr;
        u64 frameUs = 0;
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
        void init(const UiOptions& options);
        int run(Runable& body) override;
        bool next(UiEvent& event) override;
        plt::Platform* platform() override;
        void requestFrame() override;
        void requestFullscreen(bool on) override;
        void requestResize(u32 width, u32 height) override;
        RenderImage* uploadImage(ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) override;
        RenderImage* importImage(ObjPool& pool, SharedImage& source, bool hdr) override;
        RenderImage* bindImage(ObjPool& pool, u32 width, u32 height, const void* data, size_t size, size_t stride, Runable& retired) override;
        RenderShader* compileKernel(ObjPool& pool, const CompiledShader& compiled, u32 tile, const ShaderOptions& options) override;
        RenderImage* shadeImage(ObjPool& pool, ShaderFactory& factory, u32 width, u32 height, const void* data, size_t size, bool hdr, Runable& retired) override;
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

CallFrame::CallFrame(UiImpl* ui_)
    : ui(ui_)
{
}

CallFrame::~CallFrame() noexcept {
    cancel();
}

void CallFrame::ready() {
    if (!ui->finished && !ui->gone) {
        ui->window->requestFrame();
    }
}

void CallFrame::schedule(float seconds) {
    // ImGui measures the delay from the current frame's time.
    u64 delay = (u64)ceil((double)seconds * 1e6);
    u64 elapsed = monotonicNowUs() - ui->frameUs;

    ui->platform_->poller()->timeout(delay > elapsed ? delay - elapsed : 0, *this);
}

void CallFrame::cancel() {
    ui->platform_->poller()->cancel(*this);
}

void UiImpl::beginInputFrame() {
    ImGuiIO& io = ImGui::GetIO();
    plt::WindowInfo info = window->info();

    io.BackendPlatformName = "imgui_plt";
    io.DisplaySize = ImVec2((float)info.width, (float)info.height);

    u64 now = monotonicNowUs();

    io.DeltaTime = frameUs && now > frameUs ? (float)(now - frameUs) / 1e6f : 1.f / 60.f;
    frameUs = now;
}

void UiImpl::key(const plt::KeyInput& input) {
    if (input.action == plt::InputAction::Repeat) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    io.AddKeyEvent(ImGuiMod_Ctrl, (input.modifiers & plt::InputControl) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (input.modifiers & plt::InputShift) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (input.modifiers & plt::InputAlt) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (input.modifiers & plt::InputSuper) != 0);

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
    traceInput(StringView(u8"pointer"), input.pixelX, input.pixelY);

    if (traceFrames) {
        sysE << StringView(u8"im pointer: ") << (i64)input.pixelX << StringView(u8" ") << (i64)input.pixelY << endL;
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
        sysE << StringView(u8"im wheel: y/100 ") << (i64)(input.y * 100.0) << StringView(u8" precise ") << (i64)input.precise << StringView(u8" momentum ") << (i64)input.momentum << StringView(u8" phase ") << (i64)input.phase << endL;
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
    traceInput(present ? StringView(u8"pointer present") : StringView(u8"pointer absent"), 0, 0);

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

void UiImpl::init(const UiOptions& options) {
    int width = (int)px(options.width);
    int height = (int)px(options.height);

    appId << StringView(u8"im-") << name;
    title << StringView(u8"im ") << name;

    plt::WindowOptions made;

    made.appId = StringView(appId);
    made.title = StringView(title);
    made.width = (u32)width;
    made.height = (u32)height;
    made.input = this;
    made.events = this;
    made.frame = this;

    plt::Window& shown = *platform_->createWindow(*pool, made);
    plt::WindowInfo info = shown.info();

    clampWindowSize(info, width, height);

    if (width != (int)info.width || height != (int)info.height) {
        shown.requestResize((u32)width, (u32)height);
    }

    pooledGuard(*pool, [this] {
        dropTextures();
    });
    setupImGuiContext(*pool, scale);
    renderer = Renderer::create(*pool, *platform_, shown, options.renderer);

    window = &shown;
    wake = platform_->createLoopWake(*pool, *pool->make<CallFrame>(this));
    ImGui::GetIO().BackendPlatformUserData = this;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();

    pio.Platform_RequestFrameFn = [](ImGuiContext* ctx) {
        ((UiImpl*)ctx->IO.BackendPlatformUserData)->window->requestFrame();
    };
    pio.Platform_CreateFrameTimerFn = [](ImGuiContext* ctx) -> void* {
        UiImpl* ui = (UiImpl*)ctx->IO.BackendPlatformUserData;

        return ui->pool->make<CallFrame>(ui);
    };
    pio.Platform_ScheduleFrameTimerFn = [](void* timer, float seconds) {
        ((CallFrame*)timer)->schedule(seconds);
    };
    pio.Platform_CancelFrameTimerFn = [](void* timer) {
        ((CallFrame*)timer)->cancel();
    };
    pio.Platform_SetMouseCursorFn = [](ImGuiContext* ctx, ImGuiMouseCursor cursor) {
        UiImpl* ui = (UiImpl*)ctx->IO.BackendPlatformUserData;
        plt::PointerIcon wanted = pointerIcon(cursor);

        if (wanted != ui->icon) {
            ui->icon = wanted;
            ui->window->requestPointerIcon(wanted);
        }
    };
}

void UiImpl::requestFrame() {
    wake->signal();
}

plt::Platform* UiImpl::platform() {
    return platform_;
}

RenderImage* UiImpl::bindImage(ObjPool& owner, u32 width, u32 height, const void* data, size_t size, size_t stride, Runable& retired) {
    return renderer->bind(owner, width, height, data, size, stride, retired);
}

RenderShader* UiImpl::compileKernel(ObjPool& owner, const CompiledShader& compiled, u32 tile, const ShaderOptions& options) {
    return renderer->compileKernel(owner, compiled, tile, options);
}

RenderImage* UiImpl::shadeImage(ObjPool& owner, ShaderFactory& factory, u32 width, u32 height, const void* data, size_t size, bool hdr, Runable& retired) {
    return renderer->shade(owner, factory, width, height, data, size, hdr, retired);
}

bool UiImpl::next(UiEvent& event) {
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
        platform_->scheduler()->current()->park();
        parked = false;
    }
}

void UiImpl::requestFullscreen(bool on) {
    window->requestFullscreen(on);
}

void UiImpl::requestResize(u32 width, u32 height) {
    if (!window || !width || !height || width > 0x7fffffffu || height > 0x7fffffffu) {
        fail(StringView(u8"invalid window resize"));
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
        fail(StringView(StringBuilder() << StringView(u8"more than ") << (i64)renderer->maxTextures() << StringView(u8" textures at once")));
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

    if (texture->Status == ImTextureStatus_WantCreate) {
        texture->SetStatus(ImTextureStatus_Destroyed);
    } else {
        texture->SetStatus(ImTextureStatus_WantDestroy);
        texture->UnusedFrames = 0;
    }

    window->requestFrame();
}

u32 UiImpl::maxTextureSide() {
    return renderer->maxTextureSide();
}

bool UiImpl::drawErrorPanel(StringView message) {
    return drawToolErrorPanel(name, scale, message) != 0;
}

void UiImpl::trace(StringView what) {
#ifdef IM_FOR_TESTS
    sysO << StringView(u8"im ") << name << StringView(u8": ") << what << endL;
#else
    (void)what;
#endif
}

void UiImpl::timing(StringView line) {
    if (traceFrames) {
        sysE << line << endL;
    }
}

void UiImpl::run() {
    try {
        body->run();
        result = 0;
    } catch (...) {
        error = Buffer(Exception::current());
    }

    finished = true;
    platform_->stop();
}

bool UiImpl::frame(const plt::WindowInfo& info) {
    if (info.iconified) {
        return false;
    }

    u64 began = monotonicNowUs();
    u64 gap = frameBegan ? began - frameBegan : 0;

    frameBegan = began;
    if (!renderer->beginFrame(info.width, info.height)) {
        return false;
    }

    if (info.width != presentedWidth || info.height != presentedHeight) {
        presentedWidth = info.width;
        presentedHeight = info.height;
        trace(StringView(StringBuilder() << StringView(u8"presenting ") << (i64)info.width << StringView(u8"x") << (i64)info.height));
    }

    u64 begun = monotonicNowUs();

    beginInputFrame();
    ImGui::NewFrame();

    u64 opened = monotonicNowUs();
    int action = drawFrame();
    u64 drew = monotonicNowUs();

    ImGui::Render();

    u64 rendered = monotonicNowUs();
    bool presented = renderer->endFrame(ImGui::GetDrawData());

    if (traceFrames) {
        StringBuilder text;

        text << StringView(u8"im frame ") << (i64)frameCount++ << StringView(u8": gap ") << MS{gap};
        text << StringView(u8" begin ") << MS{begun - began};
        text << StringView(u8" new ") << MS{opened - begun};
        text << StringView(u8" tool ") << MS{drew - opened};
        text << StringView(u8" render ") << MS{rendered - drew};
        text << StringView(u8" end ") << MS{monotonicNowUs() - rendered};

        if (!presented) {
            text << StringView(u8" unpresented");
        }

        sysE << StringView(text) << endL;
    }

    if (action != 0) {
        platform_->stop();
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
    trace(StringView(u8"closed"));

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
            window->requestFrame();
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

Ui* Ui::create(ObjPool& pool, StringView name, const UiOptions& options) {
    UiImpl& ui = *pool.make<UiImpl>();

    ui.pool = &pool;
    ui.name = pool.intern(name);
    ui.scale = scaleFromEnv();
    ui.traceFrames = getenv("IM_TRACE_FRAMES") != nullptr;
    ui.platform_ = plt::Platform::create(pool);
    ui.init(options);
    return &ui;
}

int UiImpl::run(Runable& body_) {
    body = &body_;
    fiber = platform_->scheduler()->create(*pool, *this, toolStack);

    if (!finished) {
        platform_->run();
    }

    if (!finished && window) {
        gone = true;
        fiber->wake();
    }

    if (!error.empty()) {
        raiseError(StringView(error));
    }

    return result;
}
