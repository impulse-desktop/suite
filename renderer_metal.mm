#include "renderer_metal.h"

#include "error.h"
#include "pooled.h"
#include "renderer.h"

#include <std/dbg/insist.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <string.h>
#include <unistd.h>
#include <plt/poller.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <plt/loop_wake.h>
#include <imgui_impl_metal.h>

#import <Metal/Metal.h>
#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h>

@interface ImMetalDisplayTarget: NSObject <CAMetalDisplayLinkDelegate>
@property(nonatomic, assign) void* owner;
@end

using namespace stl;

namespace {
    constexpr u32 drawables = 3;
    constexpr u32 maxTextureSize = 16384;

    struct MetalRenderer;

    struct PollMetal final: public plt::TimerCallback {
        MetalRenderer* renderer;
        explicit PollMetal(MetalRenderer* renderer);
        void ready() override;
    };

    struct SurfaceImage final: SharedImage {
        IOSurfaceRef surface = nullptr;
        MTLPixelFormat format = MTLPixelFormatInvalid;
        PixelLayout layout = PixelLayout::Rgba8;
        ~SurfaceImage() noexcept;
    };

    struct MetalImage final: RenderImage {
        MetalRenderer* renderer = nullptr;
        id<MTLTexture> texture = nil;
        PixelLayout layout = PixelLayout::Rgba8;
        bool hdr = false;
        const void* source = nullptr;
        size_t sourceStride = 0;
        size_t bufferStride = 0;
        id<MTLBuffer> buffer = nil;
        id<MTLCommandBuffer> lastUse = nil;
        Runable* retired = nullptr;
        bool hostImported = false;
        bool dirty = false;

        ~MetalImage() noexcept;
        void prepare() override;
        void draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) override;
        void read(int x0, int y0, int x1, int y1, ImagePixels& out) override;
    };

    struct ImageDraw {
        MetalImage* image;
        ImVec2 lo;
        ImVec2 hi;
    };

    struct ImageUse {
        MetalImage* image;
        u64 serial;
    };

    struct MetalRenderer final: Renderer {
        plt::Window* host = nullptr;
        plt::LoopWake* wake = nullptr;
        CAMetalDisplayLink* displayLink = nil;
        ImMetalDisplayTarget* target = nil;
        Vector<MetalImage*> drawn;
        Vector<ImageUse> uses;
        NSMutableArray<id<MTLCommandBuffer>>* flights = nil;
        u64 submitted = 0;
        u64 completed = 0;
        bool waiting = false;
        CAMetalLayer* layer = nil;
        NSWindow* window = nil;
        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;
        id<CAMetalDrawable> drawable = nil;
        MTLRenderPassDescriptor* pass = nil;
        id<MTLCommandBuffer> last = nil;
        id<MTLRenderCommandEncoder> encoder = nil;
        id<MTLRenderPipelineState> uiPipeline = nil;
        id<MTLRenderPipelineState> imagePipeline = nil;
        bool hdr = false;
        float sdrWhiteNits = 203.f;
        ImDrawData* drawing = nullptr;

        bool beginFrame(u32 width, u32 height) override;
        void drawableReady(id<CAMetalDrawable> value);
        void poll();
        bool endFrame(ImDrawData* draw) override;
        u32 maxTextureSide() override;
        u32 maxTextures() override;
        RenderImage* upload(ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) override;
        RenderImage* import(ObjPool& pool, SharedImage& source, bool hdr) override;

        RenderImage* bind(ObjPool& pool, u32 width, u32 height, const void* data, size_t size, size_t stride, Runable& retired) override;
        void setupHdr();
        void drawHdr(ImDrawData& draw);
        bool clip(const ImDrawCmd& command);
        void drawImage(const ImageDraw& image, const ImDrawCmd& command);
    };

    static void checkCommand(id<MTLCommandBuffer> command) {
        if (command.status == MTLCommandBufferStatusError) {
            fail(StringView(StringBuilder() << StringView(u8"metal command failed: ") << StringView(command.error.localizedDescription.UTF8String)));
        }
    }

    static void drawImage(const ImDrawList*, const ImDrawCmd* command) {
        const ImageDraw& draw = *(const ImageDraw*)command->UserCallbackData;
        draw.image->renderer->drawImage(draw, *command);
    }

    static constexpr const char* hdrShaders = R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex {
    float2 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
    float4 color [[attribute(2)]];
};
struct Fragment {
    float4 position [[position]];
    float2 uv;
    float4 color;
};
vertex Fragment uiVertex(Vertex v [[stage_in]], constant float4& transform [[buffer(1)]]) {
    return {float4(v.position * transform.xy + transform.zw, 0, 1), v.uv, v.color};
}
float3 srgbToLinear(float3 c) {
    return select(pow((c + 0.055) / 1.055, float3(2.4)), c / 12.92, c <= 0.04045);
}
float3 bt709ToBt2020(float3 c) {
    return float3x3(float3(0.627404, 0.069097, 0.016391), float3(0.329283, 0.919540, 0.088013), float3(0.043313, 0.011362, 0.895595)) * c;
}
fragment float4 uiFragment(Fragment v [[stage_in]], texture2d<float> image [[texture(0)]]) {
    constexpr sampler sampled(filter::linear, address::clamp_to_edge);
    float4 pixel = image.sample(sampled, v.uv);
    return float4(bt709ToBt2020(srgbToLinear(pixel.rgb) * srgbToLinear(v.color.rgb)), pixel.a * v.color.a);
}
vertex Fragment imageVertex(uint index [[vertex_id]], constant float4& transform [[buffer(0)]], constant float4& rect [[buffer(1)]]) {
    const float2 corners[] = {float2(0,0), float2(1,0), float2(1,1), float2(0,0), float2(1,1), float2(0,1)};
    float2 uv = corners[index];
    return {float4(mix(rect.xy, rect.zw, uv) * transform.xy + transform.zw, 0, 1), uv, float4(1)};
}
fragment float4 imageFragment(Fragment v [[stage_in]], texture2d<float> image [[texture(0)]], constant float& white [[buffer(0)]]) {
    constexpr sampler sampled(filter::linear, address::clamp_to_edge);
    float3 p = pow(max(image.sample(sampled, v.uv).rgb, 0.0), float3(32.0 / 2523.0));
    float3 nits = pow(max(p - 3424.0 / 4096.0, 0.0) / (2413.0 / 128.0 - 2392.0 / 128.0 * p), float3(16384.0 / 2610.0)) * 10000.0;
    return float4(nits / white, 1);
}
)metal";
}

