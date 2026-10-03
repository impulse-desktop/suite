#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fColor;

layout(std430, set = 0, binding = 0) readonly buffer Bytes {
    uint words[];
};

layout(std140, set = 0, binding = 1) uniform Frame {
    uvec4 planeOffset;
    uvec4 lineSize;
    uvec4 size;
    vec4 white;
} frame;

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
    vec2 base = floor(at);
    vec2 f = at - base;
    uvec2 a = uvec2(max(ivec2(base), 0));
    uvec2 b = uvec2(min(ivec2(base) + 1, ivec2(frame.size.xy) - 1));
    uvec2 x = uvec2(a.x, b.x);
    uvec2 word = x >> 2;
    uvec2 shift = (x & 3u) * 8u;
    uvec3 top = (frame.planeOffset.zxy + a.y * frame.lineSize.zxy) >> 2;
    uvec3 bottom = (frame.planeOffset.zxy + b.y * frame.lineSize.zxy) >> 2;
    vec3 topLeft = vec3(uvec3(words[top.x + word.x], words[top.y + word.x], words[top.z + word.x]) >> shift.x & 0xffu);
    vec3 topRight = vec3(uvec3(words[top.x + word.y], words[top.y + word.y], words[top.z + word.y]) >> shift.y & 0xffu);
    vec3 bottomLeft = vec3(uvec3(words[bottom.x + word.x], words[bottom.y + word.x], words[bottom.z + word.x]) >> shift.x & 0xffu);
    vec3 bottomRight = vec3(uvec3(words[bottom.x + word.y], words[bottom.y + word.y], words[bottom.z + word.y]) >> shift.y & 0xffu);

    fColor = vec4(mix(mix(topLeft, topRight, f.x), mix(bottomLeft, bottomRight, f.x), f.y) * 0.003921568627, 1.0);
}
