#include "native.h"

#include <std/dbg/insist.h>
#include <std/mem/obj_pool.h>

#include <stdio.h>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Canvas final: public MetalCanvas {
        explicit Canvas(const RenderContext& context);
        bool paint(const WindowInfo& info, u32 color) override;
        void describe() override;

        CAMetalLayer* layer;
        NSWindow* window;
        id<MTLCommandQueue> queue;
    };
}

Canvas::Canvas(const RenderContext& context)
    : layer((__bridge CAMetalLayer*)context.connection)
    , window((__bridge NSWindow*)context.window)
{
    STD_INSIST(context.backend == RenderBackend::Cocoa);
    layer.device = MTLCreateSystemDefaultDevice();
    STD_INSIST(layer.device != nil);
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.presentsWithTransaction = YES;
    queue = [layer.device newCommandQueue];
    STD_INSIST(queue != nil);
}

bool Canvas::paint(const WindowInfo& info, u32 color) {
    @autoreleasepool {
        layer.drawableSize = CGSizeMake(info.width, info.height);
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (drawable == nil) {
            return false;
        }
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(
            ((color >> 16) & 255) / 255.0, ((color >> 8) & 255) / 255.0, (color & 255) / 255.0, 1);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        STD_INSIST(command.status == MTLCommandBufferStatusCompleted);
        [drawable present];
        return true;
    }
}

void Canvas::describe() {
    const NSRect frame = window.frame;
    const CGRect screen = CGDisplayBounds(CGMainDisplayID());
    printf("WINDOW %ld %.0f %.0f %.0f %.0f\n", (long)window.windowNumber,
        frame.origin.x, screen.size.height - NSMaxY(frame), frame.size.width, frame.size.height);
}

MetalCanvas* MetalCanvas::create(ObjPool& owner, const RenderContext& context) {
    return owner.make<Canvas>(context);
}
