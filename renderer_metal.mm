#include "renderer_metal.h"

#include "error.h"
#include "pooled.h"
#include "shader.h"
#include "renderer.h"

#include <std/dbg/insist.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/thr/channel.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>

#include <math.h>
#include <string.h>
#include <unistd.h>
#include <plt/poller.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <plt/loop_wake.h>

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
    constexpr u32 composeTile = 24;
    constexpr u32 composeGroup = 8;
    constexpr u32 composeTextures = 1024;
    constexpr u32 composeNone = 0xffffffffu;
    constexpr u32 composeFill = 0x80000000u;

    enum : u32 {
        OpRect,
        OpTexture,
        OpTriangle,
        OpLayer,
    };

    enum : u32 {
        DecodeLinear = 0,
        DecodeSrgb = 1,
        DecodePq = 2,
        SourceWide = 4,
        Solid = 8,
    };

    constexpr u32 outputs = 4;

    struct Header {
        u32 base;
        u32 start;
        u32 count;
        u32 pad;
        float color[4];
    };

    struct Op {
        u32 kind;
        u32 index;
        u32 flags;
        u32 pad;
        i32 rect[4];
        float uv[4];
        float map[4];
        float color[4];
    };

    struct Triangle {
        i32 edges[3][4];
        float colors[3][4];
        float uv[3][2];
        u32 texture;
        u32 flags;
        i32 clip[4];
        i64 offset[3];
        i64 area;
    };

    static_assert(sizeof(Header) == 32);
    static_assert(sizeof(Op) == 80);
    static_assert(sizeof(Triangle) == 176);

    struct Push {
        alignas(8) i32 size[2];
        i32 video[2];
        u32 tilesX;
        u32 first;
        float white;
    };

    struct MetalTexture {
        id<MTLTexture> texture = nil;
        id<MTLCommandBuffer> lastUse = nil;
        u32 flags = DecodeLinear;
        bool opaque = false;
        u64 frame = 0;
        u32 slot = 0;
    };

    struct MetalRenderer;
    struct MetalImage;

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

    struct MetalShader final: RenderShader {
        id<MTLComputePipelineState> pipeline = nil;
    };

    struct MetalImage final: RenderImage {
        MetalRenderer* renderer = nullptr;
        MetalTexture texture;
        PixelLayout layout = PixelLayout::Rgba8;
        bool hdr = false;
        u32 width = 0;
        u32 height = 0;
        const void* source = nullptr;
        size_t sourceStride = 0;
        size_t sourceSize = 0;
        size_t bufferStride = 0;
        id<MTLBuffer> buffer = nil;
        ShaderFactory* factory = nullptr;
        id<MTLCommandBuffer> lastUse = nil;
        Runable* retired = nullptr;
        bool hostImported = false;
        bool dirty = false;

        ~MetalImage() noexcept;
        void prepare() override;
        void draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) override;
        void read(int x0, int y0, int x1, int y1, ImagePixels& out) override;
        void readShaded(int x0, int y0, int x1, int y1, ImagePixels& out);
    };

    struct Layer {
        MetalImage* image;
        float lo[2];
        float hi[2];
    };

    struct LayerDraw {
        MetalImage* image;
        float lo[2];
        float hi[2];
    };

    struct Flight {
        Vector<MetalImage*> images;

        explicit Flight(const Vector<MetalImage*>& drawn);
    };

    static void drawLayer(const ImDrawList*, const ImDrawCmd*) {
    }

    float srgbTable[256];

    struct Placement {
        MetalImage* image;
        i32 box[4];
    };

    struct Tiles {
        Vector<Header> headers;
        Vector<u32> list;
        Vector<Op> ops;
        Vector<Triangle> triangles;
        Vector<u32> tiles;
        Vector<u32> programs;
        Vector<MetalTexture*> textures;
        Vector<Placement> layers;
        Vector<u32> base;
        Vector<float> color;
        Vector<u32> head;
        Vector<u32> tail;
        Vector<u32> count;
        Vector<u32> layered;
        Vector<u32> nodes;
        u32 width = 0;
        u32 height = 0;
        u32 tilesX = 0;
        u32 tilesY = 0;
        bool wide = false;
        u64 frame = 0;
        id<MTLCommandBuffer> serial = nil;
        float scale[2] = {};
        float translate[2] = {};
        float half[2] = {};
        float fit[2] = {};
        float offset[2] = {};
        float zoom[2] = {};
        ImTextureID atlas = ImTextureID_Invalid;
        ImVec2 white = {};

        void compose(const ImDrawData* draw, const Vector<Layer>& underlays, u32 width, u32 height, bool wide, u64 frame, id<MTLCommandBuffer> serial, const float (&clear)[4]);
        void reset(u32 width, u32 height);
        i64 snap(float pos, int axis) const;
        void place(const Op& op, u32 index, const i32 (&box)[4], bool fills, bool opaque);
        void cover(u32 tile, const Op& op, u32 index, bool opaque);
        void append(u32 tile, u32 entry);
        u32 slot(MetalTexture* texture);
        u32 layer(MetalImage* image, const i32 (&box)[4]);
        void addLayer(MetalImage* image, const float (&lo)[2], const float (&hi)[2], const i32 (&clip)[4]);
        void addCommand(const ImDrawList& list, const ImDrawCmd& command);
        void finish();
    };

    struct MetalRenderer final: Renderer {
        plt::Window* host = nullptr;
        plt::LoopWake* wake = nullptr;
        CAMetalDisplayLink* displayLink = nil;
        ImMetalDisplayTarget* target = nil;
        Vector<MetalImage*> drawn;
        SmallObjAllocator* smallObjects = nullptr;
        Channel* landed = nullptr;
        bool waiting = false;
        CAMetalLayer* layer = nil;
        NSWindow* window = nil;
        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;
        id<CAMetalDrawable> drawable = nil;
        id<MTLCommandBuffer> last = nil;
        id<MTLLibrary> plainLibrary = nil;
        id<MTLLibrary> layeredLibrary = nil;
        id<MTLComputePipelineState> plain[outputs] = {};
        bool edr = false;
        bool wide = false;
        float sdrWhiteNits = 203.f;
        u64 frames = 0;
        Tiles tiles;

        bool beginFrame(u32 width, u32 height) override;
        void drawableReady(id<CAMetalDrawable> value);
        void poll();
        bool endFrame(ImDrawData* draw) override;
        u32 maxTextureSide() override;
        u32 maxTextures() override;
        RenderImage* upload(ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) override;
        RenderImage* import(ObjPool& pool, SharedImage& source, bool hdr) override;

        RenderImage* bind(ObjPool& pool, u32 width, u32 height, const void* data, size_t size, size_t stride, Runable& retired) override;
        RenderShader* compileKernel(ObjPool& pool, const void* code, size_t size, u32 tile, const ShaderOptions& options) override;
        RenderImage* shade(ObjPool& pool, ShaderFactory& factory, u32 width, u32 height, const void* data, size_t size, bool hdr, Runable& retired) override;
        id<MTLLibrary> library(NSString* source);
        id<MTLComputePipelineState> pipeline(id<MTLLibrary> library, ShaderOutput output, u32 side, id<MTLFunction> linked);
        void updateTextures(ImDrawData* draw);
        void encode(id<MTLCommandBuffer> command, id<MTLTexture> target, ShaderOutput output);
        void setMode(bool wide);
    };

    static void checkCommand(id<MTLCommandBuffer> command) {
        if (command.status == MTLCommandBufferStatusError) {
            fail(StringView(StringBuilder() << StringView(u8"metal command failed: ") << StringView(command.error.localizedDescription.UTF8String)));
        }
    }

    static constexpr const char* composeSource = R"metal(
