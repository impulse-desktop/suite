#include "screenshot.h"

#include "ui.h"
#include "util.h"
#include "color.h"
#include "frame.h"
#include "pooled.h"
#include "renderer.h"
#include "imgui_plt.h"
#include "chaos_monkey.h"

#include <std/sys/fd.h>
#include <std/ios/sys.h>
#include <std/alg/defer.h>
#include <std/sys/throw.h>
#include <std/sys/types.h>
#include <std/ios/out_fd.h>
#include <std/lib/vector.h>
#include <std/ios/fs_utils.h>

#include <png.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <imgui.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <jxl/encode.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <jxl/color_encoding.h>

using namespace stl;

namespace {
    struct Image {
        Buffer file;
        u32 w = 0, h = 0;
        const u8* px = nullptr;
        int dmaFd = -1;
        SharedImage* native = nullptr;
        bool dmabuf = false;
        OutputColorState color;
        Buffer rgb16;

        bool shared() const;
    };

    constexpr u32 kMagic = 0x31574d49u;

    void readTexture(RenderImage& texture, int x0, int y0, int x1, int y1, Image& out) {
        ImagePixels pixels;
        texture.read(x0, y0, x1, y1, pixels);
        out.w = pixels.width;
        out.h = pixels.height;
        out.file.xchg(pixels.rgba);
        out.rgb16.xchg(pixels.rgb16);
        out.px = (const u8*)out.file.data();
    }

    void loadImage(ObjPool& pool, StringView path, Image& img) {
        bool inherited = path == "fd:3"_sv;
        Buffer p(inherited ? "/proc/self/fd/3"_sv : path);

        if (const char* color = getenv("IM_SHOT_COLOR")) {
            StringView value(color), hs, rest;

            if (value.split(':', hs, rest)) {
                StringView whiteString = rest;
                StringView minString, peakString, fallString, head, tail;
                bool volume = false;

                if (rest.split(':', head, tail)) {
                    whiteString = head;
                    volume = tail.split(':', minString, tail) && tail.split(':', peakString, fallString);
                }

                double white = parseFloat(whiteString);

                img.color = hs == "1"_sv ? OutputColorState::hdr10(white) : OutputColorState::sdr();

                if (volume) {
                    img.color.displayMinNits = parseFloat(minString);
                    img.color.displayPeakNits = parseFloat(peakString);
                    img.color.displayMaxFallNits = parseFloat(fallString);
                }
            }
        }

        if (const char* spec = getenv("IM_SHOT_DMABUF")) {
            img.dmaFd = inherited ? fcntl(3, F_DUPFD_CLOEXEC, 0) : open(p.cStr(), O_RDONLY | O_CLOEXEC);

            if (img.dmaFd < 0) {
                fail(sv(StringBuilder() << "cannot take the shared screenshot fd: "_sv << StringView(strerror(errno))));
            }

            img.native = SharedImage::create(pool, StringView(spec), img.dmaFd);
            img.w = img.native->width;
            img.h = img.native->height;
            img.dmabuf = true;

            return;
        }

        readFileContent(p, img.file);

        if (img.file.used() < 12) {
            fail("not an imway screenshot (too small)"_sv);
        }

        const u32* h = (const u32*)img.file.data();

        if (h[0] != kMagic || !h[1] || !h[2]) {
            fail("not an imway screenshot (bad header)"_sv);
        }

        img.w = h[1];
        img.h = h[2];

        if (img.file.used() < 12 + (size_t)img.w * img.h * 4) {
            fail("truncated screenshot"_sv);
        }

        img.px = (const u8*)img.file.data() + 12;
    }

    void mkdirs(StringView path) {
        Buffer b(path);
        char* s = b.cStr();

        for (char* p = s + 1; *p; p++) {
            if (*p == '/') {
                *p = 0;
                mkdir(s, 0755);
                *p = '/';
            }
        }

        mkdir(s, 0755);
    }

    void pngWrite(png_structp png, png_bytep data, png_size_t len) {
        ((Buffer*)png_get_io_ptr(png))->append(data, (size_t)len);
    }

