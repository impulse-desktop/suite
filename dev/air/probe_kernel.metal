#include <metal_stdlib>
using namespace metal;

struct Frame {
    int2 size;
    int2 video;
    uint tilesX;
    uint first;
    float white;
};

kernel void probe(device const uint* headers [[buffer(0)]], device const uint* list [[buffer(1)]], device const uint* ops [[buffer(2)]], device const uint* tiles [[buffer(4)]], constant Frame& frame [[buffer(5)]], const device uint* words [[buffer(7)]], texture2d<float, access::write> target [[texture(0)]], uint3 group [[threadgroup_position_in_grid]], uint3 local [[thread_position_in_threadgroup]]) {
    threadgroup float sharedA[1728];
    threadgroup float sharedB[1728];
    threadgroup float sharedW[288];
    uint lane = local.y * 24u + local.x;
    uint tile = tiles[frame.first + group.x];
    uint start = headers[tile * 8u + 1u];
    uint op = list[start] & 2147483647u;
    int left = int(ops[op * 20u + 4u]);
    int x = int(tile % frame.tilesX) * 24 + int(local.x) - frame.video.x;
    int y = int(tile / frame.tilesX) * 24 + int(local.y) - frame.video.y;
    uint word = words[lane];
    float bits = as_type<float>(word);
    float half0 = float(as_type<half2>(word).x);
    float a = floor(bits) + exp2(bits) + log2(fabs(bits) + 1.0f) + exp(half0) + log(fabs(half0) + 1.0f) + sqrt(fabs(bits)) - (bits / 3.0f);
    float b = min(a, half0) * max(a, half0) + clamp(a, 0.0f, 1.0f) + (-a);
    int i = int(b);
    uint u = uint(b);
    int m = min(i, 5) + max(i, -5) + abs(i) - i * 3 + i / 7;
    uint s = ((u << 3u) | (u >> 2u)) & 255u;
    uint t = min(u, s) + max(u, 7u) - u * 5u + u / 3u;
    bool c = a < half0;
    bool d = i <= m;
    bool e = u == s;
    float f = select(a, b, c);
    int g = select(i, m, d);
    uint h = select(u, t, e);
    bool k = select(c, d, e);
    float inside = float(x >= left) * float(c);
    if (lane < 288u) {
        sharedW[lane] = f + float(g) + float(h) + float(k) + float(lane);
    }
    if (lane < 576u) {
        sharedA[lane * 3u] = float(word & 255u);
        sharedB[lane] = float(int(word >> 8u) - 128);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float carried0 = 0.0f;
    float carried1 = 1.0f;
    for (int portion = 0; portion < 7; portion++) {
        sharedB[(lane + uint(portion)) % 1728u] = carried0 * sharedW[lane % 288u];
        threadgroup_barrier(mem_flags::mem_threadgroup);
        carried0 = carried0 + sharedA[(lane * 3u + uint(portion)) % 1728u];
        carried1 = carried1 * sharedB[(lane + 1u) % 1728u];
    }
    bool drawn = x < frame.size.x && y < frame.size.y && x >= 0 && y >= 0;
    if (drawn) {
        target.write(float4(carried0 * frame.white, carried1, inside, 1.0f), uint2(uint(x), uint(y)));
    }
}
