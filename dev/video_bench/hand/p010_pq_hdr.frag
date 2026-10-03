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
    uvec2 word = x >> 1;
    uvec2 shift = (x & 1u) * 16u + 6u;
    vec2 upper = vec2((uvec2(words[top + word.x], words[top + word.y]) >> shift) & 0x3ffu);
    vec2 lower = vec2((uvec2(words[bottom + word.x], words[bottom + word.y]) >> shift) & 0x3ffu);
    vec2 column = mix(upper, lower, f.y);

    return mix(column.x, column.y, f.x);
}

vec2 pair(uint word) {
    return vec2(word >> 6 & 0x3ffu, word >> 22);
}

vec2 chroma(vec2 at, ivec2 last) {
    vec2 base = floor(at);
    vec2 f = at - base;
    uvec2 a = uvec2(max(ivec2(base), 0));
    uvec2 b = uvec2(min(ivec2(base) + 1, last));
    uint top = (frame.planeOffset[1] + a.y * frame.lineSize[1]) >> 2;
    uint bottom = (frame.planeOffset[1] + b.y * frame.lineSize[1]) >> 2;
    vec2 left = mix(pair(words[top + a.x]), pair(words[bottom + a.x]), f.y);
    vec2 right = mix(pair(words[top + b.x]), pair(words[bottom + b.x]), f.y);

    return mix(left, right, f.x);
}

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
    float y = luma(at, ivec2(frame.size.xy) - 1) * 0.001141552511;
    vec2 c = chroma(at * 0.5 - vec2(0.0, 0.25), ivec2(frame.size.zw) - 1);
    vec3 signal = clamp(vec3(
        y + c.y * 0.001645758929 - 0.9156879322,
        y - c.x * 0.0001836530434 - c.y * 0.0006376709005 + 0.3474584985,
        y + c.x * 0.002099776786 - 1.148145075), 0.0, 1.0);
    vec3 p = pow(signal, vec3(32.0 / 2523.0));

    fColor = vec4(pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), vec3(16384.0 / 2610.0)) * (10000.0 / frame.white.x), 1.0);
}
