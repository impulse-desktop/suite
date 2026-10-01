#include "ui.h"
#include "util.h"
#include "renderer.h"

#include <std/alg/defer.h>
#include <std/mem/obj_pool.h>

#include <string.h>

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>

using namespace stl;

void checkMetalShared(ObjPool& pool, Ui& ui, bool hdr) {
    @autoreleasepool {
        NSDictionary* properties = @{
            (__bridge NSString*)kIOSurfaceWidth : @3,
            (__bridge NSString*)kIOSurfaceHeight : @2,
            (__bridge NSString*)kIOSurfaceBytesPerElement : @4,
            (__bridge NSString*)kIOSurfacePixelFormat : @(hdr ? 'l10r' : 'BGRA'),
        };
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        if (!surface) {
            fail("cannot create test IOSurface"_sv);
        }
        STD_DEFER {
            CFRelease(surface);
        };
        IOSurfaceLock(surface, 0, nullptr);
        size_t stride = IOSurfaceGetBytesPerRow(surface);
        unsigned char* bytes = (unsigned char*)IOSurfaceGetBaseAddress(surface);
        for (u32 y = 0; y < 2; y++) {
            for (u32 x = 0; x < 3; x++) {
                u32 pixel = hdr ? 0xfff80001u : 0xff112233u;
                memcpy(bytes + y * stride + x * 4, &pixel, 4);
            }
        }
        IOSurfaceUnlock(surface, 0, nullptr);
        SharedImage& shared = *SharedImage::create(pool, {}, (intptr_t)surface);
        RenderImage& image = *ui.importImage(pool, shared, hdr);
        ImagePixels pixels;
        image.read(1, 1, 3, 2, pixels);
        const u16 expected[] = {hdr ? (u16)65535 : (u16)4369, hdr ? (u16)32800 : (u16)8738, hdr ? (u16)64 : (u16)13107};
        if (pixels.width != 2 || pixels.height != 1 || memcmp(pixels.rgb16.data(), expected, sizeof(expected)) || memcmp((const char*)pixels.rgb16.data() + sizeof(expected), expected, sizeof(expected))) {
            fail("Metal IOSurface readback differs"_sv);
        }
    }
}
