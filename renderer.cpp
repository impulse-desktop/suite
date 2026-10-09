#include "renderer.h"

#include "error.h"

#include <std/lib/vector.h>

#include <math.h>
#include <string.h>

using namespace stl;

static Renderer* createRenderer(ObjPool& pool, plt::Platform& platform, plt::Window& window, const RendererOptions& options);
static SharedImage* createSharedImage(ObjPool& pool, StringView description, intptr_t handle);

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

namespace {
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

    constexpr size_t callBytes = 40;
    constexpr ShaderParameter hostParameters[] = {{ShaderInput::TargetSize, 0, 8}, {ShaderInput::VideoOrigin, 8, 8}, {ShaderInput::TilesAcross, 16, 4}, {ShaderInput::FirstTile, 20, 4}, {ShaderInput::TextureWhite, 24, 4}};
    constexpr u32 hostParameterCount = (u32)(sizeof(hostParameters) / sizeof(hostParameters[0]));

    struct Call {
        i32 size[2];
        i32 video[2];
        i32 box[2];
        u32 tilesX;
        u32 first;
        float textureWhite;
    };

    void fillCall(const ShaderParameter* parameters, u32 count, const Call& call, u8 (&block)[callBytes]) {
        memset(block, 0, callBytes);
        for (u32 i = 0; i < count; i++) {
            const ShaderParameter& parameter = parameters[i];
            const void* from;
            switch (parameter.input) {
                case ShaderInput::TargetSize:
                    from = call.size;
                    break;
                case ShaderInput::VideoOrigin:
                    from = call.video;
                    break;
                case ShaderInput::BoxSize:
                    from = call.box;
                    break;
                case ShaderInput::TilesAcross:
                    from = &call.tilesX;
                    break;
                case ShaderInput::FirstTile:
                    from = &call.first;
                    break;
                case ShaderInput::TextureWhite:
                    from = &call.textureWhite;
                    break;
                default:
                    continue;
            }
            if (parameter.offset > callBytes || parameter.size > callBytes - parameter.offset) {
                fail(StringView(u8"a shader's call block exceeds the compositor's"));
            }
            memcpy(block + parameter.offset, from, parameter.size);
        }
    }

    struct ComposeTexture {
        u32 flags = DecodeLinear;
        bool opaque = false;
        u64 frame = 0;
        u32 slot = 0;
    };

    // What the frame asks of every image it draws: the size, whether it
    // wants the wide mode, and whom to tell when the frame that drew it
    // lands. Each renderer's images derive from it.
    struct Image: RenderImage {
        u32 width = 0;
        u32 height = 0;
        bool hdr = false;
        Runable* retired = nullptr;

        void prepare() override {
        }
    };

    struct Layer {
        Image* image;
        float lo[2];
        float hi[2];
    };

    struct LayerDraw {
        Image* image;
        float lo[2];
        float hi[2];
    };

    static void drawLayer(const ImDrawList*, const ImDrawCmd*) {
    }

    float srgbTable[256];

    static void fillSrgbTable() {
        for (u32 i = 0; i < 256; i++) {
            double c = i / 255.;

            srgbTable[i] = (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
        }
    }

    struct Placement {
        Image* image;
        i32 box[4];
    };

    struct Tiles {
        Vector<Header> headers;
        Vector<u32> list;
        Vector<Op> ops;
        Vector<Triangle> triangles;
        Vector<u32> tiles;
        Vector<u32> programs;
        Vector<ComposeTexture*> textures;
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
        float scale[2] = {};
        float translate[2] = {};
        float half[2] = {};
        float fit[2] = {};
        float offset[2] = {};
        float zoom[2] = {};
        ImTextureID atlas = ImTextureID_Invalid;
        ImVec2 white = {};

        void compose(const ImDrawData* draw, const Vector<Layer>& underlays, u32 width, u32 height, bool wide, u64 frame);
        void reset(u32 width, u32 height);
        i64 snap(float pos, int axis) const;
        void place(const Op& op, u32 index, const i32 (&box)[4], bool fills, bool opaque);
        void cover(u32 tile, const Op& op, u32 index, bool opaque);
        void append(u32 tile, u32 entry);
        u32 slot(ComposeTexture* texture);
        u32 layer(Image* image, const i32 (&box)[4]);
        void addLayer(Image* image, const float (&lo)[2], const float (&hi)[2], const i32 (&clip)[4]);
        void addCommand(const ImDrawList& list, const ImDrawCmd& command);
        void finish();
    };

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

u32 Tiles::slot(ComposeTexture* texture) {
    if (texture->frame != frame) {
        if (textures.length() == composeTextures) {
            fail(StringView(u8"a frame draws more textures than the compositor binds"));
        }

        texture->frame = frame;
        texture->slot = (u32)textures.length();
        textures.pushBack(texture);
    }

    return texture->slot;
}

u32 Tiles::layer(Image* image, const i32 (&box)[4]) {
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

void Tiles::addLayer(Image* image, const float (&lo)[2], const float (&hi)[2], const i32 (&clip)[4]) {
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
    ComposeTexture* texture = (ComposeTexture*)(uintptr_t)id;

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

// Nothing lies under the frame: a tile starts transparent, the identity
// of over, and what no layer covers stays at zero alpha for the window
// system to show what is behind.
void Tiles::compose(const ImDrawData* draw, const Vector<Layer>& underlays, u32 w, u32 h, bool wideFrame, u64 mark) {
    reset(w, h);
    wide = wideFrame;
    frame = mark;

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

Renderer* Renderer::create(stl::ObjPool& pool, plt::Platform& platform, plt::Window& window, const RendererOptions& options) {
    if (!(options.textureWhiteNits > 0.f) || options.textureWhiteNits > 10000.f) {
        fail(StringView(u8"invalid texture white level"));
    }
    return createRenderer(pool, platform, window, options);
}

SharedImage* SharedImage::create(stl::ObjPool& pool, stl::StringView description, intptr_t handle) {
    return createSharedImage(pool, description, handle);
}

#if defined(__APPLE__)

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
    #include <video_generic.inc>

    #import <Metal/Metal.h>
    #import <AppKit/AppKit.h>
    #import <IOSurface/IOSurface.h>
    #import <QuartzCore/CAMetalLayer.h>

using namespace stl;

namespace {
    constexpr u32 drawables = 3;
    constexpr u32 maxTextureSize = 16384;

    struct MetalTexture: ComposeTexture {
        id<MTLTexture> texture = nil;
        id<MTLCommandBuffer> lastUse = nil;
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
        id<MTLBuffer> constants = nil;
        const ShaderParameter* parameters = nullptr;
        u32 parameterCount = 0;
    };

    // A producer's memory as the GPU reads it: the producer's own pages
    // when Metal takes them, otherwise a shared buffer the image copies the
    // source into before each frame.
    struct HostBuffer {
        id<MTLBuffer> buffer = nil;
        const void* source = nullptr;
        size_t bytes = 0;
        bool imported = false;
    };

    // An image the frame draws, with the commands it records into the frame
    // before the compositor runs.
    struct MetalImage: Image {
        MetalRenderer* renderer = nullptr;
        id<MTLCommandBuffer> lastUse = nil;

        ~MetalImage() noexcept;
        virtual void record(id<MTLCommandBuffer> command);
    };

    // A texture filled once: uploaded pixels or an imported IOSurface.
    struct TextureImage: MetalImage {
        MetalTexture texture;
        PixelLayout layout = PixelLayout::Rgba8;

        ~TextureImage() noexcept;
        void draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) override;
        void read(int x0, int y0, int x1, int y1, ImagePixels& out) override;
    };

    // A texture refilled from the producer's buffer before each frame.
    struct BoundImage final: TextureImage {
        HostBuffer host;
        size_t sourceStride = 0;
        size_t bufferStride = 0;
        bool dirty = false;

        void prepare() override;
        void record(id<MTLCommandBuffer> command) override;
    };

    // The producer's words, drawn as a layer by the kernel its factory makes.
    struct ShadedImage final: MetalImage {
        HostBuffer host;
        ShaderFactory* factory = nullptr;
        bool dirty = false;

        void prepare() override;
        void record(id<MTLCommandBuffer> command) override;
        void draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) override;
        void read(int x0, int y0, int x1, int y1, ImagePixels& out) override;
    };

    struct Flight {
        Vector<MetalImage*> images;

        explicit Flight(const Vector<MetalImage*>& drawn);
    };

    struct MetalRenderer final: Renderer {
        plt::Window* host = nullptr;
        plt::LoopWake* wake = nullptr;
        Vector<MetalImage*> drawn;
        SmallObjAllocator* smallObjects = nullptr;
        Channel* landed = nullptr;
        CAMetalLayer* layer = nil;
        NSWindow* window = nil;
        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;
        id<CAMetalDrawable> drawable = nil;
        id<MTLCommandBuffer> last = nil;
        id<MTLLibrary> plainLibrary = nil;
        id<MTLLibrary> layeredLibrary = nil;
        id<MTLLibrary> genericLayeredLibrary = nil;
        id<MTLComputePipelineState> plain[outputs] = {};
        id<MTLComputePipelineState> genericLayer[outputs] = {};
        bool edr = false;
        bool wide = false;
        float textureWhiteNits = 203.f;
        u64 frames = 0;
        Tiles tiles;

        void stamp(id<MTLCommandBuffer> command);
        void allocateHost(HostBuffer& host, const void* source, size_t size, size_t bytes, bool importable);
        bool beginFrame(u32 width, u32 height) override;
        void poll();
        bool endFrame(ImDrawData* draw) override;
        u32 maxTextureSide() override;
        u32 maxTextures() override;
        RenderImage* upload(ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) override;
        RenderImage* import(ObjPool& pool, SharedImage& source, bool hdr) override;

        RenderImage* bind(ObjPool& pool, u32 width, u32 height, const void* data, size_t size, size_t stride, Runable& retired) override;
        RenderShader* compileKernel(ObjPool& pool, const CompiledShader& compiled, u32 tile, const ShaderOptions& options) override;
        RenderImage* shade(ObjPool& pool, ShaderFactory& factory, u32 width, u32 height, const void* data, size_t size, bool hdr, Runable& retired) override;
        id<MTLLibrary> library(NSString* source);
        id<MTLLibrary> composeLibrary(const char* defines, bool generic);
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
constant float WHITE [[function_constant(2)]];

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
    float textureWhite;
    int2 box;
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
        s.rgb = pqDecode(s.rgb) / frame.textureWhite;
    }

    s.rgb = toFrame(s.rgb, (flags & SOURCE_WIDE) != 0u);

    return s;
}

static float4 over(float4 src, float4 acc) {
    return float4(src.rgb * src.a, src.a) + acc * (1.0 - src.a);
}
)metal";

    static constexpr const char* genericPrelude = R"metal(
#define ARGS_DECL , const device uint* words, constant Facts& facts, constant Frame& frame, device const Header* headers, device const uint* list, device const Op* ops, device const uint* tiles, texture2d<float, access::write> target
#define ARGS , words, facts, frame, headers, list, ops, tiles, target
#define HALF(h) float(as_type<half>(ushort((h) & 0xffffu)))
#define BITS_FLOAT(b) as_type<float>(b)
#define STORE(pixel, value) target.write(value, uint2(pixel))
#define FACTS_DECL
)metal";

    static constexpr const char* composeKernel = R"metal(
#ifdef LAYER
#ifndef GENERIC
[[visible]] float4 layer(uint2 local, int2 origin, const device uint* words);
#endif
#endif

kernel void compose(device const Header* headers [[buffer(0)]], device const uint* list [[buffer(1)]], device const Op* ops [[buffer(2)]], device const Triangle* triangles [[buffer(3)]], device const uint* tiles [[buffer(4)]], constant Frame& frame [[buffer(5)]], constant Textures& textures [[buffer(6)]],
#ifdef LAYER
    const device uint* words [[buffer(7)]],
#endif
#ifdef GENERIC
    constant Facts& facts [[buffer(8)]],
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
#ifdef GENERIC
    shown = genericShown(inside.xy, origin - frame.video ARGS);
#else
    shown = layer(inside.xy, origin - frame.video, words);
#endif
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
        target.write(acc.a > 0.0 ? float4(encode(acc.rgb / acc.a, pixel, frame) * acc.a, acc.a) : float4(0.0), uint2(pixel));
    }
}
)metal";
}

SurfaceImage::~SurfaceImage() noexcept {
    if (surface) {
        CFRelease(surface);
    }
}

static SharedImage* createSharedImage(ObjPool& pool, StringView description, intptr_t handle) {
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
}

TextureImage::~TextureImage() noexcept {
    [texture.lastUse waitUntilCompleted];
}

void MetalImage::record(id<MTLCommandBuffer>) {
}

void BoundImage::prepare() {
    if (!host.imported) {
        for (size_t y = 0; y < height; y++) {
            memcpy((u8*)host.buffer.contents + y * bufferStride, (const u8*)host.source + y * sourceStride, (size_t)width * 4);
        }
    }
    dirty = true;
}

void BoundImage::record(id<MTLCommandBuffer> command) {
    if (!dirty) {
        return;
    }
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (!blit) {
        fail(StringView(u8"cannot begin Metal image upload"));
    }
    [blit copyFromBuffer:host.buffer sourceOffset:0 sourceBytesPerRow:bufferStride sourceBytesPerImage:bufferStride * height sourceSize:MTLSizeMake(width, height, 1) toTexture:texture.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    dirty = false;
}

void ShadedImage::prepare() {
    if (!host.imported) {
        memcpy(host.buffer.contents, host.source, host.bytes);
    }
    dirty = true;
}

void ShadedImage::record(id<MTLCommandBuffer>) {
    dirty = false;
}

void TextureImage::draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) {
    renderer->drawn.pushBack(this);
    list.AddImage(ImTextureRef((ImTextureID)(uintptr_t)&texture), lo, hi);
}

