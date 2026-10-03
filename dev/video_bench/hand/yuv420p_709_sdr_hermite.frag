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

float kernel(float d, float ratio) {
    float x = min(abs(d * ratio), 1.0);

    return ratio < 0.9999 ? (2.0 * x - 3.0) * x * x + 1.0 : max(1.0 - abs(d), 0.0);
}

float sample8(uint plane, int x, int y, ivec2 last) {
    uint u = uint(clamp(x, 0, last.x));
    uint row = (frame.planeOffset[plane] + uint(clamp(y, 0, last.y)) * frame.lineSize[plane]) >> 2;

    return float((words[row + (u >> 2)] >> ((u & 3u) * 8u)) & 0xffu);
}

float filtered(uint plane, vec2 at, ivec2 last, vec2 ratio) {
    vec2 base = floor(at);
    vec2 f = at - base;
    ivec2 b = ivec2(base);
    int hx = ratio.x < 0.9999 ? int(ceil(1.0 / ratio.x - 1e-4)) : 1;
    int hy = ratio.y < 0.9999 ? int(ceil(1.0 / ratio.y - 1e-4)) : 1;
    float total = 0.0;
    float norm = 0.0;

    for (int j = 1 - hy; j <= hy; j++) {
        float wy = kernel(float(j) - f.y, ratio.y);

        for (int i = 1 - hx; i <= hx; i++) {
            float w = kernel(float(i) - f.x, ratio.x) * wy;

            total += w * sample8(plane, b.x + i, b.y + j, last);
            norm += w;
        }
    }

    return total / norm;
}

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
    vec2 chroma = at * 0.5 - vec2(0.0, 0.25);
    ivec2 chromaLast = ivec2(frame.size.zw) - 1;
    vec2 target = frame.white.yz;
    float y = filtered(0u, at, ivec2(frame.size.xy) - 1, target / vec2(frame.size.xy)) * 0.004566210046;
    float cb = filtered(1u, chroma, chromaLast, target / vec2(frame.size.zw));
    float cr = filtered(2u, chroma, chromaLast, target / vec2(frame.size.zw));
    vec3 signal = clamp(vec3(
        y + cr * 0.007030357143 - 0.972945075,
        y - cb * 0.0008362690756 - cr * 0.002089840504 + 0.3014826655,
        y + cb * 0.008283928571 - 1.133402218), 0.0, 1.0);

    fColor = vec4(mix(1.055 * signal - 0.055, 12.92 * pow(signal, vec3(2.4)), lessThanEqual(signal, vec3(0.09047384596))), 1.0);
}
