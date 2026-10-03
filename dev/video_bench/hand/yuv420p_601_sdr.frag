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
    vec3 signal = vec3(
        y + cr * 0.006258928571 - 0.8742022179,
        y - cb * 0.001536322706 - cr * 0.003188108421 + 0.5316678235,
        y + cb * 0.007910714286 - 1.085630789);
    vec3 light = pow(max(signal, 0.0), vec3(2.4));
    vec3 display = clamp(mat3(
        0.9395420638, 0.01777222314, -0.001621599943,
        0.05018135686, 0.9657928625, -0.00436974966,
        0.01027657937, 0.01643491436, 1.00599135) * light, 0.0, 1.0);

    fColor = vec4(mix(1.055 * pow(display, vec3(1.0 / 2.4)) - 0.055, display * 12.92, lessThanEqual(display, vec3(0.0031308))), 1.0);
}