void ShadedImage::draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) {
    renderer->drawn.pushBack(this);
    LayerDraw layer{this, {lo.x, lo.y}, {hi.x, hi.y}};
    list.AddCallback(drawLayer, &layer, sizeof(layer));
}

void TextureImage::read(int x0, int y0, int x1, int y1, ImagePixels& out) {
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

// Draws the image into the region of a wide target and copies the region out.
void ShadedImage::read(int x0, int y0, int x1, int y1, ImagePixels& out) {
    checkImageRegion(renderer->maxTextureSide(), renderer->maxTextureSide(), x0, y0, x1, y1);
    @autoreleasepool {
        u32 w = (u32)(x1 - x0);
        u32 h = (u32)(y1 - y0);
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
        renderer->tiles.compose(nullptr, whole, (u32)x1, (u32)y1, hdr, ++renderer->frames);
        renderer->stamp(command);
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

id<MTLLibrary> MetalRenderer::composeLibrary(const char* defines, bool generic) {
    return library([NSString stringWithFormat:@"%s%s%s%s%s", defines, composeSource, generic ? genericPrelude : "", generic ? genericSource : "", composeKernel]);
}

id<MTLComputePipelineState> MetalRenderer::pipeline(id<MTLLibrary> from, ShaderOutput output, u32 side, id<MTLFunction> linked) {
    MTLFunctionConstantValues* constants = [[MTLFunctionConstantValues alloc] init];
    int encoding = output == ShaderOutput::Srgb ? 0 : 2;
    bool wideOutput = output == ShaderOutput::WideLinear;
    float white = outputWhiteNits(output);
    NSError* error = nil;
    [constants setConstantValue:&encoding type:MTLDataTypeInt atIndex:0];
    [constants setConstantValue:&wideOutput type:MTLDataTypeBool atIndex:1];
    [constants setConstantValue:&white type:MTLDataTypeFloat atIndex:2];
    id<MTLFunction> function = [from newFunctionWithName:@"compose" constantValues:constants error:&error];
    if (!function) {
        fail(StringView(StringBuilder() << StringView(u8"Metal compositor function: ") << StringView(error ? error.localizedDescription.UTF8String : "missing")));
    }
    MTLComputePipelineDescriptor* descriptor = [[MTLComputePipelineDescriptor alloc] init];
    descriptor.computeFunction = function;
    descriptor.maxTotalThreadsPerThreadgroup = (NSUInteger)side * side;
    if (linked) {
        MTLLinkedFunctions* functions = [MTLLinkedFunctions linkedFunctions];
        functions.privateFunctions = @[ linked ];
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
        MetalTexture* texture = static_cast<MetalTexture*>(t.textures[i]);
        ids[i] = texture->texture.gpuResourceID;
        [compute useResource:texture->texture usage:MTLResourceUsageRead];
    }
    [compute setBuffer:textures offset:0 atIndex:6];
    [compute setTexture:target atIndex:0];
    Call call{{(i32)target.width, (i32)target.height}, {0, 0}, {0, 0}, t.tilesX, 0, textureWhiteNits};
    u8 block[callBytes];
    for (u32 p = 0; p * 2 < t.programs.length(); p++) {
        u32 count = t.programs[p * 2 + 1];
        if (!count) {
            continue;
        }
        call.first = t.programs[p * 2];
        if (p == 0) {
            if (!plain[(u32)output]) {
                plain[(u32)output] = pipeline(plainLibrary, output, composeGroup, nil);
            }
            fillCall(hostParameters, hostParameterCount, call, block);
            [compute setComputePipelineState:plain[(u32)output]];
            [compute setBytes:block length:callBytes atIndex:5];
            [compute dispatchThreadgroups:MTLSizeMake(count, (composeTile / composeGroup) * (composeTile / composeGroup), 1) threadsPerThreadgroup:MTLSizeMake(composeGroup, composeGroup, 1)];
            continue;
        }
        const Placement& placed = t.layers[(p - 1) / 3];
        ShadedImage* image = static_cast<ShadedImage*>(placed.image);
        ShaderOptions options{ShaderTarget::Air, output, (ShaderTiles)((p - 1) % 3), {(u32)(placed.box[2] - placed.box[0]), (u32)(placed.box[3] - placed.box[1])}};
        MetalShader& shader = static_cast<MetalShader&>(image->factory->shader(options));
        call.video[0] = placed.box[0];
        call.video[1] = placed.box[1];
        call.box[0] = placed.box[2] - placed.box[0];
        call.box[1] = placed.box[3] - placed.box[1];
        fillCall(shader.parameters, shader.parameterCount, call, block);
        [compute setComputePipelineState:shader.pipeline];
        for (u32 i = 0; i < shader.parameterCount; i++) {
            if (shader.parameters[i].input == ShaderInput::Words) {
                [compute setBuffer:image->host.buffer offset:0 atIndex:7];
            } else if (shader.parameters[i].input == ShaderInput::Constant) {
                [compute setBuffer:shader.constants offset:0 atIndex:8];
            }
        }
        [compute setBytes:block length:callBytes atIndex:5];
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
        TextureImage* image = pool.make<TextureImage>();
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
        TextureImage* image = pool.make<TextureImage>();
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

void MetalRenderer::stamp(id<MTLCommandBuffer> command) {
    for (ComposeTexture* texture : tiles.textures) {
        static_cast<MetalTexture*>(texture)->lastUse = command;
    }
}

bool MetalRenderer::beginFrame(u32 width, u32 height) {
    @autoreleasepool {
        poll();
        checkCommand(last);
        layer.drawableSize = CGSizeMake(width, height);
        layer.presentsWithTransaction = window.inLiveResize;
        drawable = [layer nextDrawable];
        return drawable != nil;
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
            image->record(command);
            image->lastUse = command;
        }
        Flight* flight = smallObjects->make<Flight>(drawn);
        drawn.clear();
        tiles.compose(draw, Vector<Layer>(), (u32)drawable.texture.width, (u32)drawable.texture.height, wide, ++frames);
        stamp(command);
        encode(command, drawable.texture, wide ? ShaderOutput::WideLinear : ShaderOutput::Srgb);
        Channel* done = landed;
        plt::LoopWake* completed = wake;
        [command addCompletedHandler:^(id<MTLCommandBuffer>) {
          done->enqueue(flight);
          completed->signal();
        }];
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
    }
    return true;
}

RenderImage* MetalRenderer::bind(ObjPool& pool, u32 width, u32 height, const void* data, size_t size, size_t stride, Runable& retired) {
    checkImageSize(width, height, maxTextureSide());
    if (!data || stride < (size_t)width * 4 || stride > size / height) {
        fail(StringView(u8"invalid bound image buffer"));
    }
    @autoreleasepool {
        BoundImage* image = pool.make<BoundImage>();
        size_t packed = ((size_t)width * 4 + 255) & ~(size_t)255;
        image->renderer = this;
        image->width = width;
        image->height = height;
        image->retired = &retired;
        image->sourceStride = stride;
        allocateHost(image->host, data, size, packed * height, stride % 256 == 0);
        image->bufferStride = image->host.imported ? stride : packed;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageShaderRead;
        image->texture.texture = [device newTextureWithDescriptor:descriptor];
        if (!image->host.buffer || !image->texture.texture) {
            fail(StringView(u8"cannot allocate bound Metal image"));
        }
        return image;
    }
}

// Takes the producer's pages as the buffer when they are whole pages and
// the caller allows it, otherwise allocates a shared buffer of bytes.
void MetalRenderer::allocateHost(HostBuffer& host, const void* source, size_t size, size_t bytes, bool importable) {
    size_t page = (size_t)getpagesize();
    host.source = source;
    host.bytes = bytes;
    if (importable && (uintptr_t)source % page == 0 && size % page == 0) {
        host.buffer = [device newBufferWithBytesNoCopy:const_cast<void*>(source) length:size options:MTLResourceStorageModeShared deallocator:nil];
        host.imported = host.buffer != nil;
    }
    if (!host.imported) {
        host.buffer = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    }
}

RenderShader* MetalRenderer::compileKernel(ObjPool& pool, const CompiledShader& compiled, u32 tile, const ShaderOptions& options) {
    if (tile != composeTile) {
        fail(StringView(u8"a kernel is not made for the compositor's tile"));
    }
    if (compiled.code.empty() && !compiled.constants.length()) {
        fail(StringView(u8"a shader without code or constants"));
    }
    @autoreleasepool {
        MetalShader* shader = pool.make<MetalShader>();
        bool mixed = options.tiles == ShaderTiles::Mixed;
        if (compiled.parameterCount) {
            ShaderParameter* parameters = (ShaderParameter*)pool.allocate(compiled.parameterCount * sizeof(ShaderParameter));
            memcpy(parameters, compiled.parameters, compiled.parameterCount * sizeof(ShaderParameter));
            shader->parameters = parameters;
            shader->parameterCount = compiled.parameterCount;
        }
        if (compiled.constants.length()) {
            shader->constants = [device newBufferWithBytes:compiled.constants.data() length:compiled.constants.length() options:MTLResourceStorageModeShared];
            if (!shader->constants) {
                fail(StringView(u8"cannot allocate a shader's constants"));
            }
        }
        if (compiled.code.empty()) {
            __strong id<MTLComputePipelineState>* made = &genericLayer[(u32)options.output];
            if (!*made) {
                if (!genericLayeredLibrary) {
                    genericLayeredLibrary = composeLibrary("#define GROUP 24\n#define GENERIC 1\n#define LAYER 1\n", true);
                }
                *made = pipeline(genericLayeredLibrary, options.output, composeTile, nil);
            }
            shader->pipeline = *made;
            return shader;
        }
        NSError* error = nil;
        dispatch_data_t bytes = dispatch_data_create(compiled.code.data(), compiled.code.length(), nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        id<MTLLibrary> made = [device newLibraryWithData:bytes error:&error];
        if (!made) {
            fail(StringView(StringBuilder() << StringView(u8"Metal shader: ") << StringView(error ? error.localizedDescription.UTF8String : "no library")));
        }
        if (!mixed) {
            shader->pipeline = pipeline(made, options.output, composeTile, nil);
            return shader;
        }
        id<MTLFunction> layer = [made newFunctionWithName:@"layer"];
        if (!layer) {
            fail(StringView(u8"Metal shader: no layer function"));
        }
        if (!layeredLibrary) {
            layeredLibrary = composeLibrary("#define GROUP 24\n#define LAYER 1\n", false);
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
        ShadedImage* image = pool.make<ShadedImage>();
        image->renderer = this;
        image->hdr = imageHdr;
        image->width = width;
        image->height = height;
        image->factory = &factory;
        image->retired = &retired;
        allocateHost(image->host, data, size, size, true);
        if (!image->host.buffer) {
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

static Renderer* createRenderer(ObjPool& pool, plt::Platform& platform, plt::Window& window, const RendererOptions& options) {
    plt::RenderContext context = window.renderContext();
    MetalRenderer* renderer = pool.make<MetalRenderer>();
    renderer->host = &window;
    renderer->layer = (__bridge CAMetalLayer*)context.connection;
    renderer->window = (__bridge NSWindow*)context.window;
    renderer->device = MTLCreateSystemDefaultDevice();
    renderer->textureWhiteNits = options.textureWhiteNits;
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
    fillSrgbTable();
    NSScreen* screen = renderer->window.screen ? renderer->window.screen : NSScreen.mainScreen;
    renderer->edr = screen.maximumPotentialExtendedDynamicRangeColorComponentValue > 1.0;
    CAMetalLayer* layer = renderer->layer;
    layer.device = renderer->device;
    layer.framebufferOnly = NO;
    layer.opaque = NO;
    layer.maximumDrawableCount = drawables;
    layer.allowsNextDrawableTimeout = NO;
    layer.presentsWithTransaction = NO;
    renderer->setMode(false);
    renderer->plainLibrary = renderer->composeLibrary("#define GROUP 8\n", false);
    renderer->genericLayeredLibrary = renderer->composeLibrary("#define GROUP 24\n#define GENERIC 1\n#define LAYER 1\n", true);
    renderer->plain[(u32)ShaderOutput::Srgb] = renderer->pipeline(renderer->plainLibrary, ShaderOutput::Srgb, composeGroup, nil);
    renderer->wake = platform.createLoopWake(pool, *pool.make<PollMetal>(renderer));
    renderer->smallObjects = SmallObjAllocator::create(&pool);
    renderer->landed = Channel::create(&pool, 64);
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

#else

    #include "error.h"
    #include "pooled.h"
    #include "shader.h"
    #include "renderer.h"

    #include <std/alg/defer.h>
    #include <std/dbg/verify.h>
    #include <std/lib/list.h>
    #include <std/lib/vector.h>
    #include <std/str/builder.h>
    #include <std/thr/runable.h>
    #include <std/mem/obj_pool.h>
    #include <std/mem/small_obj_allocator.h>

    #include <math.h>
    #include <fcntl.h>
    #include <stdlib.h>
    #include <string.h>
    #include <unistd.h>
    #include <plt/poller.h>
    #include <plt/window.h>
    #include <plt/platform.h>
    #include <vulkan/vulkan.h>
    #include <wayland-client.h>
    #include <compose_comp.spv.h>
    #include <vulkan/vulkan_wayland.h>
    #include <compose_layer_comp.spv.h>
    #include <compose_generic_layer_comp.spv.h>

using namespace stl;

namespace {
    struct VulkanChaos {
        virtual void memoryTypes(VkPhysicalDeviceMemoryProperties& props) = 0;
        virtual VkResult vulkan(VkResult result) = 0;
        virtual VkResult vulkanAt(stl::StringView site, VkResult result) = 0;
        virtual bool deviceExtension(const char* name, bool offered) = 0;
        virtual VkResult swapchain(VkResult result) = 0;
        virtual u32 count(stl::StringView what, u32 count) = 0;
        virtual VkPhysicalDeviceType deviceType(VkPhysicalDeviceType type) = 0;
        virtual VkBool32 surfaceSupport(VkBool32 supported) = 0;
        virtual void imageCounts(VkSurfaceCapabilitiesKHR& caps) = 0;

        static VulkanChaos* create(stl::ObjPool& pool);
    };

    constexpr u32 composeBuffers = 5;

    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* map = nullptr;
        VkDeviceSize size = 0;
    };

    // A producer's memory as the GPU reads it: the producer's own pages
    // imported as device memory when the driver takes them, otherwise a
    // mapped buffer the image copies the source into before each frame.
    struct HostBuffer: Buffer {
        const void* source = nullptr;
        size_t bytes = 0;
        bool imported = false;
        bool coherent = true;
    };

    struct Texture: ComposeTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        u64 lastUse = 0;
    };

    struct Frame {
        VkImage image;
        VkImageView view;
        VkCommandPool commandPool;
        VkCommandBuffer commandBuffer;
        VkFence fence;
        VkSemaphore rendered;
        u64 serial;
        VkDescriptorSet set;
        Buffer buffers[composeBuffers];
    };

    struct Sync {
        VkSemaphore acquired;
        u64 serial;
    };

    struct Presenter {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSurfaceFormatKHR sdr = {};
        VkSurfaceFormatKHR pq = {};
        bool hdr = false;
        bool wide = false;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        stl::Vector<Frame> frames;
        stl::Vector<Sync> syncs;
        u32 frameIndex = 0;
        u32 syncIndex = 0;
        int width = 0;
        int height = 0;
    };

    struct GpuOptions {
        VulkanChaos* chaos = nullptr;
        bool sharedBuffer = false;
        const u8* deviceUuid = nullptr;
    };

    struct VulkanImage;
    struct Gpu;

    struct Flight final: public plt::PollCallback, public IntrusiveNode {
        Gpu* gpu;
        u64 serial;
        plt::PollWaiter waiter;
        Vector<VulkanImage*> images;

        Flight(Gpu* gpu, u64 serial, int fd);
        void ready(PollFD event) override;
    };

    struct PollGpu final: public plt::TimerCallback {
        Gpu* gpu;
        explicit PollGpu(Gpu* gpu);
        void ready() override;
    };

    struct Gpu {
        VulkanChaos* chaos = nullptr;

        VkAllocationCallbacks* alloc = nullptr;
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        u32 queueFamily = (u32)-1;
        VkQueue queue = VK_NULL_HANDLE;
        VkDescriptorPool descPool = VK_NULL_HANDLE;
        Presenter present;
        plt::Platform* platform = nullptr;
        plt::Window* window = nullptr;
        PollGpu* timer = nullptr;
        SmallObjAllocator* smallObjects = nullptr;
        PFN_vkGetFenceFdKHR fenceFd = nullptr;
        Vector<VulkanImage*> drawn;
        IntrusiveList flying;
        u64 submitted = 0;
        u64 completed = 0;
        u64 frames = 0;
        bool acquired = false;
        bool retry = false;
        bool rebuild = false;
        PFN_vkGetMemoryHostPointerPropertiesEXT hostProperties = nullptr;
        VkDeviceSize hostAlignment = 0;
        float textureWhiteNits = 203.f;
        bool colorSpaces = false;
        PFN_vkCmdPushDescriptorSetKHR pushDescriptorSet = nullptr;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout composeSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout wordsSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout composeLayout = VK_NULL_HANDLE;
        VkPipeline plain[outputs] = {};
        VkPipeline genericLayer[outputs] = {};
        VkDescriptorSet readSet = VK_NULL_HANDLE;
        Buffer readBuffers[composeBuffers];
        Tiles tiles;

        static Gpu* create(stl::ObjPool& pool, const GpuOptions& options);

        void vkc(VkResult e);
        void vkcAt(stl::StringView site, VkResult e);

        VkSurfaceKHR createSurface(plt::Window& window);
        void setupWindow(stl::ObjPool& pool, VkSurfaceKHR surface, int w, int h);
        void stamp(u64 serial);
        void setupCompose(stl::ObjPool& pool);

        u32 findMemoryType(u32 typeBits, VkMemoryPropertyFlags props);
        void settle(u64 serial);
        template <typename F>
        void oneShot(F&& record);
        void createTexture(u32 w, u32 h, Texture& tex, VkFormat format, VkImageUsageFlags usage = 0);
        void writeTexture(Texture& tex, const u8* pixels, size_t pitch, u32 x, u32 y, u32 w, u32 h, bool fresh);
        void readTexture(VkImage image, VkImageLayout held, int x, int y, u32 w, u32 h, PixelLayout layout, ImagePixels& out);
        void destroyTexture(Texture& tex);
        bool reserve(Buffer& buffer, VkDeviceSize size);
        void releaseBuffer(Buffer& buffer);
        bool importHost(HostBuffer& host, size_t size, VkBufferUsageFlags usage);
        void allocateHost(HostBuffer& host, const void* source, size_t size, size_t bytes, VkBufferUsageFlags usage);
        void flushHost(HostBuffer& host);
        VkPipeline pipeline(const u32* code, size_t bytes, ShaderOutput output);
        void bindCompose(VkDescriptorSet set, Buffer (&buffers)[composeBuffers], VkImageView target, bool content);
        void dispatch(VkCommandBuffer command, VkDescriptorSet set, u32 width, u32 height, ShaderOutput output);

        void createSwapchain(u32 width, u32 height);
        void switchMode(bool wide);
        bool acquireFrame(u64 timeout);
        void track(VkFence fence);
        void landed(Flight* flight);
        void recordImages(VkCommandBuffer command);
        VkShaderModule shaderModule(const u32* code, size_t bytes);
        void updateTextures(ImDrawData* draw);
        void frameRender(ImDrawData* draw);
        void framePresent();

    private:
        void setupVulkan(stl::ObjPool& pool, const GpuOptions& options);
        bool hasDeviceExtension(VkPhysicalDevice candidate, const char* name);
        VkPhysicalDevice selectPhysicalDevice();
        u32 selectQueueFamily(VkPhysicalDevice candidate);
        bool selectSurfaceFormat(VkSurfaceKHR surface, const VkFormat* wanted, u32 nwanted, VkColorSpaceKHR colorSpace, VkSurfaceFormatKHR& out);
        bool storable(VkFormat format);
        void destroyFrames();
        void destroyPresenter();
    };
}

    #ifdef IM_FOR_TESTS
namespace {
    struct NamedCount {
        StringView what;
        u32 count;
    };

    struct TestVulkanChaos: public VulkanChaos {
        int memoryFaults = 0;
        int vulkanSkip = -1;
        Vector<StringView> failingSites;
        Vector<StringView> hiddenExtensions;
        int swapchainSkip = -1;
        VkResult swapchainFault = VK_SUCCESS;
        Vector<NamedCount> counts;
        bool discreteGpu = false;
        bool noWsi = false;
        bool imageCountsSet = false;
        u32 minImages = 0;
        u32 maxImages = 0;

        explicit TestVulkanChaos(StringView script);

        void arm(StringView script);
        void armFault(StringView fault, StringView arg);

        void memoryTypes(VkPhysicalDeviceMemoryProperties& props) override;
        VkResult vulkan(VkResult result) override;
        VkResult vulkanAt(StringView site, VkResult result) override;
        bool deviceExtension(const char* name, bool offered) override;
        VkResult swapchain(VkResult result) override;
        u32 count(StringView what, u32 count) override;
        VkPhysicalDeviceType deviceType(VkPhysicalDeviceType type) override;
        VkBool32 surfaceSupport(VkBool32 supported) override;
        void imageCounts(VkSurfaceCapabilitiesKHR& caps) override;
    };

    static bool spend(int& count) {
        if (count <= 0) {
            return false;
        }

        count--;

        return true;
    }

    static bool failsOnce(int& skip) {
        if (skip < 0) {
            return false;
        }

        return skip-- == 0;
    }
}

TestVulkanChaos::TestVulkanChaos(StringView script) {
    arm(script);
}

void TestVulkanChaos::arm(StringView script) {
    while (!script.empty()) {
        StringView word, rest, fault, arg;

        if (script.split(' ', word, rest)) {
            script = rest;
        } else {
            word = script;
            script = {};
        }

        if (word.split('=', fault, arg)) {
            armFault(fault, arg);
        }
    }
}

void TestVulkanChaos::armFault(StringView fault, StringView arg) {
    if (fault == StringView(u8"memory-types")) {
        memoryFaults = (int)arg.stou();
    } else if (fault == StringView(u8"vulkan")) {
        vulkanSkip = (int)arg.stou();
    } else if (fault == StringView(u8"vulkan-at")) {
        failingSites.pushBack(arg);
    } else if (fault == StringView(u8"no-ext")) {
        hiddenExtensions.pushBack(arg);
    } else if (fault == StringView(u8"swapchain") || fault == StringView(u8"swapchain-suboptimal")) {
        swapchainSkip = (int)arg.stou();
        swapchainFault = fault == StringView(u8"swapchain") ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUBOPTIMAL_KHR;
    } else if (fault == StringView(u8"count")) {
        StringView what, n;

        if (arg.split(':', what, n)) {
            counts.pushBack({what, (u32)n.stou()});
        }
    } else if (fault == StringView(u8"discrete-gpu")) {
        discreteGpu = true;
    } else if (fault == StringView(u8"no-wsi")) {
        noWsi = true;
    } else if (fault == StringView(u8"image-counts")) {
        StringView lo, hi;

        if (arg.split(':', lo, hi)) {
            imageCountsSet = true;
            minImages = (u32)lo.stou();
            maxImages = (u32)hi.stou();
        }
    }
}

void TestVulkanChaos::memoryTypes(VkPhysicalDeviceMemoryProperties& props) {
    if (spend(memoryFaults)) {
        props.memoryTypeCount = 0;
    }
}

VkResult TestVulkanChaos::vulkan(VkResult result) {
    if (!failsOnce(vulkanSkip)) {
        return result;
    }

    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

VkResult TestVulkanChaos::vulkanAt(StringView site, VkResult result) {
    for (StringView failing : failingSites) {
        if (failing == site) {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
    }

    return result;
}

bool TestVulkanChaos::deviceExtension(const char* name, bool offered) {
    for (StringView hidden : hiddenExtensions) {
        if (hidden == StringView(name)) {
            return false;
        }
    }

    return offered;
}

VkResult TestVulkanChaos::swapchain(VkResult result) {
    if (!failsOnce(swapchainSkip)) {
        return result;
    }

    return swapchainFault;
}

u32 TestVulkanChaos::count(StringView what, u32 count) {
    for (const NamedCount& named : counts) {
        if (named.what == what) {
            return named.count;
        }
    }

    return count;
}

VkPhysicalDeviceType TestVulkanChaos::deviceType(VkPhysicalDeviceType type) {
    return discreteGpu ? VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU : type;
}

VkBool32 TestVulkanChaos::surfaceSupport(VkBool32 supported) {
    return noWsi ? VK_FALSE : supported;
}

void TestVulkanChaos::imageCounts(VkSurfaceCapabilitiesKHR& caps) {
    if (imageCountsSet) {
        caps.minImageCount = minImages;
        caps.maxImageCount = maxImages;
    }
}

VulkanChaos* VulkanChaos::create(ObjPool& pool) {
    const char* script = getenv("IM_CHAOS");

    return pool.make<TestVulkanChaos>(StringView(script ? script : ""));
}
    #else
namespace {
    struct IdleVulkanChaos: public VulkanChaos {
        void memoryTypes(VkPhysicalDeviceMemoryProperties& props) override;
        VkResult vulkan(VkResult result) override;
        VkResult vulkanAt(StringView site, VkResult result) override;
        bool deviceExtension(const char* name, bool offered) override;
        VkResult swapchain(VkResult result) override;
        u32 count(StringView what, u32 count) override;
        VkPhysicalDeviceType deviceType(VkPhysicalDeviceType type) override;
        VkBool32 surfaceSupport(VkBool32 supported) override;
        void imageCounts(VkSurfaceCapabilitiesKHR& caps) override;
    };
}

void IdleVulkanChaos::memoryTypes(VkPhysicalDeviceMemoryProperties&) {
}

VkResult IdleVulkanChaos::vulkan(VkResult result) {
    return result;
}

VkResult IdleVulkanChaos::vulkanAt(StringView, VkResult result) {
    return result;
}

bool IdleVulkanChaos::deviceExtension(const char*, bool offered) {
    return offered;
}

VkResult IdleVulkanChaos::swapchain(VkResult result) {
    return result;
}

u32 IdleVulkanChaos::count(StringView, u32 count) {
    return count;
}

VkPhysicalDeviceType IdleVulkanChaos::deviceType(VkPhysicalDeviceType type) {
    return type;
}

VkBool32 IdleVulkanChaos::surfaceSupport(VkBool32 supported) {
    return supported;
}

void IdleVulkanChaos::imageCounts(VkSurfaceCapabilitiesKHR&) {
}

VulkanChaos* VulkanChaos::create(ObjPool& pool) {
    return pool.make<IdleVulkanChaos>();
}
    #endif

namespace {
    constexpr u32 kMinImageCount = 3;
    constexpr u32 maxTextureCount = 16384;
    constexpr u32 maxFrames = 16;

    struct DmaImage final: SharedImage {
        int fd = -1;
        u32 format = 0;
        u32 offset = 0;
        u32 stride = 0;
        u64 modifier = 0;
        u64 allocationSize = 0;
        u8 deviceUuid[VK_UUID_SIZE] = {};
    };

    struct VulkanShader final: RenderShader {
        Gpu* gpu = nullptr;
        VkPipeline pipeline = VK_NULL_HANDLE;
        bool shared = false;
        Buffer constants;
        const ShaderParameter* parameters = nullptr;
        u32 parameterCount = 0;
        u64 lastUse = 0;

        ~VulkanShader() noexcept;
    };

    // An image the frame draws, with the commands it records into the frame
    // before the compositor runs.
    struct VulkanImage: Image {
        Gpu* gpu = nullptr;
        u64 lastUse = 0;

        ~VulkanImage() noexcept;
        virtual void record(VkCommandBuffer command);
    };

    // A texture filled once: uploaded pixels or an imported screenshot.
    struct TextureImage: VulkanImage {
        Texture texture;
        PixelLayout layout = PixelLayout::Rgba8;

        ~TextureImage() noexcept;
        void draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) override;
        void read(int x0, int y0, int x1, int y1, ImagePixels& out) override;
    };

    // A texture refilled from the producer's buffer before each frame.
    struct BoundImage final: TextureImage {
        HostBuffer host;
        size_t stride = 0;
        bool initialized = false;
        bool dirty = false;

        ~BoundImage() noexcept;
        void prepare() override;
        void record(VkCommandBuffer command) override;
    };

    // The producer's words, drawn as a layer by the kernel its factory makes.
    struct ShadedImage final: VulkanImage {
        HostBuffer host;
        ShaderFactory* factory = nullptr;
        bool dirty = false;

        ~ShadedImage() noexcept;
        void prepare() override;
        void record(VkCommandBuffer command) override;
        void draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) override;
        void read(int x0, int y0, int x1, int y1, ImagePixels& out) override;
    };
}

namespace {
    static bool idOperand(u32 op, u32 i) {
        switch (op) {
            case 19:
            case 20:
            case 21:
            case 22:
            case 26:
            case 71:
            case 72:
            case 247:
                return i == 0;
            case 23:
            case 24:
            case 25:
            case 27:
            case 29:
            case 41:
            case 42:
            case 43:
            case 46:
            case 55:
                return i <= 1;
            case 32:
                return i == 0 || i == 2;
            case 54:
            case 59:
                return i != 2;
            case 61:
            case 81:
            case 250:
                return i <= 2;
            case 62:
            case 246:
                return i <= 1;
            case 12:
                return i != 3;
            default:
                return true;
        }
    }

    static bool mergeable(u32 op) {
        switch (op) {
            case 5:
            case 6:
            case 11:
            case 12:
            case 14:
            case 17:
            case 54:
            case 55:
            case 56:
            case 59:
            case 61:
            case 62:
            case 65:
            case 71:
            case 72:
            case 80:
            case 81:
            case 124:
            case 224:
            case 246:
            case 247:
            case 248:
            case 249:
            case 250:
            case 253:
            case 254:
                return true;
            default:
                return (op >= 19 && op <= 33) || (op >= 41 && op <= 46) || (op >= 109 && op <= 200);
        }
    }

    static void instructions(const u32* words, size_t count, Vector<u32>& out) {
        if (count < 5 || words[0] != 0x07230203u) {
            fail(StringView(u8"not a SPIR-V module"));
        }

        for (size_t at = 5; at < count;) {
            u32 length = words[at] >> 16;

            if (!length || at + length > count) {
                fail(StringView(u8"a broken SPIR-V module"));
            }

            out.pushBack((u32)at);
            at += length;
        }
    }

    static bool aggregate(u32 op) {
        return op == 28 || op == 29 || op == 30;
    }

    static bool isType(u32 op) {
        return op >= 19 && op <= 39;
    }

    static void remap(const u32* in, Vector<u32>& mapping, u32& bound, Vector<u32>& out) {
        u32 length = in[0] >> 16;
        u32 op = in[0] & 0xffffu;

        out.pushBack(in[0]);

        for (u32 i = 0; i + 1 < length; i++) {
            u32 value = in[i + 1];

            if (idOperand(op, i)) {
                if (value >= mapping.length()) {
                    fail(StringView(u8"a layer refers past its bound"));
                }

                if (!mapping[value]) {
                    mapping.mut(value) = bound++;
                }

                value = mapping[value];
            }

            out.pushBack(value);
        }
    }

    static void mergeLayer(const u32* host, size_t hostWords, const u32* layer, size_t layerWords, Vector<u32>& out) {
        Vector<u32> hostAt;
        Vector<u32> layerAt;

        instructions(host, hostWords, hostAt);
        instructions(layer, layerWords, layerAt);

        u32 bound = host[3];
        u32 placeholder = 0;
        u32 glsl = 0;
        size_t lastCapability = 0;
        size_t lastNote = 0;
        size_t firstFunction = hostAt.length();

        for (size_t i = 0; i < hostAt.length(); i++) {
            const u32* ins = host + hostAt[i];
            u32 op = ins[0] & 0xffffu;

            if (op == 5 && (ins[0] >> 16) > 2 && !strncmp((const char*)(ins + 2), "layer(", 6)) {
                placeholder = ins[1];
            } else if (op == 11) {
                glsl = ins[1];
            } else if (op == 17) {
                lastCapability = i;
            } else if (op == 71 || op == 72) {
                lastNote = i;
            } else if (op == 54 && firstFunction == hostAt.length()) {
                firstFunction = i;
            }
        }

        if (!placeholder || !glsl || firstFunction == hostAt.length()) {
            fail(StringView(u8"the compositor has no place for a layer"));
        }

        Vector<u32> mapping;
        Vector<u32> capabilities;
        Vector<u32> notes;
        Vector<u32> globals;
        Vector<u32> body;
        Vector<u32> params;
        bool inside = false;

        mapping.zero(layer[3]);

        for (u32 at : layerAt) {
            const u32* ins = layer + at;
            u32 length = ins[0] >> 16;
            u32 op = ins[0] & 0xffffu;

            if (!mergeable(op)) {
                fail(StringView(u8"a layer holds an instruction the compositor cannot take"));
            }

            if (op == 17) {
                bool known = false;

                for (size_t i = 0; i <= lastCapability; i++) {
                    known = known || ((host[hostAt[i]] & 0xffffu) == 17 && host[hostAt[i] + 1] == ins[1]);
                }

                if (!known) {
                    capabilities.append(ins, length);
                }

                continue;
            }

            if (op == 11) {
                mapping.mut(ins[1]) = glsl;
                continue;
            }

            if (op == 5 || op == 6 || op == 14) {
                continue;
            }

            if (op == 71 || op == 72) {
                notes.append(ins, length);
                continue;
            }

            if (!inside && (isType(op) || (op >= 41 && op <= 46) || op == 59)) {
                u32 result = isType(op) ? 1 : 2;

                if (isType(op) && !aggregate(op)) {
                    u32 found = 0;

                    for (size_t i = 0; i < firstFunction && !found; i++) {
                        const u32* other = host + hostAt[i];

                        if (other[0] != ins[0]) {
                            continue;
                        }

                        bool same = true;

                        for (u32 k = 1; k + 1 < length && same; k++) {
                            same = idOperand(op, k) ? mapping[ins[k + 1]] == other[k + 1] : ins[k + 1] == other[k + 1];
                        }

                        found = same ? other[1] : 0;
                    }

                    if (found) {
                        mapping.mut(ins[result]) = found;
                        continue;
                    }
                }

                mapping.mut(ins[result]) = bound++;
                remap(ins, mapping, bound, globals);
                continue;
            }

            if (op == 54) {
                inside = true;
                continue;
            }

            if (op == 55) {
                params.pushBack(ins[2]);
                continue;
            }

            body.append(ins, length);
        }

        out.clear();
        out.append(host, 5);

        for (size_t i = 0; i < firstFunction; i++) {
            const u32* ins = host + hostAt[i];

            out.append(ins, ins[0] >> 16);

            if (i == lastCapability) {
                out.append(capabilities.data(), capabilities.length());
            }

            if (i == lastNote) {
                for (size_t at = 0; at < notes.length(); at += notes[at] >> 16) {
                    remap(notes.data() + at, mapping, bound, out);
                }
            }
        }

        out.append(globals.data(), globals.length());

        size_t param = 0;

        inside = false;

        for (size_t i = firstFunction; i < hostAt.length(); i++) {
            const u32* ins = host + hostAt[i];
            u32 op = ins[0] & 0xffffu;

            if (op == 54 && ins[2] == placeholder) {
                inside = true;
                out.append(ins, ins[0] >> 16);
                continue;
            }

            if (!inside) {
                out.append(ins, ins[0] >> 16);
                continue;
            }

            if (op == 55) {
                if (param == params.length()) {
                    fail(StringView(u8"a layer takes other parameters than the compositor gives"));
                }

                mapping.mut(params[param++]) = ins[2];
                out.append(ins, ins[0] >> 16);
                continue;
            }

            if (op != 56) {
                continue;
            }

            inside = false;

            if (param != params.length()) {
                fail(StringView(u8"a layer takes other parameters than the compositor gives"));
            }

            for (size_t at = 0; at < body.length(); at += body[at] >> 16) {
                remap(body.data() + at, mapping, bound, out);
            }
        }

        out.mut(1) = host[1] > layer[1] ? host[1] : layer[1];
        out.mut(3) = bound;
    }
}

void Gpu::stamp(u64 serial) {
    for (ComposeTexture* texture : tiles.textures) {
        static_cast<Texture*>(texture)->lastUse = serial;
    }
}

Gpu* Gpu::create(ObjPool& pool, const GpuOptions& options) {
    Gpu* gpu = pool.make<Gpu>();

    gpu->chaos = options.chaos;
    gpu->setupVulkan(pool, options);

    return gpu;
}

bool Gpu::hasDeviceExtension(VkPhysicalDevice candidate, const char* name) {
    u32 count = 0;

    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr);
    Vector<VkExtensionProperties> props;

    props.zero(count);
    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, props.mutData());

    bool offered = false;

    for (const VkExtensionProperties& prop : props) {
        if (StringView(prop.extensionName) == StringView(name)) {
            offered = true;

            break;
        }
    }

    return chaos->deviceExtension(name, offered);
}

VkPhysicalDevice Gpu::selectPhysicalDevice() {
    u32 count = 0;

    vkc(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    count = chaos->count(StringView(u8"devices"), count);

    if (!count) {
        fail(StringView(u8"no vulkan device"));
    }

    Vector<VkPhysicalDevice> devices;

    devices.zero(count);
    vkc(vkEnumeratePhysicalDevices(instance, &count, devices.mutData()));

    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props;

        vkGetPhysicalDeviceProperties(candidate, &props);

        if (chaos->deviceType(props.deviceType) == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            return candidate;
        }
    }

    return devices[0];
}

u32 Gpu::selectQueueFamily(VkPhysicalDevice candidate) {
    u32 count = 0;

    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);

    Vector<VkQueueFamilyProperties> families;

    families.zero(count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.mutData());
    count = chaos->count(StringView(u8"queue-families"), count);

    for (u32 i = 0; i < count; i++) {
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            return i;
        }
    }

    fail(StringView(u8"no vulkan graphics queue"));
}

bool Gpu::storable(VkFormat format) {
    VkFormatProperties properties;

    vkGetPhysicalDeviceFormatProperties(phys, format, &properties);

    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

bool Gpu::selectSurfaceFormat(VkSurfaceKHR surface, const VkFormat* wanted, u32 nwanted, VkColorSpaceKHR colorSpace, VkSurfaceFormatKHR& out) {
    u32 count = 0;

    vkc(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, nullptr));
    count = chaos->count(StringView(u8"surface-formats"), count);

    if (!count) {
        fail(StringView(u8"vulkan WSI offers no surface format"));
    }

    Vector<VkSurfaceFormatKHR> available;

    available.zero(count);
    vkc(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, available.mutData()));

    for (u32 i = 0; i < nwanted; i++) {
        for (const VkSurfaceFormatKHR& format : available) {
            if (format.format == wanted[i] && format.colorSpace == colorSpace && storable(format.format)) {
                out = format;

                return true;
            }
        }
    }

    out = available[0];

    return false;
}

void Gpu::destroyFrames() {
    for (size_t i = 0; i < present.frames.length(); i++) {
        Frame& frame = present.frames.mut(i);

        for (Buffer& buffer : frame.buffers) {
            releaseBuffer(buffer);
        }
        if (frame.set) {
            vkFreeDescriptorSets(device, descPool, 1, &frame.set);
        }
        if (frame.view) {
            vkDestroyImageView(device, frame.view, alloc);
        }
        if (frame.fence) {
            vkDestroyFence(device, frame.fence, alloc);
        }
        if (frame.rendered) {
            vkDestroySemaphore(device, frame.rendered, alloc);
        }
        if (frame.commandPool) {
            vkDestroyCommandPool(device, frame.commandPool, alloc);
        }
    }

    for (size_t i = 0; i < present.syncs.length(); i++) {
        const Sync& sync = present.syncs[i];

        if (sync.acquired) {
            vkDestroySemaphore(device, sync.acquired, alloc);
        }
    }

    present.frames.clear();
    present.syncs.clear();
}

void Gpu::createSwapchain(u32 width, u32 height) {
    VkSurfaceCapabilitiesKHR caps;

    vkc(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, present.surface, &caps));
    chaos->imageCounts(caps);

    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT)) {
        fail(StringView(u8"vulkan cannot write this surface from compute"));
    }

    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)) {
        fail(StringView(u8"vulkan cannot composite this surface with its alpha"));
    }

    VkExtent2D extent = caps.currentExtent;

    if (extent.width == 0xffffffffu) {
        extent.width = width < caps.minImageExtent.width ? caps.minImageExtent.width : width > caps.maxImageExtent.width ? caps.maxImageExtent.width : width;
        extent.height = height < caps.minImageExtent.height ? caps.minImageExtent.height : height > caps.maxImageExtent.height ? caps.maxImageExtent.height : height;
    }

    u32 images = caps.minImageCount > kMinImageCount ? caps.minImageCount : kMinImageCount;

    if (caps.maxImageCount && images > caps.maxImageCount) {
        images = caps.maxImageCount;
    }

    const VkSurfaceFormatKHR& format = present.wide ? present.pq : present.sdr;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};

    ci.surface = present.surface;
    ci.minImageCount = images;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    // the frame is premultiplied with its alpha; where the alpha is short
    // the window system shows what is behind
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = present.swapchain;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;

    VkResult made = vkCreateSwapchainKHR(device, &ci, alloc, &swapchain);

    if (chaos->vulkanAt(StringView(u8"swapchain"), made) < 0) {
        if (made == VK_SUCCESS) {
            vkDestroySwapchainKHR(device, swapchain, alloc);
        }

        fail(StringView(u8"vulkan cannot make a swapchain on this surface"));
    }

    if (present.swapchain) {
        vkDeviceWaitIdle(device);
        completed = submitted;
        acquired = false;
        destroyFrames();
        vkDestroySwapchainKHR(device, present.swapchain, alloc);
    }

    present.swapchain = swapchain;
    present.width = (int)extent.width;
    present.height = (int)extent.height;
    present.frameIndex = 0;
    present.syncIndex = 0;

    u32 count = 0;

    vkc(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr));

    if (count > maxFrames) {
        fail(StringView(u8"vulkan gives a swapchain of too many images"));
    }

    Vector<VkImage> handles;

    handles.zero(count);
    vkc(vkGetSwapchainImagesKHR(device, swapchain, &count, handles.mutData()));
    present.frames.zero(count);
    present.syncs.zero(count + 1);

    for (u32 i = 0; i < count; i++) {
        Frame& frame = present.frames.mut(i);

        frame.image = handles[i];

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};

        vci.image = frame.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkc(vkCreateImageView(device, &vci, alloc, &frame.view));

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};

        pci.queueFamilyIndex = queueFamily;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkc(vkCreateCommandPool(device, &pci, alloc, &frame.commandPool));

        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

        cai.commandPool = frame.commandPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkc(vkAllocateCommandBuffers(device, &cai, &frame.commandBuffer));

        VkExportFenceCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO};
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

        exportInfo.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
        fenceInfo.pNext = &exportInfo;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkc(vkCreateFence(device, &fenceInfo, alloc, &frame.fence));
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkc(vkCreateSemaphore(device, &semaphoreInfo, alloc, &frame.rendered));

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};

        ai.descriptorPool = descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &composeSetLayout;
        vkc(vkAllocateDescriptorSets(device, &ai, &frame.set));
        bindCompose(frame.set, frame.buffers, frame.view, false);
    }

    for (u32 i = 0; i <= count; i++) {
        Sync& sync = present.syncs.mut(i);
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

        vkc(vkCreateSemaphore(device, &semaphoreInfo, alloc, &sync.acquired));
    }
}