SurfaceImage::~SurfaceImage() noexcept {
    if (surface) {
        CFRelease(surface);
    }
}

SharedImage* createMetalSharedImage(ObjPool& pool, StringView description, intptr_t handle) {
    if (!description.empty() || !handle) {
        fail(StringView(u8"a shared Metal image needs an IOSurface handle"));
    }
    SurfaceImage* source = pool.make<SurfaceImage>();
    source->surface = (IOSurfaceRef)handle;
    CFRetain(source->surface);
    size_t width = IOSurfaceGetWidth(source->surface);
    size_t height = IOSurfaceGetHeight(source->surface);
    if (width > maxTextureSize || height > maxTextureSize || IOSurfaceGetPlaneCount(source->surface) > 1) {
        fail(StringView(u8"unsupported IOSurface dimensions"));
    }
    source->width = (u32)width;
    source->height = (u32)height;
    checkImageSize(source->width, source->height, maxTextureSize);
    switch (IOSurfaceGetPixelFormat(source->surface)) {
        case 'RGBA': {
            source->format = MTLPixelFormatRGBA8Unorm;
            source->layout = PixelLayout::Rgba8;
            break;
        }
        case 'BGRA': {
            source->format = MTLPixelFormatBGRA8Unorm;
            source->layout = PixelLayout::Bgra8;
            break;
        }
        case 'l10r': {
            source->format = MTLPixelFormatBGR10A2Unorm;
            source->layout = PixelLayout::Bgr10A2;
            break;
        }
        default: {
            fail(StringView(u8"unsupported IOSurface pixel format"));
        }
    }
    return source;
}

MetalImage::~MetalImage() noexcept {
    for (const MetalImage* image : renderer->drawn) {
        STD_INSIST(image != this);
    }
    for (const ImageUse& use : renderer->uses) {
        STD_INSIST(use.image != this);
    }
    [lastUse waitUntilCompleted];
}

