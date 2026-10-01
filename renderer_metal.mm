#include "util.h"
#include "frame.h"
#include "pooled.h"

#include <std/mem/obj_pool.h>

#include <imgui.h>
#include <plt/window.h>
#include <imgui_impl_metal.h>

#import <Metal/Metal.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

using namespace stl;

namespace {
    constexpr u32 drawables = 3;
    constexpr u32 maxTextureSize = 16384;

    struct MetalRenderer final: Renderer {
        CAMetalLayer* layer = nil;
        NSWindow* window = nil;
        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;
        id<CAMetalDrawable> drawable = nil;
        MTLRenderPassDescriptor* pass = nil;
        id<MTLCommandBuffer> last = nil;

        void beginFrame(u32 width, u32 height) override;
        bool endFrame(ImDrawData* draw) override;
        u32 maxTextureSide() override;
        u32 maxTextures() override;
    };
}

void MetalRenderer::beginFrame(u32 width, u32 height) {
    @autoreleasepool {
        layer.drawableSize = CGSizeMake(width, height);
        layer.presentsWithTransaction = window.inLiveResize;
        drawable = [layer nextDrawable];
        pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.1, 1.0);
        ImGui_ImplMetal_NewFrame(pass);
    }
}

bool MetalRenderer::endFrame(ImDrawData* draw) {
    @autoreleasepool {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];

        ImGui_ImplMetal_RenderDrawData(draw, command, encoder);
        [encoder endEncoding];

        if (layer.presentsWithTransaction) {
            [command commit];
            [command waitUntilScheduled];
            [drawable present];
        } else {
            [command presentDrawable:drawable];
            [command commit];
        }

        last = command;
        drawable = nil;
        pass = nil;
    }

    return true;
}

u32 MetalRenderer::maxTextureSide() {
    return maxTextureSize;
}

u32 MetalRenderer::maxTextures() {
    return 0xffffffffu;
}

Renderer* Renderer::create(ObjPool& pool, plt::Window& window) {
    plt::RenderContext context = window.renderContext();
    MetalRenderer* renderer = pool.make<MetalRenderer>();

    renderer->layer = (__bridge CAMetalLayer*)context.connection;
    renderer->window = (__bridge NSWindow*)context.window;
    renderer->device = MTLCreateSystemDefaultDevice();

    if (renderer->device == nil) {
        fail("no metal device"_sv);
    }

    renderer->queue = [renderer->device newCommandQueue];

    CAMetalLayer* layer = renderer->layer;

    layer.device = renderer->device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.maximumDrawableCount = drawables;
    layer.allowsNextDrawableTimeout = NO;
    layer.presentsWithTransaction = NO;

    ImGui_ImplMetal_Init(renderer->device);
    pooledGuard(pool, [renderer] {
        [renderer->last waitUntilCompleted];
        ImGui_ImplMetal_Shutdown();
    });

    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();

    pio.Renderer_TextureMaxWidth = (int)maxTextureSize;
    pio.Renderer_TextureMaxHeight = (int)maxTextureSize;

    return renderer;
}