void Gpu::destroyPresenter() {
    destroyFrames();

    if (present.swapchain) {
        vkDestroySwapchainKHR(device, present.swapchain, alloc);
    }
    if (present.surface) {
        vkDestroySurfaceKHR(instance, present.surface, alloc);
    }

    present.swapchain = VK_NULL_HANDLE;
    present.surface = VK_NULL_HANDLE;
}

VkShaderModule Gpu::shaderModule(const u32* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};

    ci.codeSize = bytes;
    ci.pCode = code;

    VkShaderModule module = VK_NULL_HANDLE;

    vkc(vkCreateShaderModule(device, &ci, alloc, &module));

    return module;
}

VkPipeline Gpu::pipeline(const u32* code, size_t bytes, ShaderOutput output) {
    VkShaderModule module = shaderModule(code, bytes);

    struct {
        u32 output;
        u32 wide;
        float white;
    } constants{output == ShaderOutput::Srgb ? 0u : output == ShaderOutput::Pq ? 1u : 2u, output == ShaderOutput::Pq || output == ShaderOutput::WideLinear ? 1u : 0u, outputWhiteNits(output)};

    VkSpecializationMapEntry entries[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec{3, entries, sizeof(constants), &constants};
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};

    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", &spec};
    ci.layout = composeLayout;

    VkPipeline made = VK_NULL_HANDLE;
    VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, alloc, &made);

    vkDestroyShaderModule(device, module, alloc);

    if (chaos->vulkanAt(StringView(u8"compose"), result) < 0 && made) {
        vkDestroyPipeline(device, made, alloc);
        made = VK_NULL_HANDLE;
    }

    vkcAt(StringView(u8"compose"), result);

    return made;
}