#ifndef GROUP
#define GROUP 8
#endif

constant int OUTPUT [[function_constant(0)]];
constant bool WIDE [[function_constant(1)]];

constant int TILE = 24;
constant uint NONE = 0xffffffffu;
constant uint FILL = 0x80000000u;
constant uint OP_RECT = 0u;
constant uint OP_TEXTURE = 1u;
constant uint OP_TRIANGLE = 2u;
constant uint OP_LAYER = 3u;
constant uint DECODE = 3u;
constant uint DECODE_SRGB = 1u;
constant uint DECODE_PQ = 2u;
constant uint SOURCE_WIDE = 4u;
constant uint SOLID = 8u;

struct Header {
    uint base;
    uint start;
    uint count;
    uint pad;
    float4 color;
};

struct Op {
    uint kind;
    uint index;
    uint flags;
    uint pad;
    int4 rect;
    float4 uv;
    float4 map;
    float4 color;
};

struct Triangle {
    int4 edges[3];
    float4 colors[3];
    float2 uv[3];
    uint texture;
    uint flags;
    int4 clip;
    long offset[3];
    long area;
};

struct Frame {
    int2 size;
    int2 video;
    uint tilesX;
    uint first;
    float white;
};

struct Textures {
    array<texture2d<float>, 1024> items;
};

static float3 widen(float3 c) {
    return float3x3(float3(0.627404, 0.069097, 0.016391), float3(0.329283, 0.919540, 0.088013), float3(0.043313, 0.011362, 0.895595)) * c;
}

static float3 narrow(float3 c) {
    return float3x3(float3(1.660491, -0.124550, -0.018151), float3(-0.587641, 1.132900, -0.100579), float3(-0.072850, -0.008349, 1.118730)) * c;
}

static float3 toFrame(float3 c, bool wide) {
    if (WIDE) {
        return wide ? c : widen(c);
    }

    return wide ? clamp(narrow(c), 0.0, 1.0) : c;
}

static float3 srgbDecode(float3 c) {
    return select(pow((c + 0.055) / 1.055, float3(2.4)), c / 12.92, c <= 0.04045);
}

static float3 srgbEncode(float3 c) {
    return select(1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, c * 12.92, c <= 0.0031308);
}

static float3 pqDecode(float3 e) {
    float3 p = pow(max(e, 0.0), float3(32.0 / 2523.0));

    return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), float3(16384.0 / 2610.0)) * 10000.0;
}

static float noise(int2 pixel) {
    float2 at = float2(pixel) + 0.5;
    float inner = at.x * 0.06711056 + at.y * 0.00583715;

    return fract(52.9829189 * fract(inner)) - 0.5;
}

static float3 encode(float3 c, int2 pixel, constant Frame& frame) {
    if (OUTPUT == 0) {
        return srgbEncode(clamp(c, 0.0, 1.0)) + noise(pixel) / 255.0;
    }

    return c;
}

static float4 sampled(constant Textures& textures, uint index, uint flags, float2 at, constant Frame& frame) {
    constexpr sampler smooth(filter::linear, address::clamp_to_edge);
    float4 s = textures.items[index].sample(smooth, at, level(0.0));
    uint decode = flags & DECODE;

    if (decode == DECODE_SRGB) {
        s.rgb = srgbDecode(s.rgb);
    } else if (decode == DECODE_PQ) {
        s.rgb = pqDecode(s.rgb) / frame.white;
    }

    s.rgb = toFrame(s.rgb, (flags & SOURCE_WIDE) != 0u);

    return s;
}

static float4 over(float4 src, float4 acc) {
    return float4(src.rgb * src.a, src.a) + acc * (1.0 - src.a);
}

#ifdef LAYER
[[visible]] float4 layer(uint2 local, int2 origin, const device uint* words);
#endif

kernel void compose(device const Header* headers [[buffer(0)]], device const uint* list [[buffer(1)]], device const Op* ops [[buffer(2)]], device const Triangle* triangles [[buffer(3)]], device const uint* tiles [[buffer(4)]], constant Frame& frame [[buffer(5)]], constant Textures& textures [[buffer(6)]],
#ifdef LAYER
    const device uint* words [[buffer(7)]],
#endif
    texture2d<float, access::write> target [[texture(0)]], uint3 group [[threadgroup_position_in_grid]], uint3 inside [[thread_position_in_threadgroup]]) {
    const uint across = uint(TILE / GROUP);
    uint tile = tiles[frame.first + group.x];
    int2 origin = int2(int(tile % frame.tilesX), int(tile / frame.tilesX)) * TILE;
    int2 corner = origin + int2(int(group.y % across), int(group.y / across)) * GROUP;
    int2 local = int2(inside.xy);
    int2 pixel = corner + local;
    float2 centre = float2(pixel) + 0.5;
    Header h = headers[tile];
    float4 shown = float4(0.0);

#ifdef LAYER
    shown = layer(inside.xy, origin - frame.video, words);
#endif

    if (pixel.x >= frame.size.x || pixel.y >= frame.size.y) {
        return;
    }

    float4 acc = h.color;

    if (h.base != NONE) {
        Op b = ops[h.base];
        float4 src = shown;

        if (b.kind != OP_LAYER) {
            src = b.color * sampled(textures, b.index, b.flags, mix(b.uv.xy, b.uv.zw, (centre - b.map.xy) * b.map.zw), frame);
        }

        acc = h.color + float4(src.rgb * src.a, src.a) * (1.0 - h.color.a);
    }

    for (uint i = 0u; i < h.count; i++) {
        uint entry = list[h.start + i];
        Op op = ops[entry & ~FILL];

        if ((entry & FILL) != 0u) {
            acc = over(op.color, acc);
            continue;
        }

        if (op.kind == OP_TRIANGLE) {
            Triangle t = triangles[op.index];
            long cx = long(corner.x * 256 + 128);
            long cy = long(corner.y * 256 + 128);
            float area = float(t.area);
            float b[3];
            bool covered = pixel.x >= t.clip.x && pixel.y >= t.clip.y && pixel.x < t.clip.z && pixel.y < t.clip.w;

            for (int k = 0; k < 3; k++) {
                long start = long(t.edges[k].x) * cx + long(t.edges[k].y) * cy + t.offset[k];
                long need = -start;
                long whole = (need + (need > 0 ? 255l : 0l)) / 256l;
                int threshold = int(whole < -2147483647l ? -2147483647l : whole > 2147483647l ? 2147483647l : whole);
                int step = t.edges[k].x * local.x + t.edges[k].y * local.y;

                covered = covered && step >= threshold;
                b[k] = (float(start + long(t.edges[k].z)) + 256.0 * float(step)) / area;
            }

            if (!covered) {
                continue;
            }

            float b0 = 1.0 - b[1] - b[2];
            float4 src = b0 * t.colors[0] + b[1] * t.colors[1] + b[2] * t.colors[2];

            if ((t.flags & SOLID) == 0u) {
                src *= sampled(textures, t.texture, t.flags, b0 * t.uv[0] + b[1] * t.uv[1] + b[2] * t.uv[2], frame);
            }

            acc = over(src, acc);
            continue;
        }

        if (pixel.x < op.rect.x || pixel.y < op.rect.y || pixel.x >= op.rect.z || pixel.y >= op.rect.w) {
            continue;
        }

        if (op.kind == OP_RECT) {
            acc = over(op.color, acc);
        } else if (op.kind == OP_TEXTURE) {
            acc = over(op.color * sampled(textures, op.index, op.flags, mix(op.uv.xy, op.uv.zw, (centre - op.map.xy) * op.map.zw), frame), acc);
        } else if (op.kind == OP_LAYER) {
            acc = over(shown, acc);
        }
    }

    if (OUTPUT == 2) {
        target.write(acc.a > 0.0 ? float4(acc.rgb / acc.a, acc.a) : float4(0.0), uint2(pixel));
    } else {
        target.write(float4(encode(acc.rgb, pixel, frame), 1.0), uint2(pixel));
    }
}
)metal";

    static i64 nearest(float x) {
        volatile float magic = 12582912.f;

        return (i64)((x + magic) - magic);
    }

    static i32 firstPixel(i64 fixed) {
        i64 v = fixed - 128;

        return (i32)(v >= 0 ? (v + 255) / 256 : -((-v) / 256));
    }

    static void edge(const i64 (&a)[2], const i64 (&b)[2], i32 (&out)[4], i64& offset) {
        i64 dx = b[0] - a[0];
        i64 dy = b[1] - a[1];
        bool topLeft = (dy == 0 && dx > 0) || dy < 0;

        out[0] = (i32)-dy;
        out[1] = (i32)dx;
        out[2] = topLeft ? 0 : 1;
        out[3] = 0;
        offset = dy * a[0] - dx * a[1] - (topLeft ? 0 : 1);
    }

    static void linearColor(u32 col, bool wide, float (&out)[4]) {
        float r = srgbTable[col & 255u];
        float g = srgbTable[(col >> 8) & 255u];
        float b = srgbTable[(col >> 16) & 255u];

        if (wide) {
            out[0] = 0.627404f * r + 0.329283f * g + 0.043313f * b;
            out[1] = 0.069097f * r + 0.919540f * g + 0.011362f * b;
            out[2] = 0.016391f * r + 0.088013f * g + 0.895595f * b;
        } else {
            out[0] = r;
            out[1] = g;
            out[2] = b;
        }

        out[3] = (float)(col >> 24) / 255.f;
    }

    static bool opaquePixels(const u8* rgba, size_t pitch, u32 w, u32 h) {
        for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < w; x++) {
                if (rgba[(size_t)y * pitch + (size_t)x * 4 + 3] != 255) {
                    return false;
                }
            }
        }

        return true;
    }
}