void MetalImage::prepare() {
    if (source) {
        if (!hostImported) {
            for (size_t y = 0; y < texture.height; y++) {
                memcpy((u8*)buffer.contents + y * bufferStride, (const u8*)source + y * sourceStride, texture.width * 4);
            }
        }
        dirty = true;
    }
}

void MetalImage::draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) {
    renderer->drawn.pushBack(this);
    if (hdr) {
        ImageDraw draw{this, lo, hi};
        list.AddCallback(drawImage, &draw, sizeof(draw));
        list.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    } else {
        list.AddImage(ImTextureRef((ImTextureID)(__bridge void*)texture), lo, hi);
    }
}

void MetalImage::read(int x0, int y0, int x1, int y1, ImagePixels& out) {
    checkImageRegion((u32)texture.width, (u32)texture.height, x0, y0, x1, y1);
    @autoreleasepool {
        u32 width = (u32)(x1 - x0);
        u32 height = (u32)(y1 - y0);
        size_t stride = ((size_t)width * 4 + 255) & ~(size_t)255;
        id<MTLBuffer> buffer = [renderer->device newBufferWithLength:stride * height options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [renderer->queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!buffer || !command || !blit) {
            fail(StringView(u8"cannot allocate Metal readback"));
        }
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(width, height, 1) toBuffer:buffer destinationOffset:0 destinationBytesPerRow:stride destinationBytesPerImage:stride * height];
        [blit endEncoding];
        [command commit];
        [command waitUntilCompleted];
        checkCommand(command);
        checkCommand(renderer->last);
        unpackPixels(buffer.contents, width, height, stride, layout, out);
    }
}

RenderImage* MetalRenderer::upload(ObjPool& pool, u32 width, u32 height, const void* rgba, bool imageHdr) {
    checkImageSize(width, height, maxTextureSide());
    if (!rgba || (imageHdr && !hdr)) {
        fail(StringView(u8"invalid renderer image source"));
    }
    @autoreleasepool {
        MetalImage* image = pool.make<MetalImage>();
        image->renderer = this;
        image->hdr = imageHdr;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModeManaged;
        descriptor.usage = MTLTextureUsageShaderRead;
        image->texture = [device newTextureWithDescriptor:descriptor];
        if (!image->texture) {
            fail(StringView(u8"cannot allocate Metal image"));
        }
        [image->texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:rgba bytesPerRow:(size_t)width * 4];
        return image;
    }
}

RenderImage* MetalRenderer::import(ObjPool& pool, SharedImage& shared, bool imageHdr) {
    SurfaceImage& source = static_cast<SurfaceImage&>(shared);
    if (imageHdr && !hdr) {
        fail(StringView(u8"HDR image needs an HDR renderer"));
    }
    @autoreleasepool {
        MetalImage* image = pool.make<MetalImage>();
        image->renderer = this;
        image->hdr = imageHdr;
        image->layout = source.layout;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.format width:source.width height:source.height mipmapped:NO];
        descriptor.storageMode = MTLStorageModeManaged;
        descriptor.usage = MTLTextureUsageShaderRead;
        image->texture = [device newTextureWithDescriptor:descriptor iosurface:source.surface plane:0];
        if (!image->texture) {
            fail(StringView(u8"cannot import IOSurface into Metal"));
        }
        return image;
    }
}

