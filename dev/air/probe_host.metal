#include <metal_stdlib>
using namespace metal;

struct Frame {
    int2 size;
    int2 video;
    uint tilesX;
    uint first;
    float white;
};

extern float4 layer(uint2 local, int2 origin, const device uint* words, threadgroup float* sharedA, threadgroup float* sharedB, threadgroup float* sharedW);

kernel void host(device const uint* tiles [[buffer(4)]], constant Frame& frame [[buffer(5)]], const device uint* words [[buffer(7)]], texture2d<float, access::write> target [[texture(0)]], uint3 group [[threadgroup_position_in_grid]], uint3 local [[thread_position_in_threadgroup]]) {
    threadgroup float sharedA[576];
    threadgroup float sharedB[576];
    threadgroup float sharedW[288];
    uint tile = tiles[frame.first + group.x];
    int2 origin = int2(int(tile % frame.tilesX), int(tile / frame.tilesX)) * 24;
    float4 shown = layer(local.xy, origin - frame.video, words, sharedA, sharedB, sharedW);
    int2 pixel = origin + int2(local.xy);
    if (pixel.x < frame.size.x && pixel.y < frame.size.y) {
        target.write(shown, uint2(pixel));
    }
}