void Tiles::reset(u32 w, u32 h) {
    width = w;
    height = h;
    tilesX = (w + composeTile - 1) / composeTile;
    tilesY = (h + composeTile - 1) / composeTile;

    u32 n = tilesX * tilesY;

    base.zero(n);
    head.zero(n);
    tail.zero(n);
    count.zero(n);
    layered.zero(n);
    color.zero((size_t)n * 4);
    memset(base.mutData(), 0xff, n * sizeof(u32));
    memset(head.mutData(), 0xff, n * sizeof(u32));
    memset(tail.mutData(), 0xff, n * sizeof(u32));
    memset(layered.mutData(), 0xff, n * sizeof(u32));
    nodes.clear();
    ops.clear();
    triangles.clear();
    textures.clear();
    layers.clear();
    list.clear();
    tiles.clear();
    programs.clear();
    headers.clear();
}

i64 Tiles::snap(float pos, int axis) const {
    return nearest(((pos * scale[axis] + translate[axis]) * half[axis] + half[axis]) * 256.f);
}

u32 Tiles::slot(MetalTexture* texture) {
    if (texture->frame != frame) {
        if (textures.length() == composeTextures) {
            fail(StringView(u8"a frame draws more textures than the compositor binds"));
        }

        texture->frame = frame;
        texture->slot = (u32)textures.length();
        texture->lastUse = serial;
        textures.pushBack(texture);
    }

    return texture->slot;
}

u32 Tiles::layer(MetalImage* image, const i32 (&box)[4]) {
    Placement placement{image, {box[0], box[1], box[2], box[3]}};

    layers.pushBack(placement);

    return (u32)layers.length() - 1;
}

void Tiles::append(u32 tile, u32 entry) {
    u32 node = (u32)(nodes.length() / 2);

    nodes.pushBack(entry);
    nodes.pushBack(composeNone);

    if (tail[tile] == composeNone) {
        head.mut(tile) = node;
    } else {
        nodes.mut(tail[tile] * 2 + 1) = node;
    }

    tail.mut(tile) = node;
    count.mut(tile)++;
}

void Tiles::cover(u32 tile, const Op& op, u32 index, bool opaque) {
    float* c = color.mutData() + (size_t)tile * 4;

    if (opaque) {
        head.mut(tile) = composeNone;
        tail.mut(tile) = composeNone;
        count.mut(tile) = 0;

        if (op.kind == OpRect) {
            base.mut(tile) = composeNone;
            c[0] = op.color[0];
            c[1] = op.color[1];
            c[2] = op.color[2];
            c[3] = 1.f;
        } else {
            base.mut(tile) = index;
            c[0] = c[1] = c[2] = c[3] = 0.f;
        }

        return;
    }

    if (count[tile]) {
        append(tile, index | composeFill);

        return;
    }

    float a = op.color[3];

    for (int k = 0; k < 3; k++) {
        c[k] = op.color[k] * a + c[k] * (1.f - a);
    }

    c[3] = a + c[3] * (1.f - a);
}

void Tiles::place(const Op& op, u32 index, const i32 (&box)[4], bool fills, bool opaque) {
    if (box[2] <= box[0] || box[3] <= box[1]) {
        return;
    }

    i32 tile = (i32)composeTile;
    i32 lastX = (i32)tilesX - 1;
    i32 lastY = (i32)tilesY - 1;
    i32 tx0 = box[0] / tile;
    i32 ty0 = box[1] / tile;
    i32 tx1 = (box[2] - 1) / tile;
    i32 ty1 = (box[3] - 1) / tile;
    i32 fx0 = (box[0] + tile - 1) / tile;
    i32 fy0 = (box[1] + tile - 1) / tile;
    i32 fx1 = box[2] >= (i32)width ? lastX : box[2] / tile - 1;
    i32 fy1 = box[3] >= (i32)height ? lastY : box[3] / tile - 1;

    tx1 = tx1 > lastX ? lastX : tx1;
    ty1 = ty1 > lastY ? lastY : ty1;

    for (i32 ty = ty0; ty <= ty1; ty++) {
        bool inside = fills && ty >= fy0 && ty <= fy1;

        for (i32 tx = tx0; tx <= tx1; tx++) {
            u32 at = (u32)ty * tilesX + (u32)tx;

            if (inside && tx >= fx0 && tx <= fx1) {
                cover(at, op, index, opaque);
            } else {
                append(at, index);
            }

            if (op.kind == OpLayer) {
                layered.mut(at) = op.index;
            }
        }
    }
}