void MetalRenderer::setupHdr() {
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:hdrShaders] options:nil error:&error];
    if (!library) {
        fail(StringView(StringBuilder() << StringView(u8"Metal HDR shaders: ") << StringView(error.localizedDescription.UTF8String)));
    }
    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [library newFunctionWithName:@"uiVertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"uiFragment"];
    MTLVertexDescriptor* vertex = [[MTLVertexDescriptor alloc] init];
    vertex.attributes[0].format = MTLVertexFormatFloat2;
    vertex.attributes[0].offset = offsetof(ImDrawVert, pos);
    vertex.attributes[1].format = MTLVertexFormatFloat2;
    vertex.attributes[1].offset = offsetof(ImDrawVert, uv);
    vertex.attributes[2].format = MTLVertexFormatUChar4Normalized;
    vertex.attributes[2].offset = offsetof(ImDrawVert, col);
    vertex.layouts[0].stride = sizeof(ImDrawVert);
    descriptor.vertexDescriptor = vertex;
    MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
    color.pixelFormat = MTLPixelFormatRGBA16Float;
    color.blendingEnabled = YES;
    color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    uiPipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!uiPipeline) {
        fail(StringView(StringBuilder() << StringView(u8"Metal HDR UI pipeline: ") << StringView(error.localizedDescription.UTF8String)));
    }
    descriptor.vertexDescriptor = nil;
    descriptor.vertexFunction = [library newFunctionWithName:@"imageVertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"imageFragment"];
    color.blendingEnabled = NO;
    imagePipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!imagePipeline) {
        fail(StringView(StringBuilder() << StringView(u8"Metal HDR image pipeline: ") << StringView(error.localizedDescription.UTF8String)));
    }
}

bool MetalRenderer::clip(const ImDrawCmd& command) {
    ImVec2 offset = drawing->DisplayPos;
    ImVec2 scale = drawing->FramebufferScale;
    float x0 = fmaxf(0, (command.ClipRect.x - offset.x) * scale.x);
    float y0 = fmaxf(0, (command.ClipRect.y - offset.y) * scale.y);
    float x1 = fminf((float)drawable.texture.width, (command.ClipRect.z - offset.x) * scale.x);
    float y1 = fminf((float)drawable.texture.height, (command.ClipRect.w - offset.y) * scale.y);
    if (x1 <= x0 || y1 <= y0 || (NSUInteger)x1 <= (NSUInteger)x0 || (NSUInteger)y1 <= (NSUInteger)y0) {
        return false;
    }
    [encoder setScissorRect:MTLScissorRect{(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)x1 - (NSUInteger)x0, (NSUInteger)y1 - (NSUInteger)y0}];
    return true;
}

void MetalRenderer::drawImage(const ImageDraw& image, const ImDrawCmd& command) {
    if (!clip(command)) {
        return;
    }
    float transform[] = {2.f / drawing->DisplaySize.x, -2.f / drawing->DisplaySize.y, -1.f - 2.f * drawing->DisplayPos.x / drawing->DisplaySize.x, 1.f + 2.f * drawing->DisplayPos.y / drawing->DisplaySize.y};
    float rect[] = {image.lo.x, image.lo.y, image.hi.x, image.hi.y};
    [encoder setRenderPipelineState:imagePipeline];
    [encoder setVertexBytes:transform length:sizeof(transform) atIndex:0];
    [encoder setVertexBytes:rect length:sizeof(rect) atIndex:1];
    [encoder setFragmentBytes:&sdrWhiteNits length:sizeof(sdrWhiteNits) atIndex:0];
    [encoder setFragmentTexture:image.image->texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

void MetalRenderer::drawHdr(ImDrawData& draw) {
    if (draw.Textures) {
        for (ImTextureData* texture : *draw.Textures) {
            if (texture->Status != ImTextureStatus_OK) {
                ImGui_ImplMetal_UpdateTexture(texture);
            }
        }
    }
    if (draw.DisplaySize.x <= 0 || draw.DisplaySize.y <= 0) {
        return;
    }
    drawing = &draw;
    [encoder setCullMode:MTLCullModeNone];
    [encoder setViewport:MTLViewport{0, 0, (double)drawable.texture.width, (double)drawable.texture.height, 0, 1}];
    float transform[] = {2.f / draw.DisplaySize.x, -2.f / draw.DisplaySize.y, -1.f - 2.f * draw.DisplayPos.x / draw.DisplaySize.x, 1.f + 2.f * draw.DisplayPos.y / draw.DisplaySize.y};
    for (const ImDrawList* list : draw.CmdLists) {
        id<MTLBuffer> vertices = nil;
        id<MTLBuffer> indices = nil;
        if (list->VtxBuffer.Size && list->IdxBuffer.Size) {
            vertices = [device newBufferWithBytes:list->VtxBuffer.Data length:(size_t)list->VtxBuffer.Size * sizeof(ImDrawVert) options:MTLResourceStorageModeShared];
            indices = [device newBufferWithBytes:list->IdxBuffer.Data length:(size_t)list->IdxBuffer.Size * sizeof(ImDrawIdx) options:MTLResourceStorageModeShared];
            if (!vertices || !indices) {
                fail(StringView(u8"cannot allocate Metal HDR draw buffers"));
            }
        }
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback) {
                if (command.UserCallback != ImDrawCallback_ResetRenderState) {
                    command.UserCallback(list, &command);
                }
                continue;
            }
            if (!command.ElemCount || !clip(command)) {
                continue;
            }
            [encoder setRenderPipelineState:uiPipeline];
            [encoder setVertexBuffer:vertices offset:command.VtxOffset * sizeof(ImDrawVert) atIndex:0];
            [encoder setVertexBytes:transform length:sizeof(transform) atIndex:1];
            [encoder setFragmentTexture:(__bridge id<MTLTexture>)(void*)(uintptr_t)command.GetTexID() atIndex:0];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:command.ElemCount indexType:sizeof(ImDrawIdx) == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32 indexBuffer:indices indexBufferOffset:command.IdxOffset * sizeof(ImDrawIdx)];
        }
    }
    drawing = nullptr;
}

