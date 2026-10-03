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

vec3 pixelAt(uint row, uint x) {
    uint byte = x * 6u;
    uint place = (byte & 2u) * 8u;
    uint word = row + (byte >> 2);
    uint second = words[word + 1u];
    uint first = words[word] >> place | second << 16 << (16u - place);

    return vec3(first >> 4 & 0xfffu, first >> 20, (second >> place) >> 4 & 0xfffu);
}

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
    vec2 base = floor(at);
    vec2 f = at - base;
    uvec2 a = uvec2(max(ivec2(base), 0));
    uvec2 b = uvec2(min(ivec2(base) + 1, ivec2(frame.size.xy) - 1));
    uint top = (frame.planeOffset[0] + a.y * frame.lineSize[0]) >> 2;
    uint bottom = (frame.planeOffset[0] + b.y * frame.lineSize[0]) >> 2;
    vec3 left = mix(pixelAt(top, a.x), pixelAt(bottom, a.x), f.y);
    vec3 right = mix(pixelAt(top, b.x), pixelAt(bottom, b.x), f.y);
    vec3 xyz = pow(mix(left, right, f.x) * 0.0002442002442, vec3(2.6));
    vec3 display = clamp(mat3(
        3.536033247, -1.057485192, 0.06069473487,
        -1.677349104, 2.046758709, -0.2225473612,
        -0.5440051149, 0.04533829909, 1.153199963) * xyz, 0.0, 1.0);

    fColor = vec4(mix(1.055 * pow(display, vec3(1.0 / 2.4)) - 0.055, display * 12.92, lessThanEqual(display, vec3(0.0031308))), 1.0);
}