void Tiles::addLayer(MetalImage* image, const float (&lo)[2], const float (&hi)[2], const i32 (&clip)[4]) {
    i64 x0 = snap(lo[0], 0);
    i64 y0 = snap(lo[1], 1);
    i64 x1 = snap(hi[0], 0);
    i64 y1 = snap(hi[1], 1);
    i32 drawn[4] = {firstPixel(x0 < x1 ? x0 : x1), firstPixel(y0 < y1 ? y0 : y1), firstPixel(x0 < x1 ? x1 : x0), firstPixel(y0 < y1 ? y1 : y0)};
    i32 box[4] = {drawn[0], drawn[1], drawn[2], drawn[3]};

    if (drawn[2] <= drawn[0] || drawn[3] <= drawn[1]) {
        return;
    }

    for (int k = 0; k < 2; k++) {
        box[k] = box[k] < clip[k] ? clip[k] : box[k];
        box[k + 2] = box[k + 2] > clip[k + 2] ? clip[k + 2] : box[k + 2];
    }

    Op op{};

    op.kind = OpLayer;
    op.index = layer(image, drawn);
    memcpy(op.rect, box, sizeof(box));

    for (int k = 0; k < 4; k++) {
        op.color[k] = 1.f;
    }

    ops.pushBack(op);
    place(op, (u32)ops.length() - 1, box, false, false);
}

void Tiles::addCommand(const ImDrawList& list, const ImDrawCmd& command) {
    float x0 = (command.ClipRect.x - offset[0]) * zoom[0];
    float y0 = (command.ClipRect.y - offset[1]) * zoom[1];
    float x1 = (command.ClipRect.z - offset[0]) * zoom[0];
    float y1 = (command.ClipRect.w - offset[1]) * zoom[1];

    x0 = x0 < 0.f ? 0.f : x0;
    y0 = y0 < 0.f ? 0.f : y0;
    x1 = x1 > fit[0] ? fit[0] : x1;
    y1 = y1 > fit[1] ? fit[1] : y1;

    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    i32 clip[4] = {(i32)x0, (i32)y0, (i32)x0 + (i32)(u32)(x1 - x0), (i32)y0 + (i32)(u32)(y1 - y0)};

    if (command.UserCallback) {
        if (command.UserCallback == drawLayer) {
            const LayerDraw& draw = *(const LayerDraw*)command.UserCallbackData;

            addLayer(draw.image, draw.lo, draw.hi, clip);
        }

        return;
    }

    ImTextureID id = command.GetTexID();
    MetalTexture* texture = (MetalTexture*)(uintptr_t)id;

    if (!texture) {
        fail(StringView(u8"an interface draw has no texture"));
    }

    u32 at = slot(texture);
    const ImDrawVert* vertices = list.VtxBuffer.Data + command.VtxOffset;
    const ImDrawIdx* indices = list.IdxBuffer.Data + command.IdxOffset;
    bool atlas = id == this->atlas;

    for (u32 e = 0; e < command.ElemCount; e += 3) {
        const ImDrawIdx* index = indices + e;

        if (e + 3 < command.ElemCount && index[3] == index[0] && index[4] == index[2]) {
            const ImDrawVert* v[4] = {&vertices[index[0]], &vertices[index[1]], &vertices[index[2]], &vertices[index[5]]};
            bool rect = v[0]->pos.y == v[1]->pos.y && v[1]->pos.x == v[2]->pos.x && v[2]->pos.y == v[3]->pos.y && v[3]->pos.x == v[0]->pos.x;

            rect = rect && v[0]->uv.y == v[1]->uv.y && v[1]->uv.x == v[2]->uv.x && v[2]->uv.y == v[3]->uv.y && v[3]->uv.x == v[0]->uv.x;
            rect = rect && v[0]->col == v[1]->col && v[0]->col == v[2]->col && v[0]->col == v[3]->col;

            if (rect) {
                i64 fx0 = snap(v[0]->pos.x, 0);
                i64 fx1 = snap(v[2]->pos.x, 0);
                i64 fy0 = snap(v[0]->pos.y, 1);
                i64 fy1 = snap(v[2]->pos.y, 1);
                float u0 = v[0]->uv.x;
                float u1 = v[2]->uv.x;
                float w0 = v[0]->uv.y;
                float w1 = v[2]->uv.y;

                if (fx0 > fx1) {
                    i64 t = fx0;
                    float q = u0;

                    fx0 = fx1;
                    fx1 = t;
                    u0 = u1;
                    u1 = q;
                }

                if (fy0 > fy1) {
                    i64 t = fy0;
                    float q = w0;

                    fy0 = fy1;
                    fy1 = t;
                    w0 = w1;
                    w1 = q;
                }

                i32 box[4] = {firstPixel(fx0), firstPixel(fy0), firstPixel(fx1), firstPixel(fy1)};

                box[0] = box[0] < clip[0] ? clip[0] : box[0];
                box[1] = box[1] < clip[1] ? clip[1] : box[1];
                box[2] = box[2] > clip[2] ? clip[2] : box[2];
                box[3] = box[3] > clip[3] ? clip[3] : box[3];
                e += 3;

                if (box[2] <= box[0] || box[3] <= box[1]) {
                    continue;
                }

                Op op{};
                bool solid = atlas && u0 == u1 && w0 == w1 && u0 == white.x && w0 == white.y;

                op.kind = solid ? OpRect : OpTexture;
                op.index = at;
                op.flags = texture->flags;
                linearColor(v[0]->col, wide, op.color);
                memcpy(op.rect, box, sizeof(box));
                op.uv[0] = u0;
                op.uv[1] = w0;
                op.uv[2] = u1;
                op.uv[3] = w1;
                op.map[0] = (float)fx0 * (1.f / 256.f);
                op.map[1] = (float)fy0 * (1.f / 256.f);
                op.map[2] = fx1 > fx0 ? 256.f / (float)(fx1 - fx0) : 0.f;
                op.map[3] = fy1 > fy0 ? 256.f / (float)(fy1 - fy0) : 0.f;

                bool opaque = (v[0]->col >> 24) == 255u && (solid || texture->opaque);

                ops.pushBack(op);
                place(op, (u32)ops.length() - 1, box, solid || opaque, opaque);
                continue;
            }
        }

        const ImDrawVert* v[3] = {&vertices[index[0]], &vertices[index[1]], &vertices[index[2]]};
        i64 q0[3][2];

        for (int k = 0; k < 3; k++) {
            q0[k][0] = snap(v[k]->pos.x, 0);
            q0[k][1] = snap(v[k]->pos.y, 1);
        }

        i64 area = (q0[1][0] - q0[0][0]) * (q0[2][1] - q0[0][1]) - (q0[1][1] - q0[0][1]) * (q0[2][0] - q0[0][0]);

        if (area == 0) {
            continue;
        }

        int order[3] = {0, 1, 2};

        if (area < 0) {
            order[1] = 2;
            order[2] = 1;
            area = -area;
        }

        Triangle t{};
        i64 q[3][2];

        for (int k = 0; k < 3; k++) {
            q[k][0] = q0[order[k]][0];
            q[k][1] = q0[order[k]][1];
            t.uv[k][0] = v[order[k]]->uv.x;
            t.uv[k][1] = v[order[k]]->uv.y;
            linearColor(v[order[k]]->col, wide, t.colors[k]);
        }

        edge(q[1], q[2], t.edges[0], t.offset[0]);
        edge(q[2], q[0], t.edges[1], t.offset[1]);
        edge(q[0], q[1], t.edges[2], t.offset[2]);
        t.area = area;
        t.texture = at;

        bool solid = atlas && t.uv[0][0] == white.x && t.uv[1][0] == white.x && t.uv[2][0] == white.x && t.uv[0][1] == white.y && t.uv[1][1] == white.y && t.uv[2][1] == white.y;

        t.flags = texture->flags;

        if (solid) {
            t.flags |= Solid;
        }

        memcpy(t.clip, clip, sizeof(clip));

        i64 lo[2];
        i64 hi[2];

        for (int d = 0; d < 2; d++) {
            lo[d] = hi[d] = q[0][d];

            for (int k = 1; k < 3; k++) {
                lo[d] = q[k][d] < lo[d] ? q[k][d] : lo[d];
                hi[d] = q[k][d] > hi[d] ? q[k][d] : hi[d];
            }
        }

        i32 box[4] = {firstPixel(lo[0]), firstPixel(lo[1]), firstPixel(hi[0] + 1), firstPixel(hi[1] + 1)};

        box[0] = box[0] < clip[0] ? clip[0] : box[0];
        box[1] = box[1] < clip[1] ? clip[1] : box[1];
        box[2] = box[2] > clip[2] ? clip[2] : box[2];
        box[3] = box[3] > clip[3] ? clip[3] : box[3];

        if (box[2] <= box[0] || box[3] <= box[1]) {
            continue;
        }

        Op op{};

        op.kind = OpTriangle;
        op.index = (u32)triangles.length();
        memcpy(op.rect, box, sizeof(box));
        triangles.pushBack(t);
        ops.pushBack(op);
        place(op, (u32)ops.length() - 1, box, false, false);
    }
}