void Gpu::releaseBuffer(Buffer& buffer) {
    if (buffer.map) {
        vkUnmapMemory(device, buffer.memory);
    }
    if (buffer.buffer) {
        vkDestroyBuffer(device, buffer.buffer, alloc);
    }
    if (buffer.memory) {
        vkFreeMemory(device, buffer.memory, alloc);
    }

    buffer = Buffer();
}

// grows the buffer to hold size bytes; true when it is a new buffer, whose
// descriptors are to be written again: the handle may be the old one's, as
// the driver gives a freed buffer's address to the next it makes
bool Gpu::reserve(Buffer& buffer, VkDeviceSize size) {
    size = size < 256 ? 256 : size;

    if (buffer.size >= size) {
        return false;
    }

    releaseBuffer(buffer);

    VkDeviceSize capacity = 4096;

    while (capacity < size) {
        capacity *= 2;
    }

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

    info.size = capacity;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    vkc(vkCreateBuffer(device, &info, alloc, &buffer.buffer));

    VkMemoryRequirements requirements;

    vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);

    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

    memory.allocationSize = requirements.size;
    memory.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkc(vkAllocateMemory(device, &memory, alloc, &buffer.memory));
    vkc(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0));
    vkc(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.map));
    buffer.size = capacity;

    return true;
}

