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
    uvec3 top = (frame.planeOffset.xyz + a.y * frame.lineSize.xyz) >> 2;
    uvec3 bottom = (frame.planeOffset.xyz + b.y * frame.lineSize.xyz) >> 2;
    vec3 topLeft = vec3(uvec3(words[top.x + word.x], words[top.y + word.x], words[top.z + word.x]) >> shift.x & 0xffu);
    vec3 topRight = vec3(uvec3(words[top.x + word.y], words[top.y + word.y], words[top.z + word.y]) >> shift.y & 0xffu);
    vec3 bottomLeft = vec3(uvec3(words[bottom.x + word.x], words[bottom.y + word.x], words[bottom.z + word.x]) >> shift.x & 0xffu);
    vec3 bottomRight = vec3(uvec3(words[bottom.x + word.y], words[bottom.y + word.y], words[bottom.z + word.y]) >> shift.y & 0xffu);
    vec3 codes = mix(mix(topLeft, topRight, f.x), mix(bottomLeft, bottomRight, f.x), f.y);
    float y = codes.x * 0.004566210046;
    vec3 signal = clamp(vec3(
        y + codes.z * 0.007030357143 - 0.972945075,
        y - codes.y * 0.0008362690756 - codes.z * 0.002089840504 + 0.3014826655,
        y + codes.y * 0.008283928571 - 1.133402218), 0.0, 1.0);

    fColor = vec4(mix(1.055 * signal - 0.055, 12.92 * pow(signal, vec3(2.4)), lessThanEqual(signal, vec3(0.09047384596))), 1.0);
}