@implementation ImMetalDisplayTarget

- (void)metalDisplayLink:(CAMetalDisplayLink*)link needsUpdate:(CAMetalDisplayLinkUpdate*)update {
    (void)link;
    ((MetalRenderer*)self.owner)->drawableReady(update.drawable);
}

@end

PollMetal::PollMetal(MetalRenderer* value)
    : renderer(value)
{
}

void PollMetal::ready() {
    renderer->poll();
}

void MetalRenderer::poll() {
    while (flights.count > 0 && flights[0].status >= MTLCommandBufferStatusCompleted) {
        checkCommand(flights[0]);
        [flights removeObjectAtIndex:0];
        completed++;
    }
    Vector<ImageUse> done;
    size_t kept = 0;
    for (const ImageUse& use : uses) {
        if (use.serial <= completed) {
            done.pushBack(use);
        } else {
            uses.mut(kept++) = use;
        }
    }
    while (uses.length() > kept) {
        uses.popBack();
    }
    for (const ImageUse& use : done) {
        use.image->retired->run();
    }
}

void MetalRenderer::drawableReady(id<CAMetalDrawable> value) {
    if (waiting) {
        drawable = value;
        waiting = false;
        displayLink.paused = YES;
        host->requestFrame();
    }
}

bool MetalRenderer::beginFrame(u32 width, u32 height) {
    @autoreleasepool {
        poll();
        checkCommand(last);
        layer.drawableSize = CGSizeMake(width, height);
        if (drawable && (drawable.texture.width != width || drawable.texture.height != height)) {
            drawable = nil;
        }
        if (!drawable) {
            waiting = true;
            displayLink.paused = NO;
            return false;
        }
        pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.1, 1.0);
        ImGui_ImplMetal_NewFrame(pass);
        return true;
    }
}

bool MetalRenderer::endFrame(ImDrawData* draw) {
    @autoreleasepool {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        if (!command) {
            fail(StringView(u8"cannot begin Metal command buffer"));
        }
        for (MetalImage* image : drawn) {
            if (image->dirty) {
                id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
                if (!blit) {
                    fail(StringView(u8"cannot begin Metal image upload"));
                }
                [blit copyFromBuffer:image->buffer sourceOffset:0 sourceBytesPerRow:image->bufferStride sourceBytesPerImage:image->bufferStride * image->texture.height sourceSize:MTLSizeMake(image->texture.width, image->texture.height, 1) toTexture:image->texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding];
                image->dirty = false;
            }
            image->lastUse = command;
            if (image->retired) {
                uses.pushBack(ImageUse{image, submitted + 1});
            }
        }
        drawn.clear();
        encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!encoder) {
            fail(StringView(u8"cannot begin Metal frame"));
        }
        if (hdr) {
            drawHdr(*draw);
        } else {
            ImGui_ImplMetal_RenderDrawData(draw, command, encoder);
        }
        [encoder endEncoding];
        encoder = nil;
        plt::LoopWake* completed = wake;
        [command addCompletedHandler:^(id<MTLCommandBuffer>) {
          completed->signal();
        }];
        [command presentDrawable:drawable];
        [command commit];
        [flights addObject:command];
        submitted++;
        last = command;
        drawable = nil;
        pass = nil;
    }
    return true;
}

