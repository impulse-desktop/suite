#include "renderer.h"

#include "ui.h"
#include "util.h"

#include <std/ios/sys.h>
#include <std/mem/obj_pool.h>

#include <string.h>

using namespace stl;

#if defined(__APPLE__)
void checkMetalShared(ObjPool& pool, Ui& ui, bool hdr);
#endif

namespace {
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

    static int checkRenderer(ObjPool& pool, Ui& ui, int argc, char**) {
        UiOptions options{64_d, 64_d};
        options.renderer.hdr = argc > 1;
        ui.open(options);
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
        RenderImage& image = *ui.uploadImage(pool, 3, 2, source, options.renderer.hdr);
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
        checkMetalShared(pool, ui, options.renderer.hdr);
#endif
        UiEvent event;
        int frames = 0;
        while (ui.next(event)) {
            verify(event.kind == UiEvent::Kind::Frame);
            if (frames == 3) {
                break;
            }
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImDrawList& list = *ImGui::GetBackgroundDrawList();
            image.draw(list, viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));
            list.AddRectFilled(ImVec2(8, 8), ImVec2(32, 32), IM_COL32(255, 0, 0, 128));
            frames++;
        }
        image.read(0, 0, 3, 2, pixels);
        verify(!memcmp(pixels.rgba.data(), source, sizeof(source)));
        verify(frames == 3);
        sysO << "OK: renderer upload, crop readback, precision, bounds and drawing"_sv << endL;
        return 0;
    }
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
        return runTool("renderer-test"_sv, checkRenderer, argc, argv);
    } catch (...) {
        sysE << Exception::current() << endL;
        return 1;
    }
}