    double pqToNits(double value) {
        constexpr double m1 = 2610.0 / 16384.0;
        constexpr double m2 = 2523.0 / 32.0;
        constexpr double c1 = 3424.0 / 4096.0;
        constexpr double c2 = 2413.0 / 128.0;
        constexpr double c3 = 2392.0 / 128.0;
        double p = pow(fmax(value, 0.0), 1.0 / m2);

        return pow(fmax(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1) * 10000.0;
    }

    u8 linearToSrgb8(double value) {
        value = fmax(0.0, fmin(1.0, value));
        double encoded = value <= .0031308 ? value * 12.92 : 1.055 * pow(value, 1.0 / 2.4) - .055;

        return (u8)lround(encoded * 255.0);
    }

    void encodePng(ChaosMonkey& chaos, const Image& img, int x0, int y0, int x1, int y1, Buffer& out) {
        png_structp png = chaos.encoderAlloc(true) ? png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr) : nullptr;
        png_infop info = png && chaos.encoderAlloc(true) ? png_create_info_struct(png) : nullptr;

        if (!png || !info || setjmp(png_jmpbuf(png))) {
            if (png) {
                png_destroy_write_struct(&png, info ? &info : nullptr);
            }

            fail("png encode failed"_sv);
        }

        png_set_write_fn(png, &out, pngWrite, nullptr);
        png_set_IHDR(png, info, (u32)(x1 - x0), (u32)(y1 - y0), 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_set_sRGB(png, info, PNG_sRGB_INTENT_PERCEPTUAL);
        png_write_info(png, info);

        if (!img.color.hdr) {
            for (int y = y0; y < y1; y++) {
                png_write_row(png, (png_bytep)(img.px + ((size_t)y * img.w + x0) * 4));
            }
        } else {
            Buffer row;
            row.zero((size_t)(x1 - x0) * 4);
            OutputMapping mapping = sdrMapping();

            for (int y = y0; y < y1; y++) {
                u8* dst = (u8*)row.mutData();

                for (int x = x0; x < x1; x++) {
                    size_t source = ((size_t)y * img.w + x);
                    ColorRgb pq;

                    if (img.rgb16.length()) {
                        const u16* src = (const u16*)img.rgb16.data() + source * 3;

                        pq = {(double)src[0] / 65535.0, (double)src[1] / 65535.0, (double)src[2] / 65535.0};
                    } else {
                        const u8* src = img.px + source * 4;

                        pq = {(double)src[0] / 255.0, (double)src[1] / 255.0, (double)src[2] / 255.0};
                    }

                    ColorRgb nits{pqToNits(pq.r), pqToNits(pq.g), pqToNits(pq.b)};
                    ColorRgb mapped = mapping.toTarget.apply(mapOutputNits(mapping, nits));
                    size_t at = (size_t)(x - x0) * 4;

                    dst[at + 0] = linearToSrgb8(mapped.r / 203.0);
                    dst[at + 1] = linearToSrgb8(mapped.g / 203.0);
                    dst[at + 2] = linearToSrgb8(mapped.b / 203.0);
                    dst[at + 3] = 255;
                }

                png_write_row(png, (png_bytep)dst);
            }
        }

        png_write_end(png, nullptr);
        png_destroy_write_struct(&png, &info);
    }

    void saveFile(const Buffer& data, StringView file) {
        ScopedFD fd(open(Buffer(file).cStr(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));

        if (fd.get() < 0) {
            fail(sv(StringBuilder() << "cannot write "_sv << file));
        }

        FDRegular out(fd);

        out.write(data.data(), data.length());
        out.flush();
    }

    void encodeJxlPixels(ChaosMonkey& chaos, const Image& img, const u16* pixels, u32 w, u32 h, Buffer& out) {
        JxlEncoder* enc = chaos.encoderAlloc(true) ? JxlEncoderCreate(nullptr) : nullptr;

        if (!enc) {
            fail("jxl encoder allocation failed"_sv);
        }

        JxlBasicInfo info;

        JxlEncoderInitBasicInfo(&info);
        info.xsize = w;
        info.ysize = h;
        info.bits_per_sample = 16;
        info.num_color_channels = 3;
        info.uses_original_profile = JXL_TRUE;

        if (img.color.hdr) {
            info.intensity_target = (float)img.color.displayPeakNits;
            info.min_nits = (float)img.color.displayMinNits;
            info.relative_to_max_display = JXL_FALSE;
            info.linear_below = (float)img.color.displayMinNits;
        }

        JxlColorEncoding color{};

        if (img.color.hdr) {
            color.color_space = JXL_COLOR_SPACE_RGB;
            color.white_point = JXL_WHITE_POINT_D65;
            color.primaries = JXL_PRIMARIES_2100;
            color.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
            color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
        } else {
            JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
        }

        JxlEncoderFrameSettings* frame = chaos.encoderAlloc(true) ? JxlEncoderFrameSettingsCreate(enc, nullptr) : nullptr;
        JxlPixelFormat format{3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
        size_t bytes = (size_t)w * h * 3 * sizeof(u16);
        bool lossless = !getenv("IM_SHOT_LOSSLESS") || StringView(getenv("IM_SHOT_LOSSLESS")) != "0"_sv;
        bool frameConfigured = false;

        if (frame) {
            if (lossless) {
                frameConfigured = JxlEncoderSetFrameLossless(frame, JXL_TRUE) == JXL_ENC_SUCCESS;
            } else {
                double quality = 90.;

                if (const char* value = getenv("IM_SHOT_QUALITY")) {
                    quality = strtod(value, nullptr);
                }

                quality = quality < 1. ? 1. : quality > 100. ? 100. : quality;
                float distance = quality >= 100. ? 0.f : (float)(.1 + (100. - quality) * .15);

                frameConfigured = JxlEncoderSetFrameDistance(frame, distance) == JXL_ENC_SUCCESS;
            }
        }

        bool ok = JxlEncoderSetBasicInfo(enc, &info) == JXL_ENC_SUCCESS && JxlEncoderSetColorEncoding(enc, &color) == JXL_ENC_SUCCESS && frameConfigured && JxlEncoderAddImageFrame(frame, &format, pixels, bytes) == JXL_ENC_SUCCESS;

        if (!ok) {
            JxlEncoderDestroy(enc);
            fail("jxl encode setup failed"_sv);
        }

        JxlEncoderCloseInput(enc);
        out.reset();

        for (;;) {
            unsigned char chunk[64 * 1024];
            unsigned char* next = chunk;
            size_t available = sizeof(chunk);
            JxlEncoderStatus status = JxlEncoderProcessOutput(enc, &next, &available);

            out.append(chunk, sizeof(chunk) - available);

            if (!chaos.encoderOutput(status == JXL_ENC_SUCCESS || status == JXL_ENC_NEED_MORE_OUTPUT)) {
                JxlEncoderDestroy(enc);
                fail("jxl encode failed"_sv);
            }
            if (status == JXL_ENC_SUCCESS) {
                break;
            }
        }

        JxlEncoderDestroy(enc);
    }

    void encodeJxlSelection(ChaosMonkey& chaos, const Image& img, RenderImage& tex, int x0, int y0, int x1, int y1, Buffer& out) {
        Image selected;

        if (img.shared()) {
            readTexture(tex, x0, y0, x1, y1, selected);
        } else {
            selected.w = (u32)(x1 - x0);
            selected.h = (u32)(y1 - y0);
            selected.rgb16.zero((size_t)selected.w * selected.h * 3 * sizeof(u16));
            u16* dst = (u16*)selected.rgb16.mutData();

            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    const u8* src = img.px + ((size_t)y * img.w + x) * 4;
                    size_t at = ((size_t)(y - y0) * selected.w + (x - x0)) * 3;

                    dst[at + 0] = (u16)(src[0] * 257);
                    dst[at + 1] = (u16)(src[1] * 257);
                    dst[at + 2] = (u16)(src[2] * 257);
                }
            }
        }

        selected.color = img.color;
        encodeJxlPixels(chaos, selected, (const u16*)selected.rgb16.data(), selected.w, selected.h, out);
    }

    Buffer destPath() {
        Buffer dir;
        StringBuilder builder((Buffer&&)dir);
        const char* configured = getenv("IM_SHOT_DIR");
        const char* base = getenv("XDG_PICTURES_DIR");

        if (configured && *configured) {
            builder << StringView(configured);
        } else if (base && *base) {
            builder << StringView(base);
        } else {
            const char* home = getenv("HOME");

            builder << StringView(home ? home : ".") << "/Pictures"_sv;
        }

        if (!configured || !*configured) {
            builder << "/screenshots"_sv;
        }

        builder.xchg(dir);
        mkdirs(sv(dir));

        time_t t = time(nullptr);
        struct tm tm;

        localtime_r(&t, &tm);

        char stamp[256];
        const char* name = getenv("IM_SHOT_NAME");

        if (!name || !*name) {
            name = "imway-%Y%m%d-%H%M%S";
        }

        if (!strftime(stamp, sizeof(stamp), name, &tm)) {
            fail("screenshot filename is too long"_sv);
        }

        StringView extension = getenv("IM_SHOT_FORMAT") && StringView(getenv("IM_SHOT_FORMAT")) == "png"_sv ? ".png"_sv : ".jxl"_sv;

        return Buffer(sv(StringBuilder() << sv(dir) << "/"_sv << StringView(stamp) << extension));
    }

    void encodeSelection(ChaosMonkey& chaos, const Image& img, RenderImage& tex, int x0, int y0, int x1, int y1, Buffer& png) {
        if (!img.shared()) {
            encodePng(chaos, img, x0, y0, x1, y1, png);

            return;
        }

        Image pixels;

        readTexture(tex, x0, y0, x1, y1, pixels);
        pixels.color = img.color;
        encodePng(chaos, pixels, 0, 0, (int)pixels.w, (int)pixels.h, png);
    }

    struct Crop {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool dragging = false;
        float dragOx = 0, dragOy = 0;

        bool empty() const;
        void clear();
    };

    constexpr int kInitialZoom = 50;

    struct Viewer {
        int zoom = kInitialZoom;
        Crop crop;
    };

    constexpr int kZoomMin = 10, kZoomMax = 400, kZoomStep = 10;

    void initialWindowSize(const Image& img, const ImGuiStyle& style, float scale, int& w, int& h) {
        float zoom = (float)kInitialZoom / 100.f;

        w = (int)ceilf(scaledPx(200_d, scale) + style.ItemSpacing.x + img.w * zoom);
        h = (int)ceilf(img.h * zoom);

        int minH = (int)scaledPx(220_d, scale);

        if (h < minH) {
            h = minH;
        }
    }

    void traceView(StringView what, const Viewer& v) {
#ifdef IM_FOR_TESTS
        sysO << "im screenshot: "_sv << what << " zoom "_sv << v.zoom << endL;
#else
        (void)what;
        (void)v;
#endif
    }

    void applyZoom(Viewer& v, int delta) {
        int z = (int)clampf((float)(v.zoom + delta), (float)kZoomMin, (float)kZoomMax);

        if (z != v.zoom) {
            v.zoom = z;
            v.crop.clear();
            traceView("zoomed"_sv, v);
        }
    }

    void resetView(Viewer& v) {
        v.zoom = kInitialZoom;
        v.crop.clear();
        traceView("reset"_sv, v);
    }

    void drawPanel(Viewer& v, int& result, bool& reset) {
        Crop& crop = v.crop;

        ImGui::TextUnformatted("Zoom (view only)");
        ImGui::SetNextItemWidth(-FLT_MIN);

        int z = v.zoom;

        if (ImGui::SliderInt("##zoom", &z, kZoomMin, kZoomMax, "%d%%")) {
            applyZoom(v, z - v.zoom);
        }

        ImGui::Spacing();

        float avail = ImGui::GetContentRegionAvail().x;
        float bw = (avail - ImGui::GetStyle().ItemSpacing.x) / 2.f;

        if (ImGui::Button("Save", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            result = 1;
        }

        ImGui::SameLine();

        if (ImGui::Button("Reset", ImVec2(bw, 0))) {
            resetView(v);
            reset = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            result = -1;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (crop.empty()) {
            ImGui::TextDisabled("whole frame");
        } else {
            StringBuilder text;

            text << "selection "_sv << (i64)(crop.x1 - crop.x0 + 0.5f) << " x "_sv << (i64)(crop.y1 - crop.y0 + 0.5f);
            ImGui::TextUnformatted(text.cStr());
        }

        ImGui::TextDisabled("drag: select");
        ImGui::TextDisabled("middle-drag: pan");
        ImGui::TextDisabled("scroll / +-: zoom, 0: reset");
    }

    void drawCanvas(const Image& img, RenderImage& tex, Viewer& v, bool reset) {
        Crop& crop = v.crop;

        if (reset) {
            ImGui::SetScrollX(0.f);
            ImGui::SetScrollY(0.f);
        }

        float scale = (float)v.zoom / 100.f;
        ImVec2 content((float)img.w * scale, (float)img.h * scale);
        ImVec2 origin = ImGui::GetCursorScreenPos();

        ImGui::InvisibleButton("img", content, ImGuiButtonFlags_MouseButtonLeft);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        tex.draw(*dl, origin, ImVec2(origin.x + content.x, origin.y + content.y));

        ImVec2 mouse = ImGui::GetIO().MousePos;
        auto toScreen = [&](float px, float py) {
            return ImVec2(origin.x + px * scale, origin.y + py * scale);
        };
        auto toImg = [&](ImVec2 s) {
            return ImVec2(clampf((s.x - origin.x) / scale, 0.f, (float)img.w), clampf((s.y - origin.y) / scale, 0.f, (float)img.h));
        };

        if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;

            ImGui::SetScrollX(ImGui::GetScrollX() - d.x);
            ImGui::SetScrollY(ImGui::GetScrollY() - d.y);
            crop.clear();
            traceView("panned"_sv, v);
        }

        float wheel = ImGui::GetIO().MouseWheel;

        if (wheel != 0.f && ImGui::IsWindowHovered()) {
            applyZoom(v, wheel > 0.f ? kZoomStep : -kZoomStep);
        }

        if (ImGui::IsItemActivated()) {
            ImVec2 p = toImg(mouse);

            crop.dragOx = p.x;
            crop.dragOy = p.y;
            crop.dragging = true;
        }

        if (crop.dragging) {
            ImVec2 p = toImg(mouse);

            crop.x0 = crop.dragOx < p.x ? crop.dragOx : p.x;
            crop.y0 = crop.dragOy < p.y ? crop.dragOy : p.y;
            crop.x1 = crop.dragOx > p.x ? crop.dragOx : p.x;
            crop.y1 = crop.dragOy > p.y ? crop.dragOy : p.y;

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                crop.dragging = false;
            }
        }

        if (!crop.empty()) {
            ImVec2 s0 = toScreen(crop.x0, crop.y0);
            ImVec2 s1 = toScreen(crop.x1, crop.y1);
            ImVec2 hi(origin.x + content.x, origin.y + content.y);
            ImU32 dim = IM_COL32(0, 0, 0, 140);

            dl->AddRectFilled(origin, ImVec2(hi.x, s0.y), dim);
            dl->AddRectFilled(ImVec2(origin.x, s1.y), hi, dim);
            dl->AddRectFilled(ImVec2(origin.x, s0.y), s0, dim);
            dl->AddRectFilled(ImVec2(s1.x, s0.y), ImVec2(hi.x, s1.y), dim);
            dl->AddRect(s0, s1, IM_COL32(255, 255, 255, 230), 0, 0, 1.5f);
        }
    }

    int drawUi(float scale, plt::Window& window, const Image& img, RenderImage& tex, Viewer& v) {
        ImGuiViewport* vp = ImGui::GetMainViewport();

        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);

        int result = 0;
        bool reset = false;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        ImGui::Begin("##shot", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
        const float panelW = scaledPx(200_d, scale);

        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
            applyZoom(v, kZoomStep);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
            applyZoom(v, -kZoomStep);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) {
            resetView(v);
            reset = true;
        }

        ImGui::BeginChild("panel", ImVec2(panelW, 0), ImGuiChildFlags_Borders);
        drawPanel(v, result, reset);

        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("canvas", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawCanvas(img, tex, v, reset);

        ImGui::EndChild();

        ImGui::End();
        ImGui::PopStyleVar();

        if (reset) {
            int w, h;

            initialWindowSize(img, ImGui::GetStyle(), scale, w, h);
            clampWindowSize(window.info(), w, h);
            window.requestResize((u32)w, (u32)h);
        }

        return result;
    }

    struct ScreenshotUi final: UiFrame {
        float scale = 1.f;
        plt::Window* window = nullptr;
        const Image* img = nullptr;
        RenderImage* tex = nullptr;
        Viewer* view = nullptr;
        const Buffer* error = nullptr;

        int frame() override;
    };

    void cropRegion(const Image& img, const Crop& c, int& x0, int& y0, int& x1, int& y1) {
        x0 = (int)(clampf(c.x0, 0, (float)img.w) + 0.5f);
        y0 = (int)(clampf(c.y0, 0, (float)img.h) + 0.5f);
        x1 = (int)(clampf(c.x1, 0, (float)img.w) + 0.5f);
        y1 = (int)(clampf(c.y1, 0, (float)img.h) + 0.5f);

        if (x1 - x0 < 1 || y1 - y0 < 1) {
            x0 = 0;
            y0 = 0;
            x1 = (int)img.w;
            y1 = (int)img.h;
        }
    }
}

bool Crop::empty() const {
    return x1 - x0 < 1 || y1 - y0 < 1;
}

void Crop::clear() {
    x0 = y0 = x1 = y1 = 0;
    dragging = false;
}

bool Image::shared() const {
    return dmabuf;
}

int ScreenshotUi::frame() {
    return error->empty() ? drawUi(scale, *window, *img, *tex, *view) : drawErrorPanel("screenshot"_sv, scale, sv(*error));
}

int mainScreenshot(StringView path) {
    float scale = scaleFromEnv();
    bool traceFrames = getenv("IM_TRACE_FRAMES") != nullptr;

    FrameDriver driver;
    ScreenshotUi ui;
    Viewer view;
    ObjPool::Ref shot = ObjPool::fromMemory();
    Image img;
    Buffer errText;
    bool loaded = false;

    STD_DEFER {
        if (img.dmaFd >= 0) {
            close(img.dmaFd);
        }
    };

    try {
        loadImage(*shot, path, img);
        loaded = true;
    } catch (...) {
        errText = Buffer(Exception::current());
    }

    RenderImage* tex = nullptr;
    ChaosMonkey& chaos = *ChaosMonkey::create(*shot);

    plt::Platform& platform = *plt::Platform::create(*shot);
    ImGuiPlt& imgui = *ImGuiPlt::create(*shot, scale, traceFrames);

    int winW, winH;

    if (loaded) {
        initialWindowSize(img, scaledStyle(scale), scale, winW, winH);
    } else {
        winW = (int)scaledPx(480_d, scale);
        winH = (int)scaledPx(180_d, scale);
    }

    plt::WindowOptions options;

    options.appId = "im-screenshot"_sv;
    options.title = "im screenshot"_sv;
    options.width = (u32)winW;
    options.height = (u32)winH;
    options.input = imgui.sink();
    options.events = &driver;
    options.frame = &driver;

    plt::Window& window = *platform.createWindow(*shot, options);

    plt::WindowInfo made = window.info();
    int wantW = winW, wantH = winH;

    clampWindowSize(made, wantW, wantH);

    if (wantW != (int)made.width || wantH != (int)made.height) {
        window.requestResize((u32)wantW, (u32)wantH);
    }

    RendererOptions wants;
    wants.hdr = loaded && img.color.hdr;
    wants.sdrWhiteNits = (float)img.color.sdrWhiteNits;
    wants.shared = img.native;
    setupImGuiContext(*shot, scale);
    Renderer& renderer = *Renderer::create(*shot, window, wants);
    traceTool("screenshot"_sv, wants.hdr ? "surface HDR10 PQ"_sv : "surface sRGB"_sv);
    if (loaded) {
        tex = img.shared() ? renderer.import(*shot, *img.native, img.color.hdr) : renderer.upload(*shot, img.w, img.h, img.px, img.color.hdr);
    }

    ui.scale = scale;
    ui.window = &window;
    ui.img = &img;
    ui.tex = tex;
    ui.view = &view;
    ui.error = &errText;
    driver.platform = &platform;
    driver.window = &window;
    driver.imgui = &imgui;
    driver.renderer = &renderer;
    driver.tool = "screenshot"_sv;
    driver.traceFrames = traceFrames;
    driver.ui = &ui;

    int action = 0;
    StringView configuredAction(getenv("IM_SHOT_ACTION") ? getenv("IM_SHOT_ACTION") : "editor");

    auto report = [&] {
        if (!errText.empty()) {
            sysE << "im screenshot: "_sv << sv(errText) << endL;
        }
    };

    if (loaded && configuredAction == "save"_sv) {
        action = 1;
    } else {
        report();
        action = runUi(driver);
    }

    if (loaded && action == 1) {
        try {
            int x0, y0, x1, y1;

            cropRegion(img, view.crop, x0, y0, x1, y1);

            Buffer encoded;
            bool png = getenv("IM_SHOT_FORMAT") && StringView(getenv("IM_SHOT_FORMAT")) == "png"_sv;

            if (png) {
                encodeSelection(chaos, img, *tex, x0, y0, x1, y1, encoded);
            } else {
                encodeJxlSelection(chaos, img, *tex, x0, y0, x1, y1, encoded);
            }

            Buffer dest = destPath();

            saveFile(encoded, sv(dest));
            sysO << "im screenshot: saved "_sv << sv(dest) << endL;
        } catch (...) {
            errText = Buffer(Exception::current());
        }

        if (!errText.empty()) {
            report();
            runUi(driver);
        }
    }

    return 0;
}