RenderImage* MetalRenderer::bind(ObjPool& pool, u32 width, u32 height, const void* data, size_t size, size_t stride, Runable& retired) {
    checkImageSize(width, height, maxTextureSide());
    if (!data || stride < (size_t)width * 4 || stride > size / height) {
        fail(StringView(u8"invalid bound image buffer"));
    }
    @autoreleasepool {
        MetalImage* image = pool.make<MetalImage>();
        image->renderer = this;
        image->source = data;
        image->sourceStride = stride;
        image->retired = &retired;
        size_t page = (size_t)getpagesize();
        if ((uintptr_t)data % page == 0 && size % page == 0 && stride % 256 == 0) {
            image->buffer = [device newBufferWithBytesNoCopy:const_cast<void*>(data) length:size options:MTLResourceStorageModeShared deallocator:nil];
            image->hostImported = image->buffer != nil;
        }
        image->bufferStride = image->hostImported ? stride : ((size_t)width * 4 + 255) & ~(size_t)255;
        if (!image->hostImported) {
            image->buffer = [device newBufferWithLength:image->bufferStride * height options:MTLResourceStorageModeShared];
        }
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageShaderRead;
        image->texture = [device newTextureWithDescriptor:descriptor];
        if (!image->buffer || !image->texture) {
            fail(StringView(u8"cannot allocate bound Metal image"));
        }
        return image;
    }
}

u32 MetalRenderer::maxTextureSide() {
    return maxTextureSize;
}

u32 MetalRenderer::maxTextures() {
    return 0xffffffffu;
}

Renderer* createMetalRenderer(ObjPool& pool, plt::Platform& platform, plt::Window& window, const RendererOptions& options) {
    plt::RenderContext context = window.renderContext();
    MetalRenderer* renderer = pool.make<MetalRenderer>();
    renderer->host = &window;
    renderer->layer = (__bridge CAMetalLayer*)context.connection;
    renderer->window = (__bridge NSWindow*)context.window;
    renderer->device = MTLCreateSystemDefaultDevice();
    renderer->hdr = options.hdr;
    renderer->sdrWhiteNits = options.sdrWhiteNits;
    if (renderer->device == nil) {
        fail(StringView(u8"no metal device"));
    }
    renderer->queue = [renderer->device newCommandQueue];
    if (!renderer->queue) {
        fail(StringView(u8"cannot create Metal queue"));
    }
    CAMetalLayer* layer = renderer->layer;
    layer.device = renderer->device;
    layer.pixelFormat = options.hdr ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.maximumDrawableCount = drawables;
    layer.presentsWithTransaction = NO;
    layer.wantsExtendedDynamicRangeContent = options.hdr;
    renderer->wake = platform.createLoopWake(pool, *pool.make<PollMetal>(renderer));
    renderer->flights = [NSMutableArray new];
    renderer->target = [ImMetalDisplayTarget new];
    renderer->target.owner = renderer;
    renderer->displayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
    renderer->displayLink.delegate = renderer->target;
    renderer->displayLink.paused = YES;
    [renderer->displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    pooledGuard(pool, [renderer] {
        [renderer->displayLink invalidate];
    });
    CGColorSpaceRef color = CGColorSpaceCreateWithName(options.hdr ? kCGColorSpaceExtendedLinearITUR_2020 : kCGColorSpaceSRGB);
    if (!color) {
        fail(StringView(u8"cannot create Metal color space"));
    }
    layer.colorspace = color;
    CGColorSpaceRelease(color);
    if (options.hdr) {
        renderer->setupHdr();
    }
    if (!ImGui_ImplMetal_Init(renderer->device)) {
        fail(StringView(u8"cannot initialize Metal ImGui backend"));
    }
    pooledGuard(pool, [renderer] {
        [renderer->last waitUntilCompleted];
        ImGui_ImplMetal_Shutdown();
    });
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Renderer_TextureMaxWidth = (int)maxTextureSize;
    pio.Renderer_TextureMaxHeight = (int)maxTextureSize;
    return renderer;
}
