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

float bilinear(uint plane, vec2 at, ivec2 last) {
    vec2 base = floor(at);
    vec2 f = at - base;
    uvec2 a = uvec2(max(ivec2(base), 0));
    uvec2 b = uvec2(min(ivec2(base) + 1, last));
    uint top = (frame.planeOffset[plane] + a.y * frame.lineSize[plane]) >> 2;
    uint bottom = (frame.planeOffset[plane] + b.y * frame.lineSize[plane]) >> 2;
    uvec2 x = uvec2(a.x, b.x);
    uvec2 word = x >> 2;
    uvec2 shift = (x & 3u) * 8u;
    vec2 upper = vec2((uvec2(words[top + word.x], words[top + word.y]) >> shift) & 0xffu);
    vec2 lower = vec2((uvec2(words[bottom + word.x], words[bottom + word.y]) >> shift) & 0xffu);
    vec2 column = mix(upper, lower, f.y);

    return mix(column.x, column.y, f.x);
}

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
    vec2 chroma = at * 0.5 - vec2(0.0, 0.25);
    ivec2 chromaLast = ivec2(frame.size.zw) - 1;
    float y = bilinear(0u, at, ivec2(frame.size.xy) - 1) * 0.004566210046;
    float cb = bilinear(1u, chroma, chromaLast);
    float cr = bilinear(2u, chroma, chromaLast);
    vec3 signal = clamp(vec3(
        y + cr * 0.007030357143 - 0.972945075,
        y - cb * 0.0008362690756 - cr * 0.002089840504 + 0.3014826655,
        y + cb * 0.008283928571 - 1.133402218), 0.0, 1.0);

    fColor = vec4(mix(1.055 * signal - 0.055, 12.92 * pow(signal, vec3(2.4)), lessThanEqual(signal, vec3(0.09047384596))), 1.0);
}
