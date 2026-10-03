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

float lanczos(float x) {
    float a = 3.14159265358979 * max(abs(x), 1e-4);

    return 3.0 * sin(a) * sin(a / 3.0) / (a * a);
}

float sample8(uint plane, int x, int y, ivec2 last) {
    uint u = uint(clamp(x, 0, last.x));
    uint row = (frame.planeOffset[plane] + uint(clamp(y, 0, last.y)) * frame.lineSize[plane]) >> 2;

    return float((words[row + (u >> 2)] >> ((u & 3u) * 8u)) & 0xffu);
}

float lanczos3(uint plane, vec2 at, ivec2 last) {
    vec2 base = floor(at);
    vec2 f = at - base;
    ivec2 b = ivec2(base);
    float wx[6];
    float wy[6];
    float sx = 0.0;
    float sy = 0.0;

    for (int i = 0; i < 6; i++) {
        wx[i] = lanczos(float(i - 2) - f.x);
        wy[i] = lanczos(float(i - 2) - f.y);
        sx += wx[i];
        sy += wy[i];
    }

    float total = 0.0;

    for (int j = 0; j < 6; j++) {
        float row = 0.0;

        for (int i = 0; i < 6; i++) {
            row += wx[i] * sample8(plane, b.x + i - 2, b.y + j - 2, last);
        }

        total += wy[j] * row;
    }

    return total / (sx * sy);
}

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
    vec2 chroma = at * 0.5 - vec2(0.0, 0.25);
    ivec2 chromaLast = ivec2(frame.size.zw) - 1;
    float y = lanczos3(0u, at, ivec2(frame.size.xy) - 1) * 0.004566210046;
    float cb = lanczos3(1u, chroma, chromaLast);
    float cr = lanczos3(2u, chroma, chromaLast);
    vec3 signal = clamp(vec3(
        y + cr * 0.007030357143 - 0.972945075,
        y - cb * 0.0008362690756 - cr * 0.002089840504 + 0.3014826655,
        y + cb * 0.008283928571 - 1.133402218), 0.0, 1.0);

    fColor = vec4(mix(1.055 * signal - 0.055, 12.92 * pow(signal, vec3(2.4)), lessThanEqual(signal, vec3(0.09047384596))), 1.0);
}
