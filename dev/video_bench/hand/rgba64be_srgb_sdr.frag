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

uvec2 swapped(uvec2 value) {
    return (value & 0x00ff00ffu) << 8 | value >> 8 & 0x00ff00ffu;
}

vec4 pixelAt(uint row, uint x) {
    uvec2 value = swapped(uvec2(words[row + x * 2u], words[row + x * 2u + 1u]));

    return vec4(value.x & 0xffffu, value.x >> 16, value.y & 0xffffu, value.y >> 16);
}

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
    vec2 base = floor(at);
    vec2 f = at - base;
    uvec2 a = uvec2(max(ivec2(base), 0));
    uvec2 b = uvec2(min(ivec2(base) + 1, ivec2(frame.size.xy) - 1));
    uint top = (frame.planeOffset[0] + a.y * frame.lineSize[0]) >> 2;
    uint bottom = (frame.planeOffset[0] + b.y * frame.lineSize[0]) >> 2;
    vec4 left = mix(pixelAt(top, a.x), pixelAt(bottom, a.x), f.y);
    vec4 right = mix(pixelAt(top, b.x), pixelAt(bottom, b.x), f.y);

    fColor = mix(left, right, f.x) * 0.00001525902190;
}