void Tiles::finish() {
    u32 n = tilesX * tilesY;
    u32 programCount = (u32)layers.length() * 3 + 1;
    Vector<u32> sizes;
    Vector<u32> program;

    headers.zero(n);
    sizes.zero(programCount);
    program.zero(n);

    for (u32 i = 0; i < n; i++) {
        Header& h = headers.mut(i);
        u32 start = (u32)list.length();
        const Op* video = nullptr;
        bool alone = base[i] == composeNone;

        h.base = base[i];
        h.start = start;
        memcpy(h.color, color.data() + (size_t)i * 4, sizeof(h.color));

        for (u32 node = head[i]; node != composeNone; node = nodes[node * 2 + 1]) {
            u32 entry = nodes[node * 2];
            const Op& op = ops[entry & ~composeFill];
            bool layer = !(entry & composeFill) && op.kind == OpLayer;

            if (layer && op.index != layered[i]) {
                continue;
            }

            video = layer ? &op : video;
            alone = alone && layer;
            list.pushBack(entry);
        }

        h.count = (u32)list.length() - start;

        if (video) {
            i32 x0 = (i32)((i % tilesX) * composeTile);
            i32 y0 = (i32)((i / tilesX) * composeTile);
            i32 x1 = x0 + (i32)composeTile < (i32)width ? x0 + (i32)composeTile : (i32)width;
            i32 y1 = y0 + (i32)composeTile < (i32)height ? y0 + (i32)composeTile : (i32)height;
            bool inside = video->rect[0] <= x0 && video->rect[1] <= y0 && video->rect[2] >= x1 && video->rect[3] >= y1;
            ShaderTiles kind = !alone ? ShaderTiles::Mixed : inside ? ShaderTiles::Inside : ShaderTiles::Edge;

            program.mut(i) = 1 + video->index * 3 + (u32)kind;
        }

        sizes.mut(program[i])++;
    }

    programs.zero((size_t)programCount * 2);

    u32 first = 0;

    for (u32 p = 0; p < programCount; p++) {
        programs.mut(p * 2) = first;
        programs.mut(p * 2 + 1) = 0;
        first += sizes[p];
    }

    tiles.zero(n);

    for (u32 i = 0; i < n; i++) {
        u32 p = program[i];

        tiles.mut(programs[p * 2] + programs[p * 2 + 1]) = i;
        programs.mut(p * 2 + 1)++;
    }
}

void Tiles::compose(const ImDrawData* draw, const Vector<Layer>& underlays, u32 w, u32 h, bool wideFrame, u64 mark, id<MTLCommandBuffer> serialNow, const float (&clear)[4]) {
    reset(w, h);
    wide = wideFrame;
    frame = mark;
    serial = serialNow;

    for (u32 i = 0; i < tilesX * tilesY; i++) {
        float* c = color.mutData() + (size_t)i * 4;

        c[0] = clear[0] * clear[3];
        c[1] = clear[1] * clear[3];
        c[2] = clear[2] * clear[3];
        c[3] = clear[3];
    }

    float size[2] = {(float)w, (float)h};
    float position[2] = {0.f, 0.f};
    float framebuffer[2] = {1.f, 1.f};

    if (draw && draw->DisplaySize.x > 0.f && draw->DisplaySize.y > 0.f) {
        size[0] = draw->DisplaySize.x;
        size[1] = draw->DisplaySize.y;
        position[0] = draw->DisplayPos.x;
        position[1] = draw->DisplayPos.y;
        framebuffer[0] = draw->FramebufferScale.x;
        framebuffer[1] = draw->FramebufferScale.y;
        atlas = ImGui::GetIO().Fonts->TexRef.GetTexID();
        white = ImGui::GetIO().Fonts->TexUvWhitePixel;
    }

    for (int k = 0; k < 2; k++) {
        float pixels = size[k] * framebuffer[k];
        float target = k ? (float)h : (float)w;

        scale[k] = 2.f / size[k];
        translate[k] = -1.f - position[k] * scale[k];
        half[k] = pixels * 0.5f;
        offset[k] = position[k];
        zoom[k] = framebuffer[k];
        fit[k] = pixels < target ? pixels : target;
    }

    i32 whole[4] = {0, 0, (i32)w, (i32)h};

    for (const Layer& under : underlays) {
        addLayer(under.image, under.lo, under.hi, whole);
    }

    if (draw) {
        for (const ImDrawList* list : draw->CmdLists) {
            for (const ImDrawCmd& command : list->CmdBuffer) {
                addCommand(*list, command);
            }
        }
    }

    finish();
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
    [lastUse waitUntilCompleted];
    [texture.lastUse waitUntilCompleted];
}

void MetalImage::prepare() {
    if (source && factory) {
        if (!hostImported) {
            memcpy(buffer.contents, source, sourceSize);
        }
        dirty = true;
    } else if (source) {
        if (!hostImported) {
            for (size_t y = 0; y < height; y++) {
                memcpy((u8*)buffer.contents + y * bufferStride, (const u8*)source + y * sourceStride, (size_t)width * 4);
            }
        }
        dirty = true;
    }
}

void MetalImage::draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) {
    renderer->drawn.pushBack(this);
    if (factory) {
        LayerDraw layer{this, {lo.x, lo.y}, {hi.x, hi.y}};
        list.AddCallback(drawLayer, &layer, sizeof(layer));
        return;
    }
    list.AddImage(ImTextureRef((ImTextureID)(uintptr_t)&texture), lo, hi);
}

