#include "renderer.h"

#include "util.h"
#include "frame.h"
#include "imgui_plt.h"

#include <std/ios/sys.h>
#include <std/mem/obj_pool.h>

#include <string.h>
#include <plt/window.h>
#include <plt/platform.h>

using namespace stl;

#if defined(__APPLE__)
void checkMetalShared(ObjPool& pool, Renderer& renderer, bool hdr);
#endif

namespace {
    struct Probe final: UiFrame {
        RenderImage* image = nullptr;
        int frames = 0;
        int frame() override;
    };

    static void verify(bool condition) {
        if (!condition) {
            fail("renderer pixel check failed"_sv);
        }
    }

    static void checkPacked() {
        const u32 rgba[] = {0x7f332211, 0xffccbbaa};
        ImagePixels pixels;
        unpackPixels(rgba, 1, 2, 4, PixelLayout::Rgba8, pixels);
        const unsigned char expected[] = {0x11, 0x22, 0x33, 0x7f, 0xaa, 0xbb, 0xcc, 0xff};
        verify(pixels.rgba.length() == sizeof(expected) && !memcmp(pixels.rgba.data(), expected, sizeof(expected)));
        const u32 packed[] = {0xc0000000u | (1023u << 20) | (512u << 10) | 1u};
        unpackPixels(packed, 1, 1, 4, PixelLayout::Bgr10A2, pixels);
        const unsigned char expected10[] = {255, 128, 0, 255};
        const u16 expected16[] = {65535, 32800, 64};
        verify(!memcmp(pixels.rgba.data(), expected10, sizeof(expected10)));
        verify(!memcmp(pixels.rgb16.data(), expected16, sizeof(expected16)));
        unpackPixels(packed, 1, 1, 4, PixelLayout::Rgb10A2, pixels);
        const u16 reversed[] = {64, 32800, 65535};
        verify(!memcmp(pixels.rgb16.data(), reversed, sizeof(reversed)));
    }
}

int Probe::frame() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList& list = *ImGui::GetBackgroundDrawList();
    image->draw(list, viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));
    list.AddRectFilled(ImVec2(8, 8), ImVec2(32, 32), IM_COL32(255, 0, 0, 128));
    return ++frames == 3 ? -1 : 0;
}

int main(int argc, char** argv) {
    try {
        checkPacked();
        if (argc == 2 && StringView(argv[1]) == "--pixels"_sv) {
            sysO << "OK: pixel layouts and 10-bit precision"_sv << endL;
            return 0;
        }
        if (argc > 2 || (argc == 2 && StringView(argv[1]) != "--hdr"_sv)) {
            fail("usage: renderer_test [--pixels|--hdr]"_sv);
        }
        FrameDriver driver;
        Probe probe;
        ObjPool::Ref pool = ObjPool::fromMemory();
        plt::Platform& platform = *plt::Platform::create(*pool);
        ImGuiPlt& imgui = *ImGuiPlt::create(*pool, 1.f, false);
        plt::WindowOptions windowOptions;
        windowOptions.width = 64;
        windowOptions.height = 64;
        windowOptions.appId = "im-renderer-test"_sv;
        windowOptions.title = "Renderer test"_sv;
        windowOptions.input = imgui.sink();
        windowOptions.frame = &driver;
        windowOptions.events = &driver;
        plt::Window& window = *platform.createWindow(*pool, windowOptions);
        setupImGuiContext(*pool, 1.f);
        RendererOptions options;
        options.hdr = argc > 1;
        Renderer& renderer = *Renderer::create(*pool, window, options);
        const unsigned char source[] = {
            1,
            2,
            3,
            255,
            11,
            12,
            13,
            255,
            21,
            22,
            23,
            255,
            31,
            32,
            33,
            255,
            41,
            42,
            43,
            127,
            51,
            52,
            53,
            255,
        };
        RenderImage& image = *renderer.upload(*pool, 3, 2, source, options.hdr);
        ImagePixels pixels;
        image.read(0, 0, 3, 2, pixels);
        verify(pixels.width == 3 && pixels.height == 2 && pixels.rgba.length() == sizeof(source));
        verify(!memcmp(pixels.rgba.data(), source, sizeof(source)));
        image.read(1, 1, 3, 2, pixels);
        verify(pixels.width == 2 && pixels.height == 1);
        verify(!memcmp(pixels.rgba.data(), source + 16, 8));
        const u16 expected[] = {10537, 10794, 11051, 13107, 13364, 13621};
        verify(!memcmp(pixels.rgb16.data(), expected, sizeof(expected)));
        bool rejected = false;
        try {
            image.read(-1, 0, 1, 1, pixels);
        } catch (...) {
            rejected = true;
        }
        verify(rejected);
#if defined(__APPLE__)
        checkMetalShared(*pool, renderer, options.hdr);
#endif
        probe.image = &image;
        driver.platform = &platform;
        driver.window = &window;
        driver.imgui = &imgui;
        driver.renderer = &renderer;
        driver.ui = &probe;
        driver.tool = "renderer-test"_sv;
        runUi(driver);
        image.read(0, 0, 3, 2, pixels);
        verify(!memcmp(pixels.rgba.data(), source, sizeof(source)));
        verify(probe.frames == 3);
        sysO << "OK: renderer upload, crop readback, precision, bounds and drawing"_sv << endL;
        return 0;
    } catch (...) {
        sysE << Exception::current() << endL;
        return 1;
    }
}
