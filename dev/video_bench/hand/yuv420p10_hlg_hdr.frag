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
    float y = bilinear(0u, at, ivec2(frame.size.xy) - 1) * 0.001141552511;
    float cb = bilinear(1u, chroma, chromaLast);
    float cr = bilinear(2u, chroma, chromaLast);
    vec3 signal = clamp(vec3(
        y + cr * 0.001645758929 - 0.9156879322,
        y - cb * 0.0001836530434 - cr * 0.0006376709005 + 0.3474584985,
        y + cb * 0.002099776786 - 1.148145075), 0.0, 1.0);
    vec3 scene = mix(exp(signal * 5.591816310 - 3.130917952) * 0.08333333333 + 0.02372241, signal * signal * 0.3333333333, lessThanEqual(signal, vec3(0.5)));
    float luminance = dot(scene, vec3(0.262700212, 0.6779980715, 0.05930171647));

    fColor = vec4(scene * (pow(luminance, 0.2) * (1000.0 / frame.white.x)), 1.0);
}