void Gpu::bindCompose(VkDescriptorSet set, Buffer (&buffers)[composeBuffers], VkImageView target, bool content) {
    const Tiles& t = tiles;
    const void* sources[composeBuffers] = {t.headers.data(), t.list.data(), t.ops.data(), t.triangles.data(), t.tiles.data()};
    VkDeviceSize sizes[composeBuffers] = {t.headers.length() * sizeof(Header), t.list.length() * sizeof(u32), t.ops.length() * sizeof(Op), t.triangles.length() * sizeof(Triangle), t.tiles.length() * sizeof(u32)};
    VkDescriptorBufferInfo infos[composeBuffers];
    VkWriteDescriptorSet writes[composeBuffers + 2];
    u32 written = 0;

    for (u32 i = 0; i < composeBuffers; i++) {
        bool made = reserve(buffers[i], content ? sizes[i] : 0);

        if (content && sizes[i]) {
            memcpy(buffers[i].map, sources[i], sizes[i]);
        }

        if (made) {
            infos[i] = {buffers[i].buffer, 0, VK_WHOLE_SIZE};
            writes[written] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[written].dstSet = set;
            writes[written].dstBinding = i;
            writes[written].descriptorCount = 1;
            writes[written].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[written].pBufferInfo = &infos[i];
            written++;
        }
    }

    VkDescriptorImageInfo storage{VK_NULL_HANDLE, target, VK_IMAGE_LAYOUT_GENERAL};

    if (target) {
        writes[written] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[written].dstSet = set;
        writes[written].dstBinding = composeBuffers;
        writes[written].descriptorCount = 1;
        writes[written].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[written].pImageInfo = &storage;
        written++;
    }

    Vector<VkDescriptorImageInfo> sampled;

    for (size_t i = 0; content && i < t.textures.length(); i++) {
        sampled.pushBack(VkDescriptorImageInfo{sampler, static_cast<Texture*>(t.textures[i])->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }

    if (!sampled.empty()) {
        writes[written] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[written].dstSet = set;
        writes[written].dstBinding = composeBuffers + 1;
        writes[written].descriptorCount = (u32)sampled.length();
        writes[written].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[written].pImageInfo = sampled.data();
        written++;
    }

    if (written) {
        vkUpdateDescriptorSets(device, written, writes, 0, nullptr);
    }
}

void Gpu::dispatch(VkCommandBuffer command, VkDescriptorSet set, u32 width, u32 height, ShaderOutput output) {
    const Tiles& t = tiles;
    Call call{{(i32)width, (i32)height}, {0, 0}, {0, 0}, t.tilesX, 0, textureWhiteNits};
    u8 block[callBytes];
    u32 groups = (composeTile / composeGroup) * (composeTile / composeGroup);

    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, composeLayout, 0, 1, &set, 0, nullptr);

    for (u32 p = 0; p * 2 < t.programs.length(); p++) {
        u32 count = t.programs[p * 2 + 1];

        if (!count) {
            continue;
        }

        call.first = t.programs[p * 2];

        if (p == 0) {
            if (!plain[(u32)output]) {
                plain[(u32)output] = pipeline(compose_comp_spv, sizeof(compose_comp_spv), output);
            }

            fillCall(hostParameters, hostParameterCount, call, block);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, plain[(u32)output]);
            vkCmdPushConstants(command, composeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, callBytes, block);
            vkCmdDispatch(command, count, groups, 1);
            continue;
        }

        const Placement& placed = t.layers[(p - 1) / 3];
        ShadedImage* image = static_cast<ShadedImage*>(placed.image);
        ShaderOptions options{ShaderTarget::Spirv, output, (ShaderTiles)((p - 1) % 3), {(u32)(placed.box[2] - placed.box[0]), (u32)(placed.box[3] - placed.box[1])}};
        VulkanShader& shader = static_cast<VulkanShader&>(image->factory->shader(options));
        VkDescriptorBufferInfo bound[2] = {{image->host.buffer, 0, VK_WHOLE_SIZE}, {shader.constants.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[2];
        u32 written = 0;

        shader.lastUse = submitted + 1;
        call.video[0] = placed.box[0];
        call.video[1] = placed.box[1];
        call.box[0] = placed.box[2] - placed.box[0];
        call.box[1] = placed.box[3] - placed.box[1];
        fillCall(shader.parameters, shader.parameterCount, call, block);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, shader.pipeline);

        for (u32 i = 0; i < shader.parameterCount; i++) {
            ShaderInput input = shader.parameters[i].input;

            if (input != ShaderInput::Words && input != ShaderInput::Constant) {
                continue;
            }

            writes[written] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[written].dstBinding = input == ShaderInput::Words ? 0 : 1;
            writes[written].descriptorCount = 1;
            writes[written].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[written].pBufferInfo = &bound[input == ShaderInput::Words ? 0 : 1];
            written++;
        }

        if (written) {
            pushDescriptorSet(command, VK_PIPELINE_BIND_POINT_COMPUTE, composeLayout, 1, written, writes);
        }

        vkCmdPushConstants(command, composeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, callBytes, block);
        vkCmdDispatch(command, count, 1, 1);
    }
}

bool Gpu::acquireFrame(u64 timeout) {
    Sync& sync = present.syncs.mut(present.syncIndex);
    if (sync.serial > completed) {
        retry = true;
        return false;
    }
    if (!acquired) {
        VkResult e = chaos->swapchain(vkAcquireNextImageKHR(device, present.swapchain, timeout, sync.acquired, VK_NULL_HANDLE, &present.frameIndex));
        if (e == VK_NOT_READY || e == VK_TIMEOUT || e == VK_ERROR_OUT_OF_DATE_KHR) {
            rebuild = e == VK_ERROR_OUT_OF_DATE_KHR;
            retry = true;
            platform->poller()->timeout(1000, *timer);
            return false;
        }
        vkc(e);
        rebuild = e == VK_SUBOPTIMAL_KHR;
        acquired = true;
    }
    Frame& frame = present.frames.mut(present.frameIndex);
    if (frame.serial > completed) {
        retry = true;
        return false;
    }
    retry = false;
    return true;
}

void Gpu::switchMode(bool wide) {
    Sync& sync = present.syncs.mut(present.syncIndex);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};

    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sync.acquired;
    si.pWaitDstStageMask = &stage;
    vkc(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    vkc(vkQueueWaitIdle(queue));
    acquired = false;
    present.wide = wide;
    createSwapchain((u32)present.width, (u32)present.height);

    if (!acquireFrame(UINT64_MAX)) {
        fail(StringView(u8"vulkan gives no image of the new swapchain"));
    }
}

void Gpu::frameRender(ImDrawData* draw) {
    Sync& sync = present.syncs.mut(present.syncIndex);
    Frame& fd = present.frames.mut(present.frameIndex);

    vkc(vkResetFences(device, 1, &fd.fence));
    vkc(vkResetCommandPool(device, fd.commandPool, 0));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkc(vkBeginCommandBuffer(fd.commandBuffer, &bi));
    recordImages(fd.commandBuffer);
    tiles.compose(draw, Vector<Layer>(), (u32)present.width, (u32)present.height, present.wide, ++frames);
    stamp(submitted + 1);
    bindCompose(fd.set, fd.buffers, VK_NULL_HANDLE, true);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = fd.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(fd.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    dispatch(fd.commandBuffer, fd.set, (u32)present.width, (u32)present.height, present.wide ? ShaderOutput::Pq : ShaderOutput::Srgb);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(fd.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};

    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sync.acquired;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fd.commandBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &fd.rendered;
    vkc(vkEndCommandBuffer(fd.commandBuffer));
    vkc(vkQueueSubmit(queue, 1, &si, fd.fence));
    fd.serial = ++submitted;
    sync.serial = submitted;
    track(fd.fence);
}

void Gpu::framePresent() {
    Frame& frame = present.frames.mut(present.frameIndex);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};

    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &frame.rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &present.swapchain;
    pi.pImageIndices = &present.frameIndex;

    VkResult e = chaos->swapchain(vkQueuePresentKHR(queue, &pi));

    if (e == VK_ERROR_OUT_OF_DATE_KHR || e == VK_SUBOPTIMAL_KHR) {
        rebuild = true;
    }

    acquired = false;
    present.syncIndex = (present.syncIndex + 1) % (u32)present.syncs.length();
}

void Gpu::vkc(VkResult e) {
    e = chaos->vulkan(e);

    if (e < 0) {
        fail(StringView(StringBuilder() << StringView(u8"vulkan error ") << (i64)e));
    }
}

void Gpu::vkcAt(StringView site, VkResult e) {
    e = chaos->vulkanAt(site, e);

    if (e < 0) {
        fail(StringView(StringBuilder() << StringView(u8"vulkan error ") << (i64)e << StringView(u8" at ") << site));
    }
}

void Gpu::setupVulkan(ObjPool& pool, const GpuOptions& wants) {
    VkApplicationInfo app = {};

    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "im";
    app.apiVersion = VK_API_VERSION_1_2;

    Vector<const char*> instanceExts;
    u32 offered = 0;

    instanceExts.pushBack(VK_KHR_SURFACE_EXTENSION_NAME);
    instanceExts.pushBack(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
    vkEnumerateInstanceExtensionProperties(nullptr, &offered, nullptr);

    Vector<VkExtensionProperties> instanceProps;

    instanceProps.zero(offered);
    vkEnumerateInstanceExtensionProperties(nullptr, &offered, instanceProps.mutData());

    for (const VkExtensionProperties& prop : instanceProps) {
        if (StringView(prop.extensionName) == StringView(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
            instanceExts.pushBack(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
            colorSpaces = true;
        }
    }

    VkInstanceCreateInfo ci = {};

    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (u32)instanceExts.length();
    ci.ppEnabledExtensionNames = instanceExts.data();
    vkc(vkCreateInstance(&ci, alloc, &instance));
    pooledGuard(pool, [this] {
        vkDestroyInstance(instance, alloc);
    });

    if (wants.sharedBuffer) {
        u32 count = 0;

        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        Vector<VkPhysicalDevice> devices;

        devices.zero(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.mutData());

        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

            props.pNext = &ids;
            vkGetPhysicalDeviceProperties2(candidate, &props);

            if (memcmp(ids.deviceUUID, wants.deviceUuid, VK_UUID_SIZE) == 0) {
                phys = candidate;

                break;
            }
        }

        if (!phys) {
            fail(StringView(u8"shared screenshot gpu is unavailable"));
        }
    } else {
        phys = selectPhysicalDevice();
    }

    queueFamily = selectQueueFamily(phys);

    const char* wantedExts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
    u32 wantedCount = wants.sharedBuffer ? 6 : 3;
    Vector<const char*> devExts;

    for (u32 i = 0; i < wantedCount; i++) {
        if (!hasDeviceExtension(phys, wantedExts[i])) {
            fail(StringView(StringBuilder() << StringView(u8"vulkan lacks ") << StringView(wantedExts[i])));
        }

        devExts.pushBack(wantedExts[i]);
    }

    VkPhysicalDeviceExternalFenceInfo fenceQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO};
    VkExternalFenceProperties fenceSupport{VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};

    fenceQuery.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    vkGetPhysicalDeviceExternalFenceProperties(phys, &fenceQuery, &fenceSupport);

    if (!(fenceSupport.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT)) {
        fail(StringView(u8"vulkan cannot export fences as sync files"));
    }

    if (hasDeviceExtension(phys, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &host;
        vkGetPhysicalDeviceProperties2(phys, &properties);
        hostAlignment = host.minImportedHostPointerAlignment;
        devExts.pushBack(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    }

    VkPhysicalDeviceVulkan12Features offered12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 offeredAll{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

    offeredAll.pNext = &offered12;
    vkGetPhysicalDeviceFeatures2(phys, &offeredAll);

    const VkPhysicalDeviceFeatures& core = offeredAll.features;

    if (!core.shaderInt64 || !core.shaderStorageImageWriteWithoutFormat || !core.shaderSampledImageArrayDynamicIndexing || !offered12.descriptorBindingPartiallyBound) {
        fail(StringView(u8"vulkan lacks the compositor's shader features"));
    }

    VkPhysicalDeviceProperties limits;

    vkGetPhysicalDeviceProperties(phys, &limits);

    if (limits.limits.maxPerStageDescriptorSamplers < composeTextures || limits.limits.maxPerStageDescriptorSampledImages < composeTextures) {
        fail(StringView(u8"vulkan binds fewer textures than the compositor draws"));
    }

    if (limits.limits.maxComputeWorkGroupInvocations < composeTile * composeTile || limits.limits.maxComputeWorkGroupSize[0] < composeTile || limits.limits.maxComputeWorkGroupSize[1] < composeTile || limits.limits.maxComputeSharedMemorySize < kernelShared) {
        fail(StringView(u8"vulkan runs smaller workgroups than the compositor's tiles"));
    }

    VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

    enabled12.descriptorBindingPartiallyBound = VK_TRUE;
    enabled.pNext = &enabled12;
    enabled.features.shaderInt64 = VK_TRUE;
    enabled.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabled.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {};

    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci = {};

    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &enabled;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = (u32)devExts.length();
    dci.ppEnabledExtensionNames = devExts.data();
    vkc(vkCreateDevice(phys, &dci, alloc, &device));
    pooledGuard(pool, [this] {
        vkDestroyDevice(device, alloc);
    });
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    fenceFd = (PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr(device, "vkGetFenceFdKHR");
    if (!fenceFd) {
        fail(StringView(u8"vulkan lacks vkGetFenceFdKHR"));
    }
    pushDescriptorSet = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
    if (!pushDescriptorSet) {
        fail(StringView(u8"vulkan lacks vkCmdPushDescriptorSetKHR"));
    }
    if (hostAlignment) {
        hostProperties = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT");
    }
}

void Gpu::setupCompose(ObjPool& pool) {
    fillSrgbTable();

    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (maxFrames + 1) * composeBuffers},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxFrames + 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (maxFrames + 1) * composeTextures},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};

    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = maxFrames + 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    vkc(vkCreateDescriptorPool(device, &pi, alloc, &descPool));
    pooledGuard(pool, [this] {
        vkDestroyDescriptorPool(device, descPool, alloc);
    });

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};

    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.minLod = -1000;
    sci.maxLod = 1000;
    vkc(vkCreateSampler(device, &sci, alloc, &sampler));
    pooledGuard(pool, [this] {
        vkDestroySampler(device, sampler, alloc);
    });

    VkDescriptorSetLayoutBinding bindings[composeBuffers + 2];

    for (u32 i = 0; i < composeBuffers; i++) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }

    bindings[composeBuffers] = {composeBuffers, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[composeBuffers + 1] = {composeBuffers + 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, composeTextures, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorBindingFlags flags[composeBuffers + 2] = {};

    flags[composeBuffers + 1] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo bound{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};

    bound.bindingCount = composeBuffers + 2;
    bound.pBindingFlags = flags;

    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

    layout.pNext = &bound;
    layout.bindingCount = composeBuffers + 2;
    layout.pBindings = bindings;
    vkc(vkCreateDescriptorSetLayout(device, &layout, alloc, &composeSetLayout));
    pooledGuard(pool, [this] {
        vkDestroyDescriptorSetLayout(device, composeSetLayout, alloc);
    });

    VkDescriptorSetLayoutBinding words[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo pushed{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

    pushed.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    pushed.bindingCount = 2;
    pushed.pBindings = words;
    vkc(vkCreateDescriptorSetLayout(device, &pushed, alloc, &wordsSetLayout));
    pooledGuard(pool, [this] {
        vkDestroyDescriptorSetLayout(device, wordsSetLayout, alloc);
    });

    VkDescriptorSetLayout sets[2] = {composeSetLayout, wordsSetLayout};
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, callBytes};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

    pl.setLayoutCount = 2;
    pl.pSetLayouts = sets;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    vkc(vkCreatePipelineLayout(device, &pl, alloc, &composeLayout));
    pooledGuard(pool, [this] {
        for (VkPipeline made : plain) {
            if (made) {
                vkDestroyPipeline(device, made, alloc);
            }
        }

        for (VkPipeline made : genericLayer) {
            if (made) {
                vkDestroyPipeline(device, made, alloc);
            }
        }

        vkDestroyPipelineLayout(device, composeLayout, alloc);
    });

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};

    ai.descriptorPool = descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &composeSetLayout;
    vkc(vkAllocateDescriptorSets(device, &ai, &readSet));
    pooledGuard(pool, [this] {
        for (Buffer& buffer : readBuffers) {
            releaseBuffer(buffer);
        }
    });
    plain[(u32)ShaderOutput::Srgb] = pipeline(compose_comp_spv, sizeof(compose_comp_spv), ShaderOutput::Srgb);
}

VkSurfaceKHR Gpu::createSurface(plt::Window& window) {
    plt::RenderContext render = window.renderContext();
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkWaylandSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};

    sci.display = (wl_display*)render.connection;
    sci.surface = (wl_surface*)render.window;
    vkc(vkCreateWaylandSurfaceKHR(instance, &sci, alloc, &surface));

    return surface;
}

void Gpu::setupWindow(ObjPool& pool, VkSurfaceKHR surface, int w, int h) {
    present.surface = surface;
    pooledGuard(pool, [this] {
        destroyPresenter();
    });

    VkBool32 supported = VK_FALSE;

    vkc(vkGetPhysicalDeviceSurfaceSupportKHR(phys, queueFamily, surface, &supported));

    if (!chaos->surfaceSupport(supported)) {
        fail(StringView(u8"no vulkan WSI support"));
    }

    const VkFormat pqFormats[] = {
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_A2R10G10B10_UNORM_PACK32,
    };
    const VkFormat sdrFormats[] = {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
    };

    if (!selectSurfaceFormat(surface, sdrFormats, 2, VK_COLORSPACE_SRGB_NONLINEAR_KHR, present.sdr)) {
        fail(StringView(u8"vulkan WSI offers no 8-bit sRGB surface the compositor can write"));
    }

    present.hdr = colorSpaces && selectSurfaceFormat(surface, pqFormats, 2, VK_COLOR_SPACE_HDR10_ST2084_EXT, present.pq);

    if (present.hdr) {
        plain[(u32)ShaderOutput::Pq] = pipeline(compose_comp_spv, sizeof(compose_comp_spv), ShaderOutput::Pq);
    }

    createSwapchain((u32)w, (u32)h);
}

u32 Gpu::findMemoryType(u32 typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;

    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    chaos->memoryTypes(mp);

    for (u32 i = 0; i < mp.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }

    fail(StringView(u8"no vulkan memory type fits"));
}

void Gpu::createTexture(u32 w, u32 h, Texture& tex, VkFormat format, VkImageUsageFlags usage) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};

    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkc(vkCreateImage(device, &ici, alloc, &tex.image));

    VkMemoryRequirements req;

    vkGetImageMemoryRequirements(device, tex.image, &req);

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkc(vkAllocateMemory(device, &mai, alloc, &tex.memory));
    vkc(vkBindImageMemory(device, tex.image, tex.memory, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};

    vci.image = tex.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc(vkCreateImageView(device, &vci, alloc, &tex.view));
}

void Gpu::settle(u64 serial) {
    if (serial > completed) {
        vkDeviceWaitIdle(device);
        completed = submitted;
    }
}

// Records one command buffer, submits it and waits for the queue to finish
// it: the path of uploads, imports and readbacks, which are not frames.
template <typename F>
void Gpu::oneShot(F&& record) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool commands = VK_NULL_HANDLE;

    pci.queueFamilyIndex = queueFamily;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkc(vkCreateCommandPool(device, &pci, alloc, &commands));
    STD_DEFER {
        vkQueueWaitIdle(queue);
        vkDestroyCommandPool(device, commands, alloc);
    };

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

    cai.commandPool = commands;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkc(vkAllocateCommandBuffers(device, &cai, &cmd));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkc(vkBeginCommandBuffer(cmd, &begin));
    record(cmd);
    vkc(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkc(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    vkc(vkQueueWaitIdle(queue));
}

void Gpu::writeTexture(Texture& tex, const u8* pixels, size_t pitch, u32 x, u32 y, u32 w, u32 h, bool fresh) {
    Buffer staging;

    STD_DEFER {
        releaseBuffer(staging);
    };

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

    bci.size = (VkDeviceSize)w * h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkc(vkCreateBuffer(device, &bci, alloc, &staging.buffer));

    VkMemoryRequirements req;

    vkGetBufferMemoryRequirements(device, staging.buffer, &req);

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkc(vkAllocateMemory(device, &mai, alloc, &staging.memory));
    vkc(vkBindBufferMemory(device, staging.buffer, staging.memory, 0));
    vkc(vkMapMemory(device, staging.memory, 0, VK_WHOLE_SIZE, 0, &staging.map));

    for (u32 row = 0; row < h; row++) {
        memcpy((u8*)staging.map + (size_t)row * w * 4, pixels + (size_t)(y + row) * pitch + (size_t)x * 4, (size_t)w * 4);
    }

    oneShot([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

        bar.oldLayout = fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = tex.image;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        bar.srcAccessMask = fresh ? 0 : VK_ACCESS_SHADER_READ_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, fresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

        VkBufferImageCopy copy{};

        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {(i32)x, (i32)y, 0};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    });
}

// Copies a rectangle of a texture out through a host buffer and unpacks it
// into pixels, leaving the texture in the layout it held.
void Gpu::readTexture(VkImage image, VkImageLayout held, int x, int y, u32 w, u32 h, PixelLayout layout, ImagePixels& out) {
    size_t pixel = layout == PixelLayout::Rgba16f ? 8 : 4;
    VkDeviceSize bytes = (VkDeviceSize)w * h * pixel;
    Buffer readback;

    STD_DEFER {
        releaseBuffer(readback);
    };

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkc(vkCreateBuffer(device, &bci, alloc, &readback.buffer));

    VkMemoryRequirements req;

    vkGetBufferMemoryRequirements(device, readback.buffer, &req);

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkc(vkAllocateMemory(device, &mai, alloc, &readback.memory));
    vkc(vkBindBufferMemory(device, readback.buffer, readback.memory, 0));

    oneShot([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = held;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy copy = {};

        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {x, y, 0};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = held;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    });

    vkc(vkMapMemory(device, readback.memory, 0, bytes, 0, &readback.map));
    unpackPixels(readback.map, w, h, (size_t)w * pixel, layout, out);
}

void Gpu::destroyTexture(Texture& tex) {
    if (tex.view) {
        vkDestroyImageView(device, tex.view, alloc);
    }
    if (tex.image) {
        vkDestroyImage(device, tex.image, alloc);
    }
    if (tex.memory) {
        vkFreeMemory(device, tex.memory, alloc);
    }

    tex.view = VK_NULL_HANDLE;
    tex.image = VK_NULL_HANDLE;
    tex.memory = VK_NULL_HANDLE;
}

namespace {}

void Gpu::updateTextures(ImDrawData* draw) {
    if (!draw->Textures) {
        return;
    }

    for (ImTextureData* data : *draw->Textures) {
        if (data->Status == ImTextureStatus_WantCreate) {
            if (data->Format != ImTextureFormat_RGBA32) {
                fail(StringView(u8"the interface asks for a texture format the renderer does not draw"));
            }

            Texture* texture = smallObjects->make<Texture>();

            createTexture((u32)data->Width, (u32)data->Height, *texture, VK_FORMAT_R8G8B8A8_SRGB);
            writeTexture(*texture, (const u8*)data->GetPixels(), (size_t)data->GetPitch(), 0, 0, (u32)data->Width, (u32)data->Height, true);
            texture->opaque = opaquePixels((const u8*)data->GetPixels(), (size_t)data->GetPitch(), (u32)data->Width, (u32)data->Height);
            data->BackendUserData = texture;
            data->SetTexID((ImTextureID)(uintptr_t)texture);
            data->SetStatus(ImTextureStatus_OK);
        } else if (data->Status == ImTextureStatus_WantUpdates) {
            Texture* texture = (Texture*)data->BackendUserData;

            for (const ImTextureRect& rect : data->Updates) {
                writeTexture(*texture, (const u8*)data->GetPixels(), (size_t)data->GetPitch(), rect.x, rect.y, rect.w, rect.h, false);
            }

            if (data->Updates.empty()) {
                const ImTextureRect& rect = data->UpdateRect;

                writeTexture(*texture, (const u8*)data->GetPixels(), (size_t)data->GetPitch(), rect.x, rect.y, rect.w, rect.h, false);
            }

            texture->opaque = opaquePixels((const u8*)data->GetPixels(), (size_t)data->GetPitch(), (u32)data->Width, (u32)data->Height);
            data->SetStatus(ImTextureStatus_OK);
        } else if (data->Status == ImTextureStatus_WantDestroy && data->UnusedFrames >= (int)present.frames.length()) {
            Texture* texture = (Texture*)data->BackendUserData;

            settle(texture->lastUse);
            destroyTexture(*texture);
            smallObjects->release(texture);
            data->BackendUserData = nullptr;
            data->SetTexID(ImTextureID_Invalid);
            data->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

PollGpu::PollGpu(Gpu* value)
    : gpu(value)
{
}

void PollGpu::ready() {
    if (gpu->retry) {
        gpu->window->requestFrame();
    }
}

Flight::Flight(Gpu* gpu_, u64 serial_, int fd)
    : gpu(gpu_)
    , serial(serial_)
{
    waiter.fd.fd = fd;
    waiter.fd.flags = PollFlag::In;
    waiter.callback = this;

    for (VulkanImage* image : gpu->drawn) {
        if (image->retired) {
            images.pushBack(image);
        }
    }
}

void Flight::ready(PollFD) {
    close(waiter.fd.fd);
    gpu->landed(this);
}

void Gpu::track(VkFence fence) {
    VkFenceGetFdInfoKHR info{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR};
    int fd = -1;

    info.fence = fence;
    info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    vkc(fenceFd(device, &info, &fd));

    Flight* flight = smallObjects->make<Flight>(this, submitted, fd);

    drawn.clear();

    if (fd < 0) {
        landed(flight);
    } else {
        flying.pushBack(flight);
        platform->poller()->arm(flight->waiter);
    }
}

void Gpu::landed(Flight* flight) {
    Vector<VulkanImage*> done;

    if (flight->serial > completed) {
        completed = flight->serial;
    }
    flight->unlink();
    done.xchg(flight->images);
    smallObjects->release(flight);
    for (VulkanImage* image : done) {
        image->retired->run();
    }
    if (retry) {
        window->requestFrame();
    }
}

void Gpu::recordImages(VkCommandBuffer command) {
    for (VulkanImage* image : drawn) {
        image->record(command);
        image->lastUse = submitted + 1;
    }
}

bool Gpu::importHost(HostBuffer& host, size_t size, VkBufferUsageFlags usage) {
    if (!hostAlignment || (uintptr_t)host.source % hostAlignment || size % hostAlignment) {
        return false;
    }
    VkMemoryHostPointerPropertiesEXT pointer{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (hostProperties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host.source, &pointer) != VK_SUCCESS) {
        return false;
    }
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.pNext = &external;
    info.size = host.bytes;
    info.usage = usage;
    if (vkCreateBuffer(device, &info, alloc, &host.buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, host.buffer, &requirements);
    size_t allocation = (requirements.size + hostAlignment - 1) / hostAlignment * hostAlignment;
    if (allocation > size) {
        return false;
    }
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(phys, &properties);
    u32 type = UINT32_MAX;
    for (u32 i = 0; i < properties.memoryTypeCount; i++) {
        if ((requirements.memoryTypeBits & pointer.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) {
        return false;
    }
    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = const_cast<void*>(host.source);
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.pNext = &import;
    memory.allocationSize = allocation;
    memory.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &memory, alloc, &host.memory) != VK_SUCCESS) {
        return false;
    }
    if (vkBindBufferMemory(device, host.buffer, host.memory, 0) != VK_SUCCESS) {
        return false;
    }
    host.coherent = (properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (!host.coherent && vkMapMemory(device, host.memory, 0, VK_WHOLE_SIZE, 0, &host.map) != VK_SUCCESS) {
        return false;
    }
    return true;
}

void Gpu::allocateHost(HostBuffer& host, const void* source, size_t size, size_t bytes, VkBufferUsageFlags usage) {
    host.source = source;
    host.bytes = bytes;
    if (importHost(host, size, usage)) {
        host.imported = true;
        return;
    }
    releaseBuffer(host);
    host.coherent = true;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = usage;
    vkc(vkCreateBuffer(device, &info, alloc, &host.buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, host.buffer, &requirements);
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.allocationSize = requirements.size;
    memory.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkc(vkAllocateMemory(device, &memory, alloc, &host.memory));
    vkc(vkBindBufferMemory(device, host.buffer, host.memory, 0));
    vkc(vkMapMemory(device, host.memory, 0, VK_WHOLE_SIZE, 0, &host.map));
}

void Gpu::flushHost(HostBuffer& host) {
    if (host.coherent) {
        return;
    }
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = host.memory;
    range.size = VK_WHOLE_SIZE;
    vkc(vkFlushMappedMemoryRanges(device, 1, &range));
}

void VulkanImage::record(VkCommandBuffer) {
}

void BoundImage::prepare() {
    if (!host.imported) {
        memcpy(host.map, host.source, host.bytes);
    }
    dirty = true;
}

void BoundImage::record(VkCommandBuffer command) {
    if (!dirty) {
        return;
    }
    gpu->flushHost(host);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, initialized ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.bufferRowLength = (u32)(stride / 4);
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(command, host.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    initialized = true;
    dirty = false;
}

void ShadedImage::prepare() {
    if (!host.imported) {
        memcpy(host.map, host.source, host.bytes);
    }
    dirty = true;
}

void ShadedImage::record(VkCommandBuffer) {
    if (dirty) {
        gpu->flushHost(host);
        dirty = false;
    }
}

namespace {
    struct VulkanRenderer final: Renderer {
        plt::Window* window = nullptr;
        Gpu* gpu = nullptr;
        RenderImage* upload(ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) override;
        RenderImage* import(ObjPool& pool, SharedImage& source, bool hdr) override;

        RenderImage* bind(ObjPool& pool, u32 width, u32 height, const void* data, size_t size, size_t stride, Runable& retired) override;
        RenderShader* compileKernel(ObjPool& pool, const CompiledShader& compiled, u32 tile, const ShaderOptions& options) override;
        RenderImage* shade(ObjPool& pool, ShaderFactory& factory, u32 width, u32 height, const void* data, size_t size, bool hdr, Runable& retired) override;
        bool beginFrame(u32 width, u32 height) override;
        bool endFrame(ImDrawData* draw) override;
        u32 maxTextureSide() override;
        u32 maxTextures() override;
    };
}

bool VulkanRenderer::beginFrame(u32 width, u32 height) {
    if (gpu->rebuild || gpu->present.width != (int)width || gpu->present.height != (int)height) {
        gpu->createSwapchain(width, height);
        gpu->rebuild = false;
    }

    return gpu->acquireFrame(0);
}

bool VulkanRenderer::endFrame(ImDrawData* draw) {
    bool wide = false;

    gpu->updateTextures(draw);

    for (const VulkanImage* image : gpu->drawn) {
        wide = wide || image->hdr;
    }

    wide = wide && gpu->present.hdr;

    if (wide != gpu->present.wide) {
        gpu->switchMode(wide);
    }

    gpu->frameRender(draw);
    gpu->framePresent();

    if (gpu->rebuild) {
        window->requestFrame();
    }

    return !gpu->rebuild;
}

u32 VulkanRenderer::maxTextureSide() {
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();

    return (u32)(pio.Renderer_TextureMaxWidth < pio.Renderer_TextureMaxHeight ? pio.Renderer_TextureMaxWidth : pio.Renderer_TextureMaxHeight);
}

u32 VulkanRenderer::maxTextures() {
    return maxTextureCount;
}

RenderImage* VulkanRenderer::bind(ObjPool& pool, u32 width, u32 height, const void* data, size_t size, size_t stride, Runable& retired) {
    checkImageSize(width, height, maxTextureSide());
    if (!data || stride < (size_t)width * 4 || stride % 4 || stride / 4 > UINT32_MAX || stride > size / height) {
        fail(StringView(u8"invalid bound image buffer"));
    }
    BoundImage* image = pool.make<BoundImage>();
    image->gpu = gpu;
    image->width = width;
    image->height = height;
    image->stride = stride;
    image->retired = &retired;
    gpu->createTexture(width, height, image->texture, VK_FORMAT_R8G8B8A8_SRGB);
    gpu->allocateHost(image->host, data, size, stride * height, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    return image;
}

RenderShader* VulkanRenderer::compileKernel(ObjPool& pool, const CompiledShader& compiled, u32 tile, const ShaderOptions& options) {
    if (tile != composeTile) {
        fail(StringView(u8"a kernel is not made for the compositor's tile"));
    }
    const void* code = compiled.code.length() ? compiled.code.data() : nullptr;
    size_t size = compiled.code.length();
    if (code && size % 4) {
        fail(StringView(u8"invalid shader code"));
    }
    if (!code && !compiled.constants.length()) {
        fail(StringView(u8"a shader without code or constants"));
    }
    VulkanShader* shader = pool.make<VulkanShader>();
    shader->gpu = gpu;
    if (compiled.parameterCount) {
        ShaderParameter* parameters = (ShaderParameter*)pool.allocate(compiled.parameterCount * sizeof(ShaderParameter));
        memcpy(parameters, compiled.parameters, compiled.parameterCount * sizeof(ShaderParameter));
        shader->parameters = parameters;
        shader->parameterCount = compiled.parameterCount;
    }
    if (compiled.constants.length()) {
        gpu->reserve(shader->constants, compiled.constants.length());
        memcpy(shader->constants.map, compiled.constants.data(), compiled.constants.length());
    }
    if (!code) {
        VkPipeline& made = gpu->genericLayer[(u32)options.output];
        if (!made) {
            made = gpu->pipeline(compose_generic_layer_comp_spv, sizeof(compose_generic_layer_comp_spv), options.output);
        }
        shader->pipeline = made;
        shader->shared = true;
        return shader;
    }
    if (options.tiles == ShaderTiles::Mixed) {
        Vector<u32> merged;
        mergeLayer(compose_layer_comp_spv, sizeof(compose_layer_comp_spv) / 4, (const u32*)code, size / 4, merged);
        shader->pipeline = gpu->pipeline(merged.data(), merged.length() * 4, options.output);
    } else {
        shader->pipeline = gpu->pipeline((const u32*)code, size, options.output);
    }
    return shader;
}

RenderImage* VulkanRenderer::shade(ObjPool& pool, ShaderFactory& factory, u32 width, u32 height, const void* data, size_t size, bool hdr, Runable& retired) {
    checkImageSize(width, height, maxTextureSide());
    if (!data || !size || size % 4) {
        fail(StringView(u8"invalid shaded image source"));
    }
    ShadedImage* image = pool.make<ShadedImage>();
    image->gpu = gpu;
    image->width = width;
    image->height = height;
    image->hdr = hdr;
    image->factory = &factory;
    image->retired = &retired;
    gpu->allocateHost(image->host, data, size, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    return image;
}

VulkanImage::~VulkanImage() noexcept {
    for (const VulkanImage* image : gpu->drawn) {
        STD_INSIST(image != this);
    }
}

TextureImage::~TextureImage() noexcept {
    gpu->settle(texture.lastUse);
    gpu->destroyTexture(texture);
}

BoundImage::~BoundImage() noexcept {
    gpu->settle(lastUse);
    gpu->releaseBuffer(host);
}

ShadedImage::~ShadedImage() noexcept {
    gpu->settle(lastUse);
    gpu->releaseBuffer(host);
}

VulkanShader::~VulkanShader() noexcept {
    gpu->settle(lastUse);
    if (pipeline && !shared) {
        vkDestroyPipeline(gpu->device, pipeline, gpu->alloc);
    }
    gpu->releaseBuffer(constants);
}

void TextureImage::draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) {
    gpu->drawn.pushBack(this);
    list.AddImage(ImTextureRef((ImTextureID)(uintptr_t)&texture), lo, hi);
}

void TextureImage::read(int x0, int y0, int x1, int y1, ImagePixels& out) {
    checkImageRegion(width, height, x0, y0, x1, y1);
    gpu->readTexture(texture.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, x0, y0, (u32)(x1 - x0), (u32)(y1 - y0), layout, out);
}

void ShadedImage::draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) {
    gpu->drawn.pushBack(this);
    LayerDraw layer{this, {lo.x, lo.y}, {hi.x, hi.y}};
    list.AddCallback(drawLayer, &layer, sizeof(layer));
}

// Draws the image into the region of a wide target and copies the region out.
void ShadedImage::read(int x0, int y0, int x1, int y1, ImagePixels& out) {
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    u32 limit = (u32)(pio.Renderer_TextureMaxWidth < pio.Renderer_TextureMaxHeight ? pio.Renderer_TextureMaxWidth : pio.Renderer_TextureMaxHeight);
    checkImageRegion(limit, limit, x0, y0, x1, y1);
    Vector<Layer> whole;
    Texture target;

    gpu->flushHost(host);
    gpu->createTexture((u32)x1, (u32)y1, target, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
    STD_DEFER {
        gpu->destroyTexture(target);
    };
    whole.pushBack(Layer{this, {(float)x0, (float)y0}, {(float)x1, (float)y1}});
    gpu->tiles.compose(nullptr, whole, (u32)x1, (u32)y1, hdr, ++gpu->frames);
    gpu->stamp(gpu->submitted);
    gpu->bindCompose(gpu->readSet, gpu->readBuffers, target.view, true);
    gpu->oneShot([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = target.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        gpu->dispatch(cmd, gpu->readSet, (u32)x1, (u32)y1, hdr ? ShaderOutput::WideLinear : ShaderOutput::Linear);
    });
    gpu->readTexture(target.image, VK_IMAGE_LAYOUT_GENERAL, x0, y0, (u32)(x1 - x0), (u32)(y1 - y0), PixelLayout::Rgba16f, out);
}

RenderImage* VulkanRenderer::upload(ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) {
    checkImageSize(width, height, maxTextureSide());
    if (!rgba) {
        fail(StringView(u8"invalid renderer image source"));
    }
    TextureImage* image = pool.make<TextureImage>();
    image->gpu = gpu;
    image->width = width;
    image->height = height;
    image->hdr = hdr;
    image->texture.flags = hdr ? DecodePq | SourceWide : DecodeLinear;
    image->texture.opaque = opaquePixels((const u8*)rgba, (size_t)width * 4, width, height);
    gpu->createTexture(width, height, image->texture, hdr ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB);
    gpu->writeTexture(image->texture, (const u8*)rgba, (size_t)width * 4, 0, 0, width, height, true);
    return image;
}

static Renderer* createRenderer(ObjPool& pool, plt::Platform& platform, plt::Window& window, const RendererOptions& options) {
    GpuOptions wants;
    wants.chaos = VulkanChaos::create(pool);
    wants.sharedBuffer = options.shared != nullptr;
    if (options.shared) {
        wants.deviceUuid = static_cast<DmaImage*>(options.shared)->deviceUuid;
    }
    Gpu& gpu = *Gpu::create(pool, wants);
    gpu.smallObjects = SmallObjAllocator::create(&pool);
    gpu.platform = &platform;
    gpu.window = &window;
    gpu.timer = pool.make<PollGpu>(&gpu);
    gpu.textureWhiteNits = options.textureWhiteNits;
    pooledGuard(pool, [&gpu] {
        gpu.platform->poller()->cancel(*gpu.timer);
    });
    gpu.setupCompose(pool);
    VkSurfaceKHR surface = gpu.createSurface(window);
    plt::WindowInfo info = window.info();
    gpu.setupWindow(pool, surface, (int)info.width, (int)info.height);

    VkPhysicalDeviceProperties props;

    vkGetPhysicalDeviceProperties(gpu.phys, &props);

    ImGuiIO& io = ImGui::GetIO();
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();

    io.BackendRendererName = "im_compose";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    pio.Renderer_TextureMaxWidth = (int)props.limits.maxImageDimension2D;
    pio.Renderer_TextureMaxHeight = (int)props.limits.maxImageDimension2D;
    pooledGuard(pool, [&gpu] {
        vkDeviceWaitIdle(gpu.device);

        while (!gpu.flying.empty()) {
            Flight* flight = static_cast<Flight*>(gpu.flying.popFront());

            gpu.platform->poller()->cancel(flight->waiter);
            close(flight->waiter.fd.fd);
            gpu.smallObjects->release(flight);
        }

        for (ImTextureData* data : ImGui::GetPlatformIO().Textures) {
            Texture* texture = (Texture*)data->BackendUserData;

            if (texture) {
                gpu.destroyTexture(*texture);
                gpu.smallObjects->release(texture);
                data->BackendUserData = nullptr;
                data->SetTexID(ImTextureID_Invalid);
                data->SetStatus(ImTextureStatus_Destroyed);
            }
        }

        ImGuiIO& shut = ImGui::GetIO();

        shut.BackendRendererName = nullptr;
        shut.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    });
    VulkanRenderer* renderer = pool.make<VulkanRenderer>();
    renderer->gpu = &gpu;
    renderer->window = &window;
    return renderer;
}

namespace {
    static PixelLayout pixelLayout(VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM: {
                return PixelLayout::Rgba8;
            }
            case VK_FORMAT_B8G8R8A8_UNORM: {
                return PixelLayout::Bgra8;
            }
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
                return PixelLayout::Rgb10A2;
            }
            case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
                return PixelLayout::Bgr10A2;
            }
            default: {
                fail(StringView(u8"unsupported shared image format"));
            }
        }
    }

    static bool parseShared(StringView spec, DmaImage& img) {
        u64 values[7] = {};
        size_t pos = 0;

        for (int i = 0; i < 7; i++) {
            size_t begin = pos;

            while (pos < spec.length() && spec[pos] >= '0' && spec[pos] <= '9') {
                u64 digit = (u64)(spec[pos] - '0');

                if (values[i] > (UINT64_MAX - digit) / 10) {
                    return false;
                }

                values[i] = values[i] * 10 + digit;
                pos++;
            }

            if (pos == begin || pos >= spec.length() || spec[pos] != ':') {
                return false;
            }

            pos++;
        }

        if (spec.length() - pos != 2 * VK_UUID_SIZE) {
            return false;
        }

        for (size_t i = 0; i < 2 * VK_UUID_SIZE; i++) {
            char c = spec[pos + i];
            int nibble = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

            if (nibble < 0) {
                return false;
            }

            img.deviceUuid[i / 2] = (u8)(img.deviceUuid[i / 2] << 4 | nibble);
        }

        for (int i = 0; i < 5; i++) {
            if (values[i] > 0xffffffffu) {
                return false;
            }
        }
        img.width = (u32)values[0];
        img.height = (u32)values[1];
        img.format = (u32)values[2];
        img.offset = (u32)values[3];
        img.stride = (u32)values[4];
        img.modifier = values[5];
        img.allocationSize = values[6];

        return img.width && img.height && img.stride && img.allocationSize;
    }
}

static SharedImage* createSharedImage(ObjPool& pool, StringView description, intptr_t handle) {
    DmaImage* image = pool.make<DmaImage>();
    if (!parseShared(description, *image)) {
        fail(StringView(u8"bad shared screenshot metadata"));
    }
    pixelLayout((VkFormat)image->format);
    checkImageSize(image->width, image->height, 65535);
    if (handle < 0 || handle > 0x7fffffff || image->stride < (u64)image->width * 4 || image->offset > image->allocationSize || (u64)(image->height - 1) * image->stride + (u64)image->width * 4 > image->allocationSize - image->offset) {
        fail(StringView(u8"bad shared screenshot metadata"));
    }
    image->fd = fcntl((int)handle, F_DUPFD_CLOEXEC, 0);
    if (image->fd < 0) {
        fail(StringView(u8"cannot take the shared screenshot fd"));
    }
    pooledGuard(pool, [image] {
        close(image->fd);
    });
    return image;
}

RenderImage* VulkanRenderer::import(ObjPool& pool, SharedImage& source, bool hdr) {
    DmaImage& img = static_cast<DmaImage&>(source);
    checkImageSize(img.width, img.height, maxTextureSide());
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
    VkPhysicalDeviceExternalImageFormatInfo externalQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    VkPhysicalDeviceImageFormatInfo2 formatQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    VkExternalImageFormatProperties externalSupport{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 formatSupport{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};

    modifierQuery.drmFormatModifier = img.modifier;
    modifierQuery.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    externalQuery.pNext = &modifierQuery;
    externalQuery.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    formatQuery.pNext = &externalQuery;
    formatQuery.format = (VkFormat)img.format;
    formatQuery.type = VK_IMAGE_TYPE_2D;
    formatQuery.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    formatQuery.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    formatSupport.pNext = &externalSupport;

    VkDrmFormatModifierPropertiesListEXT modifiers{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 formatFeatures{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    Vector<VkDrmFormatModifierPropertiesEXT> modifierFeatures;
    VkFormatFeatureFlags features = 0;

    formatFeatures.pNext = &modifiers;
    vkGetPhysicalDeviceFormatProperties2(gpu->phys, (VkFormat)img.format, &formatFeatures);
    modifierFeatures.zero(modifiers.drmFormatModifierCount);
    modifiers.pDrmFormatModifierProperties = modifierFeatures.mutData();
    vkGetPhysicalDeviceFormatProperties2(gpu->phys, (VkFormat)img.format, &formatFeatures);

    for (u32 i = 0; i < modifiers.drmFormatModifierCount; i++) {
        if (modifierFeatures[i].drmFormatModifier == img.modifier) {
            features = modifierFeatures[i].drmFormatModifierTilingFeatures;
        }
    }

    VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;

    if ((features & needed) != needed || vkGetPhysicalDeviceImageFormatProperties2(gpu->phys, &formatQuery, &formatSupport) != VK_SUCCESS || !(externalSupport.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        fail(StringView(u8"vulkan cannot import the shared screenshot's format"));
    }

    TextureImage* image = pool.make<TextureImage>();
    image->gpu = gpu;
    image->width = img.width;
    image->height = img.height;
    image->layout = pixelLayout((VkFormat)img.format);
    image->hdr = hdr;
    Texture& tex = image->texture;

    VkSubresourceLayout plane = {};

    plane.offset = img.offset;
    plane.rowPitch = img.stride;

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {};

    modifier.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    modifier.drmFormatModifier = img.modifier;
    modifier.drmFormatModifierPlaneCount = 1;
    modifier.pPlaneLayouts = &plane;

    VkExternalMemoryImageCreateInfo external = {};

    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.pNext = &modifier;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ici = {};

    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &external;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = (VkFormat)img.format;
    ici.extent = {img.width, img.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    gpu->vkc(vkCreateImage(gpu->device, &ici, gpu->alloc, &tex.image));

    VkMemoryRequirements req = {};

    vkGetImageMemoryRequirements(gpu->device, tex.image, &req);

    auto getFdProps = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(gpu->device, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};

    if (!getFdProps || getFdProps(gpu->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, img.fd, &fdProps) != VK_SUCCESS) {
        fail(StringView(u8"cannot query shared screenshot memory"));
    }

    u32 memoryTypes = req.memoryTypeBits & fdProps.memoryTypeBits;

    if (!memoryTypes) {
        fail(StringView(u8"shared screenshot memory is incompatible"));
    }

    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};

    dedicated.image = tex.image;

    VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};

    import.pNext = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int ownedFd = fcntl(img.fd, F_DUPFD_CLOEXEC, 0);
    if (ownedFd < 0) {
        fail(StringView(u8"cannot duplicate shared image fd"));
    }
    ScopedGuard descriptor = [&] mutable -> void {
        if (ownedFd >= 0) {
            close(ownedFd);
        }
    };
    import.fd = ownedFd;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

    mai.pNext = &import;
    mai.allocationSize = img.allocationSize;
    mai.memoryTypeIndex = gpu->findMemoryType(memoryTypes, 0);
    VkResult allocated = vkAllocateMemory(gpu->device, &mai, gpu->alloc, &tex.memory);
    if (allocated == VK_SUCCESS) {
        ownedFd = -1;
    }
    gpu->vkc(allocated);
    gpu->vkc(vkBindImageMemory(gpu->device, tex.image, tex.memory, 0));

    gpu->oneShot([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        barrier.dstQueueFamilyIndex = gpu->queueFamily;
        barrier.image = tex.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    });

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};

    vci.image = tex.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = (VkFormat)img.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    gpu->vkc(vkCreateImageView(gpu->device, &vci, gpu->alloc, &tex.view));
    tex.flags = hdr ? DecodePq | SourceWide : DecodeSrgb;
    return image;
}

#endif