void MetalImage::read(int x0, int y0, int x1, int y1, ImagePixels& out) {
    if (factory) {
        checkImageRegion(renderer->maxTextureSide(), renderer->maxTextureSide(), x0, y0, x1, y1);
        readShaded(x0, y0, x1, y1, out);
        return;
    }
    checkImageRegion((u32)texture.texture.width, (u32)texture.texture.height, x0, y0, x1, y1);
    @autoreleasepool {
        u32 w = (u32)(x1 - x0);
        u32 h = (u32)(y1 - y0);
        size_t stride = ((size_t)w * 4 + 255) & ~(size_t)255;
        id<MTLBuffer> readback = [renderer->device newBufferWithLength:stride * h options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [renderer->queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!readback || !command || !blit) {
            fail(StringView(u8"cannot allocate Metal readback"));
        }
        [blit copyFromTexture:texture.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:stride destinationBytesPerImage:stride * h];
        [blit endEncoding];
        [command commit];
        [command waitUntilCompleted];
        checkCommand(command);
        checkCommand(renderer->last);
        unpackPixels(readback.contents, w, h, stride, layout, out);
    }
}

void MetalImage::readShaded(int x0, int y0, int x1, int y1, ImagePixels& out) {
    @autoreleasepool {
        u32 w = (u32)(x1 - x0);
        u32 h = (u32)(y1 - y0);
        const float clear[4] = {0.f, 0.f, 0.f, 0.f};
        Vector<Layer> whole;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:(NSUInteger)x1 height:(NSUInteger)y1 mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageShaderWrite;
        id<MTLTexture> target = [renderer->device newTextureWithDescriptor:descriptor];
        size_t stride = ((size_t)w * 8 + 255) & ~(size_t)255;
        id<MTLBuffer> readback = [renderer->device newBufferWithLength:stride * h options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [renderer->queue commandBuffer];
        if (!target || !readback || !command) {
            fail(StringView(u8"cannot allocate Metal readback"));
        }
        whole.pushBack(Layer{this, {(float)x0, (float)y0}, {(float)x1, (float)y1}});
        renderer->tiles.compose(nullptr, whole, (u32)x1, (u32)y1, hdr, ++renderer->frames, command, clear);
        renderer->encode(command, target, hdr ? ShaderOutput::WideLinear : ShaderOutput::Linear);
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:stride destinationBytesPerImage:stride * h];
        [blit endEncoding];
        [command commit];
        [command waitUntilCompleted];
        checkCommand(command);
        unpackPixels(readback.contents, w, h, stride, PixelLayout::Rgba16f, out);
    }
}

id<MTLLibrary> MetalRenderer::library(NSString* source) {
    NSError* error = nil;
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    options.languageVersion = MTLLanguageVersion3_0;
    id<MTLLibrary> made = [device newLibraryWithSource:[@"#include <metal_stdlib>\nusing namespace metal;\n" stringByAppendingString:source] options:options error:&error];
    if (!made) {
        fail(StringView(StringBuilder() << StringView(u8"Metal compositor: ") << StringView(error ? error.localizedDescription.UTF8String : "no library")));
    }
    return made;
}

id<MTLComputePipelineState> MetalRenderer::pipeline(id<MTLLibrary> from, ShaderOutput output, u32 side, id<MTLFunction> linked) {
    MTLFunctionConstantValues* constants = [[MTLFunctionConstantValues alloc] init];
    int encoding = output == ShaderOutput::Srgb ? 0 : 2;
    bool wideOutput = output == ShaderOutput::WideLinear;
    NSError* error = nil;
    [constants setConstantValue:&encoding type:MTLDataTypeInt atIndex:0];
    [constants setConstantValue:&wideOutput type:MTLDataTypeBool atIndex:1];
    id<MTLFunction> function = [from newFunctionWithName:@"compose" constantValues:constants error:&error];
    if (!function) {
        fail(StringView(StringBuilder() << StringView(u8"Metal compositor function: ") << StringView(error ? error.localizedDescription.UTF8String : "missing")));
    }
    MTLComputePipelineDescriptor* descriptor = [[MTLComputePipelineDescriptor alloc] init];
    descriptor.computeFunction = function;
    descriptor.maxTotalThreadsPerThreadgroup = (NSUInteger)side * side;
    if (linked) {
        MTLLinkedFunctions* functions = [MTLLinkedFunctions linkedFunctions];
        functions.privateFunctions = @[linked];
        descriptor.linkedFunctions = functions;
    }
    id<MTLComputePipelineState> made = [device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone reflection:nil error:&error];
    if (!made) {
        fail(StringView(StringBuilder() << StringView(u8"Metal compositor pipeline: ") << StringView(error ? error.localizedDescription.UTF8String : "missing")));
    }
    if (made.maxTotalThreadsPerThreadgroup < (NSUInteger)side * side) {
        fail(StringView(u8"Metal compositor does not fit a threadgroup"));
    }
    return made;
}

void MetalRenderer::encode(id<MTLCommandBuffer> command, id<MTLTexture> target, ShaderOutput output) {
    const Tiles& t = tiles;
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    if (!compute) {
        fail(StringView(u8"cannot begin the Metal compositor"));
    }
    const void* sources[5] = {t.headers.data(), t.list.data(), t.ops.data(), t.triangles.data(), t.tiles.data()};
    size_t sizes[5] = {t.headers.length() * sizeof(Header), t.list.length() * sizeof(u32), t.ops.length() * sizeof(Op), t.triangles.length() * sizeof(Triangle), t.tiles.length() * sizeof(u32)};
    for (NSUInteger i = 0; i < 5; i++) {
        id<MTLBuffer> buffer = sizes[i] ? [device newBufferWithBytes:sources[i] length:sizes[i] options:MTLResourceStorageModeShared] : [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
        if (!buffer) {
            fail(StringView(u8"cannot allocate the Metal compositor's buffers"));
        }
        [compute setBuffer:buffer offset:0 atIndex:i];
    }
    id<MTLBuffer> textures = [device newBufferWithLength:1024 * sizeof(MTLResourceID) options:MTLResourceStorageModeShared];
    if (!textures) {
        fail(StringView(u8"cannot allocate the Metal compositor's textures"));
    }
    MTLResourceID* ids = (MTLResourceID*)textures.contents;
    for (size_t i = 0; i < t.textures.length(); i++) {
        ids[i] = t.textures[i]->texture.gpuResourceID;
        [compute useResource:t.textures[i]->texture usage:MTLResourceUsageRead];
    }
    [compute setBuffer:textures offset:0 atIndex:6];
    [compute setTexture:target atIndex:0];
    Push push{{(i32)target.width, (i32)target.height}, {0, 0}, t.tilesX, 0, sdrWhiteNits};
    for (u32 p = 0; p * 2 < t.programs.length(); p++) {
        u32 count = t.programs[p * 2 + 1];
        if (!count) {
            continue;
        }
        push.first = t.programs[p * 2];
        if (p == 0) {
            if (!plain[(u32)output]) {
                plain[(u32)output] = pipeline(plainLibrary, output, composeGroup, nil);
            }
            [compute setComputePipelineState:plain[(u32)output]];
            [compute setBytes:&push length:sizeof(push) atIndex:5];
            [compute dispatchThreadgroups:MTLSizeMake(count, (composeTile / composeGroup) * (composeTile / composeGroup), 1) threadsPerThreadgroup:MTLSizeMake(composeGroup, composeGroup, 1)];
            continue;
        }
        const Placement& placed = t.layers[(p - 1) / 3];
        MetalImage* image = placed.image;
        ShaderOptions options{ShaderTarget::Air, output, (ShaderTiles)((p - 1) % 3), {(u32)(placed.box[2] - placed.box[0]), (u32)(placed.box[3] - placed.box[1])}};
        MetalShader& shader = static_cast<MetalShader&>(image->factory->shader(options));
        push.video[0] = placed.box[0];
        push.video[1] = placed.box[1];
        [compute setComputePipelineState:shader.pipeline];
        [compute setBuffer:image->buffer offset:0 atIndex:7];
        [compute setBytes:&push length:sizeof(push) atIndex:5];
        [compute dispatchThreadgroups:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(composeTile, composeTile, 1)];
    }
    [compute endEncoding];
}

void MetalRenderer::updateTextures(ImDrawData* draw) {
    if (!draw->Textures) {
        return;
    }
    for (ImTextureData* data : *draw->Textures) {
        if (data->Status == ImTextureStatus_WantCreate) {
            if (data->Format != ImTextureFormat_RGBA32) {
                fail(StringView(u8"the interface asks for a texture format the renderer does not draw"));
            }
            MetalTexture* texture = smallObjects->make<MetalTexture>();
            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:(NSUInteger)data->Width height:(NSUInteger)data->Height mipmapped:NO];
            descriptor.storageMode = MTLStorageModeManaged;
            descriptor.usage = MTLTextureUsageShaderRead;
            texture->texture = [device newTextureWithDescriptor:descriptor];
            if (!texture->texture) {
                fail(StringView(u8"cannot allocate an interface texture"));
            }
            [texture->texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)data->Width, (NSUInteger)data->Height) mipmapLevel:0 withBytes:data->GetPixels() bytesPerRow:(NSUInteger)data->GetPitch()];
            texture->opaque = opaquePixels((const u8*)data->GetPixels(), (size_t)data->GetPitch(), (u32)data->Width, (u32)data->Height);
            data->BackendUserData = texture;
            data->SetTexID((ImTextureID)(uintptr_t)texture);
            data->SetStatus(ImTextureStatus_OK);
        } else if (data->Status == ImTextureStatus_WantUpdates) {
            MetalTexture* texture = (MetalTexture*)data->BackendUserData;
            [texture->lastUse waitUntilCompleted];
            for (const ImTextureRect& rect : data->Updates) {
                [texture->texture replaceRegion:MTLRegionMake2D(rect.x, rect.y, rect.w, rect.h) mipmapLevel:0 withBytes:data->GetPixelsAt(rect.x, rect.y) bytesPerRow:(NSUInteger)data->GetPitch()];
            }
            if (data->Updates.empty()) {
                const ImTextureRect& rect = data->UpdateRect;
                [texture->texture replaceRegion:MTLRegionMake2D(rect.x, rect.y, rect.w, rect.h) mipmapLevel:0 withBytes:data->GetPixelsAt(rect.x, rect.y) bytesPerRow:(NSUInteger)data->GetPitch()];
            }
            texture->opaque = opaquePixels((const u8*)data->GetPixels(), (size_t)data->GetPitch(), (u32)data->Width, (u32)data->Height);
            data->SetStatus(ImTextureStatus_OK);
        } else if (data->Status == ImTextureStatus_WantDestroy && data->UnusedFrames >= (int)drawables) {
            MetalTexture* texture = (MetalTexture*)data->BackendUserData;
            [texture->lastUse waitUntilCompleted];
            smallObjects->release(texture);
            data->BackendUserData = nullptr;
            data->SetTexID(ImTextureID_Invalid);
            data->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

RenderImage* MetalRenderer::upload(ObjPool& pool, u32 width, u32 height, const void* rgba, bool imageHdr) {
    checkImageSize(width, height, maxTextureSide());
    if (!rgba) {
        fail(StringView(u8"invalid renderer image source"));
    }
    @autoreleasepool {
        MetalImage* image = pool.make<MetalImage>();
        image->renderer = this;
        image->hdr = imageHdr;
        image->width = width;
        image->height = height;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:imageHdr ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA8Unorm_sRGB width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModeManaged;
        descriptor.usage = MTLTextureUsageShaderRead;
        image->texture.texture = [device newTextureWithDescriptor:descriptor];
        if (!image->texture.texture) {
            fail(StringView(u8"cannot allocate Metal image"));
        }
        image->texture.flags = imageHdr ? DecodePq | SourceWide : DecodeLinear;
        image->texture.opaque = opaquePixels((const u8*)rgba, (size_t)width * 4, width, height);
        [image->texture.texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:rgba bytesPerRow:(size_t)width * 4];
        return image;
    }
}

RenderImage* MetalRenderer::import(ObjPool& pool, SharedImage& shared, bool imageHdr) {
    SurfaceImage& source = static_cast<SurfaceImage&>(shared);
    @autoreleasepool {
        MetalImage* image = pool.make<MetalImage>();
        image->renderer = this;
        image->hdr = imageHdr;
        image->layout = source.layout;
        image->width = source.width;
        image->height = source.height;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.format width:source.width height:source.height mipmapped:NO];
        descriptor.storageMode = MTLStorageModeManaged;
        descriptor.usage = MTLTextureUsageShaderRead;
        image->texture.texture = [device newTextureWithDescriptor:descriptor iosurface:source.surface plane:0];
        if (!image->texture.texture) {
            fail(StringView(u8"cannot import IOSurface into Metal"));
        }
        image->texture.flags = imageHdr ? DecodePq | SourceWide : DecodeSrgb;
        return image;
    }
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

Flight::Flight(const Vector<MetalImage*>& drawn) {
    for (MetalImage* image : drawn) {
        if (image->retired) {
            images.pushBack(image);
        }
    }
}

void MetalRenderer::poll() {
    void* item;

    while (landed->tryDequeue(&item)) {
        Flight* flight = (Flight*)item;
        Vector<MetalImage*> done;

        done.xchg(flight->images);
        smallObjects->release(flight);

        for (MetalImage* image : done) {
            image->retired->run();
        }
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
        return true;
    }
}

void MetalRenderer::setMode(bool want) {
    CGColorSpaceRef color = CGColorSpaceCreateWithName(want ? kCGColorSpaceExtendedLinearITUR_2020 : kCGColorSpaceSRGB);
    if (!color) {
        fail(StringView(u8"cannot create Metal color space"));
    }
    layer.pixelFormat = want ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
    layer.colorspace = color;
    layer.wantsExtendedDynamicRangeContent = want;
    CGColorSpaceRelease(color);
    wide = want;
}

bool MetalRenderer::endFrame(ImDrawData* draw) {
    @autoreleasepool {
        bool want = false;
        updateTextures(draw);
        for (const MetalImage* image : drawn) {
            want = want || image->hdr;
        }
        want = want && edr;
        if (want != wide) {
            setMode(want);
            drawable = [layer nextDrawable];
            if (!drawable) {
                fail(StringView(u8"Metal gives no drawable in the frame's new mode"));
            }
        }
        id<MTLCommandBuffer> command = [queue commandBuffer];
        if (!command) {
            fail(StringView(u8"cannot begin Metal command buffer"));
        }
        for (MetalImage* image : drawn) {
            if (image->factory) {
                image->dirty = false;
            } else if (image->dirty) {
                id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
                if (!blit) {
                    fail(StringView(u8"cannot begin Metal image upload"));
                }
                [blit copyFromBuffer:image->buffer sourceOffset:0 sourceBytesPerRow:image->bufferStride sourceBytesPerImage:image->bufferStride * image->height sourceSize:MTLSizeMake(image->width, image->height, 1) toTexture:image->texture.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding];
                image->dirty = false;
            }
            image->lastUse = command;
        }
        Flight* flight = smallObjects->make<Flight>(drawn);
        drawn.clear();
        const float clear[4] = {srgbTable[25], srgbTable[25], srgbTable[25], 1.f};
        tiles.compose(draw, Vector<Layer>(), (u32)drawable.texture.width, (u32)drawable.texture.height, wide, ++frames, command, clear);
        encode(command, drawable.texture, wide ? ShaderOutput::WideLinear : ShaderOutput::Srgb);
        Channel* done = landed;
        plt::LoopWake* completed = wake;
        [command addCompletedHandler:^(id<MTLCommandBuffer>) {
          done->enqueue(flight);
          completed->signal();
        }];
        [command presentDrawable:drawable];
        [command commit];
        last = command;
        drawable = nil;
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
        image->width = width;
        image->height = height;
        size_t page = (size_t)getpagesize();
        if ((uintptr_t)data % page == 0 && size % page == 0 && stride % 256 == 0) {
            image->buffer = [device newBufferWithBytesNoCopy:const_cast<void*>(data) length:size options:MTLResourceStorageModeShared deallocator:nil];
            image->hostImported = image->buffer != nil;
        }
        image->bufferStride = image->hostImported ? stride : ((size_t)width * 4 + 255) & ~(size_t)255;
        if (!image->hostImported) {
            image->buffer = [device newBufferWithLength:image->bufferStride * height options:MTLResourceStorageModeShared];
        }
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageShaderRead;
        image->texture.texture = [device newTextureWithDescriptor:descriptor];
        if (!image->buffer || !image->texture.texture) {
            fail(StringView(u8"cannot allocate bound Metal image"));
        }
        return image;
    }
}

RenderShader* MetalRenderer::compileKernel(ObjPool& pool, const void* code, size_t size, u32 tile, const ShaderOptions& options) {
    if (tile != composeTile) {
        fail(StringView(u8"a kernel is not made for the compositor's tile"));
    }
    if (!code || !size) {
        fail(StringView(u8"invalid shader code"));
    }
    @autoreleasepool {
        NSError* error = nil;
        dispatch_data_t bytes = dispatch_data_create(code, size, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        id<MTLLibrary> made = [device newLibraryWithData:bytes error:&error];
        if (!made) {
            fail(StringView(StringBuilder() << StringView(u8"Metal shader: ") << StringView(error ? error.localizedDescription.UTF8String : "no library")));
        }
        MetalShader* shader = pool.make<MetalShader>();
        if (options.tiles != ShaderTiles::Mixed) {
            shader->pipeline = pipeline(made, options.output, composeTile, nil);
            return shader;
        }
        id<MTLFunction> layer = [made newFunctionWithName:@"layer"];
        if (!layer) {
            fail(StringView(u8"Metal shader: no layer function"));
        }
        if (!layeredLibrary) {
            layeredLibrary = library([NSString stringWithFormat:@"#define GROUP %u\n#define LAYER 1\n%s", composeTile, composeSource]);
        }
        shader->pipeline = pipeline(layeredLibrary, options.output, composeTile, layer);
        return shader;
    }
}

RenderImage* MetalRenderer::shade(ObjPool& pool, ShaderFactory& factory, u32 width, u32 height, const void* data, size_t size, bool imageHdr, Runable& retired) {
    checkImageSize(width, height, maxTextureSide());
    if (!data || !size || size % 4) {
        fail(StringView(u8"invalid shaded image source"));
    }
    @autoreleasepool {
        MetalImage* image = pool.make<MetalImage>();
        image->renderer = this;
        image->layout = PixelLayout::Rgba16f;
        image->hdr = imageHdr;
        image->width = width;
        image->height = height;
        image->source = data;
        image->sourceSize = size;
        image->factory = &factory;
        image->retired = &retired;
        size_t page = (size_t)getpagesize();
        if ((uintptr_t)data % page == 0 && size % page == 0) {
            image->buffer = [device newBufferWithBytesNoCopy:const_cast<void*>(data) length:size options:MTLResourceStorageModeShared deallocator:nil];
            image->hostImported = image->buffer != nil;
        }
        if (!image->hostImported) {
            image->buffer = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
        }
        if (!image->buffer) {
            fail(StringView(u8"cannot allocate shaded Metal image"));
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
    renderer->sdrWhiteNits = options.sdrWhiteNits;
    if (renderer->device == nil) {
        fail(StringView(u8"no metal device"));
    }
    if (renderer->device.maxThreadgroupMemoryLength < kernelShared || renderer->device.maxThreadsPerThreadgroup.width < composeTile || renderer->device.maxThreadsPerThreadgroup.height < composeTile) {
        fail(StringView(u8"Metal runs smaller threadgroups than the compositor's tiles"));
    }
    renderer->queue = [renderer->device newCommandQueue];
    if (!renderer->queue) {
        fail(StringView(u8"cannot create Metal queue"));
    }
    for (u32 i = 0; i < 256; i++) {
        double c = i / 255.;
        srgbTable[i] = (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
    }
    NSScreen* screen = renderer->window.screen ? renderer->window.screen : NSScreen.mainScreen;
    renderer->edr = screen.maximumPotentialExtendedDynamicRangeColorComponentValue > 1.0;
    CAMetalLayer* layer = renderer->layer;
    layer.device = renderer->device;
    layer.framebufferOnly = NO;
    layer.maximumDrawableCount = drawables;
    layer.presentsWithTransaction = NO;
    renderer->setMode(false);
    renderer->plainLibrary = renderer->library([NSString stringWithFormat:@"#define GROUP %u\n%s", composeGroup, composeSource]);
    renderer->plain[(u32)ShaderOutput::Srgb] = renderer->pipeline(renderer->plainLibrary, ShaderOutput::Srgb, composeGroup, nil);
    renderer->wake = platform.createLoopWake(pool, *pool.make<PollMetal>(renderer));
    renderer->smallObjects = SmallObjAllocator::create(&pool);
    renderer->landed = Channel::create(&pool, 64);
    renderer->target = [ImMetalDisplayTarget new];
    renderer->target.owner = renderer;
    renderer->displayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
    renderer->displayLink.delegate = renderer->target;
    renderer->displayLink.paused = YES;
    [renderer->displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    pooledGuard(pool, [renderer] {
        [renderer->displayLink invalidate];
    });
    ImGuiIO& io = ImGui::GetIO();
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    io.BackendRendererName = "im_compose";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    pio.Renderer_TextureMaxWidth = (int)maxTextureSize;
    pio.Renderer_TextureMaxHeight = (int)maxTextureSize;
    pooledGuard(pool, [renderer] {
        [renderer->last waitUntilCompleted];
        for (ImTextureData* data : ImGui::GetPlatformIO().Textures) {
            MetalTexture* texture = (MetalTexture*)data->BackendUserData;
            if (texture) {
                renderer->smallObjects->release(texture);
                data->BackendUserData = nullptr;
                data->SetTexID(ImTextureID_Invalid);
                data->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        ImGuiIO& shut = ImGui::GetIO();
        shut.BackendRendererName = nullptr;
        shut.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    });
    return renderer;
}
