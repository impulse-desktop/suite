#include "renderer.h"

#include "error.h"

#include <math.h>
#include <string.h>

#if defined(__APPLE__)
    #include "renderer_metal.h"
#else
    #include "renderer_vulkan.h"
#endif

using namespace stl;

void checkImageSize(u32 width, u32 height, u32 limit) {
    if (!width || !height || width > limit || height > limit || (u64)width * height > (1u << 28)) {
        fail(StringView(u8"invalid renderer image size"));
    }
}

void checkImageRegion(u32 width, u32 height, int x0, int y0, int x1, int y1) {
    if (x0 < 0 || y0 < 0 || x1 <= x0 || y1 <= y0 || (u32)x1 > width || (u32)y1 > height) {
        fail(StringView(u8"invalid renderer image region"));
    }
}

namespace {
    static float halfToFloat(u16 half) {
        u32 exponent = (half >> 10) & 31;
        u32 mantissa = half & 1023;
        float value = exponent == 0 ? ldexpf((float)mantissa, -24) : exponent == 31 ? (mantissa ? NAN : INFINITY) : ldexpf((float)(mantissa | 1024), (int)exponent - 25);

        return half >> 15 ? -value : value;
    }

    static void unpackHalves(const void* data, u32 width, u32 height, size_t stride, ImagePixels& out) {
        out.rgbaf.zero((size_t)width * height * 4 * sizeof(float));
        u8* rgba = (u8*)out.rgba.mutData();
        u16* rgb = (u16*)out.rgb16.mutData();
        float* rgbaf = (float*)out.rgbaf.mutData();

        for (u32 y = 0; y < height; y++) {
            for (u32 x = 0; x < width; x++) {
                size_t at = (size_t)y * width + x;
                for (u32 c = 0; c < 4; c++) {
                    u16 half;
                    memcpy(&half, (const u8*)data + y * stride + x * 8 + c * 2, 2);
                    float value = halfToFloat(half);
                    float clamped = value > 0.f ? (value < 1.f ? value : 1.f) : 0.f;
                    rgbaf[at * 4 + c] = value;
                    rgba[at * 4 + c] = (u8)(clamped * 255.f + 0.5f);
                    if (c < 3) {
                        rgb[at * 3 + c] = (u16)(clamped * 65535.f + 0.5f);
                    }
                }
            }
        }
    }
}

void unpackPixels(const void* data, u32 width, u32 height, size_t stride, PixelLayout layout, ImagePixels& out) {
    out.width = width;
    out.height = height;
    out.rgba.zero((size_t)width * height * 4);
    out.rgb16.zero((size_t)width * height * 3 * sizeof(u16));
    out.rgbaf.reset();
    if (layout == PixelLayout::Rgba16f) {
        unpackHalves(data, width, height, stride, out);
        return;
    }
    u8* rgba = (u8*)out.rgba.mutData();
    u16* rgb = (u16*)out.rgb16.mutData();
    bool packed = layout == PixelLayout::Rgb10A2 || layout == PixelLayout::Bgr10A2;
    bool reverse = layout == PixelLayout::Bgra8 || layout == PixelLayout::Bgr10A2;
    u32 bits = packed ? 10 : 8;
    u32 mask = (1u << bits) - 1;

    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u32 pixel;
            memcpy(&pixel, (const u8*)data + y * stride + x * 4, 4);
            size_t at = (size_t)y * width + x;
            for (u32 c = 0; c < 3; c++) {
                u32 channel = reverse ? 2 - c : c;
                u32 value = (pixel >> (channel * bits)) & mask;
                rgba[at * 4 + c] = (u8)((value * 255 + mask / 2) / mask);
                rgb[at * 3 + c] = (u16)((value * 65535 + mask / 2) / mask);
            }
            rgba[at * 4 + 3] = packed ? (u8)((pixel >> 30) * 85) : (u8)(pixel >> 24);
        }
    }
}

Renderer* Renderer::create(stl::ObjPool& pool, plt::Platform& platform, plt::Window& window, const RendererOptions& options) {
    if (!(options.sdrWhiteNits > 0.f) || options.sdrWhiteNits > 10000.f) {
        fail(StringView(u8"invalid SDR white level"));
    }
#if defined(__APPLE__)
    return createMetalRenderer(pool, platform, window, options);
#else
    return createVulkanRenderer(pool, platform, window, options);
#endif
}

SharedImage* SharedImage::create(stl::ObjPool& pool, stl::StringView description, intptr_t handle) {
#if defined(__APPLE__)
    return createMetalSharedImage(pool, description, handle);
#else
    return createVulkanSharedImage(pool, description, handle);
#endif
}
