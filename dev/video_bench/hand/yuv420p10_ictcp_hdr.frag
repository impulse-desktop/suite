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
    uvec2 word = x >> 1;
    uvec2 shift = (x & 1u) * 16u;
    vec2 upper = vec2((uvec2(words[top + word.x], words[top + word.y]) >> shift) & 0x3ffu);
    vec2 lower = vec2((uvec2(words[bottom + word.x], words[bottom + word.y]) >> shift) & 0x3ffu);
    vec2 column = mix(upper, lower, f.y);

    return mix(column.x, column.y, f.x);
}

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
    vec2 chroma = at * 0.5 - vec2(0.0, 0.25);
    ivec2 chromaLast = ivec2(frame.size.zw) - 1;
    vec3 codes = vec3(bilinear(0u, at, ivec2(frame.size.xy) - 1), bilinear(1u, chroma, chromaLast), bilinear(2u, chroma, chromaLast));
    vec3 lms = clamp(mat3(
        0.001141552511, 0.001141552511, 0.001141552511,
        9.608300266e-06, -9.608300266e-06, 0.0006250349729,
        0.0001239169922, -0.0001239169922, -0.0003578428292) * codes + vec3(-0.1414243105, -0.004694410993, -0.2098617383), 0.0, 1.0);
    vec3 p = pow(lms, vec3(32.0 / 2523.0));
    vec3 light = pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), vec3(16384.0 / 2610.0));

    fColor = vec4(mat3(
        3.436606694, -0.7913295556, -0.02594989969,
        -2.506452119, 1.983600452, -0.09891371471,
        0.06984542432, -0.1922708962, 1.124863614) * light * (10000.0 / frame.white.x), 1.0);
}
