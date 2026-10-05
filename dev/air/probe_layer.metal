#include <metal_stdlib>
using namespace metal;

struct Frame {
    int2 size;
    int2 video;
    uint tilesX;
    uint first;
    float white;
};

[[visible]] float4 layer(uint2 local, int2 origin, const device uint* words, threadgroup float* sharedA, threadgroup float* sharedB, threadgroup float* sharedW) {
    uint lane = local.y * 24u + local.x;
    sharedA[lane] = float(words[lane]) + float(origin.x);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    sharedB[lane] = sharedA[(lane + 1u) % 576u] + sharedW[lane % 288u];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    return float4(sharedB[lane], float(origin.y), 0.0f, 1.0f);
}
