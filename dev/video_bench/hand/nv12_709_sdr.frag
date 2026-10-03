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

float luma(vec2 at, ivec2 last) {
    vec2 base = floor(at);
    vec2 f = at - base;
    uvec2 a = uvec2(max(ivec2(base), 0));
    uvec2 b = uvec2(min(ivec2(base) + 1, last));
    uint top = (frame.planeOffset[0] + a.y * frame.lineSize[0]) >> 2;
    uint bottom = (frame.planeOffset[0] + b.y * frame.lineSize[0]) >> 2;
    uvec2 x = uvec2(a.x, b.x);
    uvec2 word = x >> 2;
    uvec2 shift = (x & 3u) * 8u;
    vec2 upper = vec2((uvec2(words[top + word.x], words[top + word.y]) >> shift) & 0xffu);
    vec2 lower = vec2((uvec2(words[bottom + word.x], words[bottom + word.y]) >> shift) & 0xffu);
    vec2 column = mix(upper, lower, f.y);

    return mix(column.x, column.y, f.x);
}

vec2 pair(uint word, uint shift) {
    uint value = word >> shift;

    return vec2(value & 0xffu, value >> 8 & 0xffu);
}

vec2 chroma(vec2 at, ivec2 last) {
    vec2 base = floor(at);
    vec2 f = at - base;
    uvec2 a = uvec2(max(ivec2(base), 0));
    uvec2 b = uvec2(min(ivec2(base) + 1, last));
    uint top = (frame.planeOffset[1] + a.y * frame.lineSize[1]) >> 2;
    uint bottom = (frame.planeOffset[1] + b.y * frame.lineSize[1]) >> 2;
    uvec2 x = uvec2(a.x, b.x);
    uvec2 word = x >> 1;
    uvec2 shift = (x & 1u) * 16u;
    vec2 left = mix(pair(words[top + word.x], shift.x), pair(words[bottom + word.x], shift.x), f.y);
    vec2 right = mix(pair(words[top + word.y], shift.y), pair(words[bottom + word.y], shift.y), f.y);

    return mix(left, right, f.x);
}

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
    float y = luma(at, ivec2(frame.size.xy) - 1) * 0.004566210046;
    vec2 c = chroma(at * 0.5 - vec2(0.0, 0.25), ivec2(frame.size.zw) - 1);
    vec3 signal = clamp(vec3(
        y + c.y * 0.007030357143 - 0.972945075,
        y - c.x * 0.0008362690756 - c.y * 0.002089840504 + 0.3014826655,
        y + c.x * 0.008283928571 - 1.133402218), 0.0, 1.0);

    fColor = vec4(mix(1.055 * signal - 0.055, 12.92 * pow(signal, vec3(2.4)), lessThanEqual(signal, vec3(0.09047384596))), 1.0);
}
