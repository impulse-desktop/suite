#include "renderer.h"

#include "ui.h"
#include "error.h"

#include <std/ios/sys.h>
#include <std/sys/throw.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
    #include "renderer_metal.h"
#endif

using namespace stl;

namespace {
    static void verify(bool condition) {
        if (!condition) {
            fail(StringView(u8"renderer pixel check failed"));
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

    struct Retired final: public Runable {
        Ui* ui = nullptr;
        bool pending = false;
        void run() override;
    };

    static void checkBound(ObjPool& pool, Ui& ui) {
        // Aligned storage permits direct import; the second source forces
        // the producer-copy fallback. Refill only after automatic retirement.
        void* memory = pool.allocateOverAligned(65536, 65536);
        unsigned char fallback[33]{};
        unsigned char* sources[] = {(unsigned char*)memory, fallback + 1};
        size_t strides[] = {256, 16};
        Retired callbacks[2];
        RenderImage* images[2];
        for (int i = 0; i < 2; i++) {
            callbacks[i].ui = &ui;
            images[i] = ui.bindImage(pool, 3, 2, sources[i], i == 0 ? 65536 : 32, strides[i], callbacks[i]);
        }
        int iteration = 0;
        bool submitted = false;
        UiEvent event;
        ui.requestFrame();
        while (ui.next(event)) {
            verify(event.kind == UiEvent::Kind::Frame);
            if (callbacks[0].pending || callbacks[1].pending) {
                continue;
            }
            if (submitted) {
                for (int i = 0; i < 2; i++) {
                    ImagePixels pixels;
                    images[i]->read(0, 0, 3, 2, pixels);
                    for (int y = 0; y < 2; y++) {
                        verify(!memcmp((const unsigned char*)pixels.rgba.data() + y * 12, sources[i] + y * strides[i], 12));
                    }
                }
                if (++iteration == 12) {
                    break;
                }
            }
            for (int i = 0; i < 2; i++) {
                for (int y = 0; y < 2; y++) {
                    for (int x = 0; x < 3; x++) {
                        unsigned char* pixel = sources[i] + y * strides[i] + x * 4;
                        pixel[0] = (unsigned char)(iteration * 17 + x);
                        pixel[1] = (unsigned char)(i * 100 + y);
                        pixel[2] = 32;
                        pixel[3] = 255;
                    }
                }
                images[i]->prepare();
                callbacks[i].pending = true;
                ImDrawList& list = *ImGui::GetBackgroundDrawList();
                images[i]->draw(list, ImVec2(0, 0), ImVec2(16, 16));
                images[i]->draw(list, ImVec2(16, 0), ImVec2(32, 16));
            }
            submitted = true;
        }
        verify(iteration == 12);
        sysO << StringView(u8"OK: bound images, automatic retirement, reuse and pixel readback") << endL;
    }

    static void checkRenderer(ObjPool& pool, Ui& ui, bool hdr) {
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
        RenderImage& image = *ui.uploadImage(pool, 3, 2, source, hdr);
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
        checkMetalShared(pool, ui, hdr);
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
            ui.requestFrame();
        }
        image.read(0, 0, 3, 2, pixels);
        verify(!memcmp(pixels.rgba.data(), source, sizeof(source)));
        verify(frames == 3);
        sysO << StringView(u8"OK: renderer upload, crop readback, precision, bounds and drawing") << endL;
        checkBound(pool, ui);
    }
}

void Retired::run() {
    verify(pending);
    pending = false;
    ui->requestFrame();
}

int main(int argc, char** argv) {
    ObjPool::Ref pool = ObjPool::fromMemory();
    int result;
    try {
        checkPacked();
        if (argc == 2 && StringView(argv[1]) == StringView(u8"--pixels")) {
            sysO << StringView(u8"OK: pixel layouts and 10-bit precision") << endL;
            return 0;
        }
        if (argc > 2 || (argc == 2 && StringView(argv[1]) != StringView(u8"--hdr"))) {
            fail(StringView(u8"usage: renderer_test [--pixels|--hdr]"));
        }
        UiOptions options{64_d, 64_d};
        options.renderer.hdr = argc > 1;
        Ui& ui = *Ui::create(*pool, StringView(u8"renderer-test"), options);
        auto body = makeRunable([&] {
            checkRenderer(*pool, ui, options.renderer.hdr);
        });
        result = ui.run(body);
    } catch (...) {
        sysE << Exception::current() << endL;
        result = 1;
    }
    exit(result);
}
