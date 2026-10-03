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
    uint byte = x * 3u;
    uint place = (byte & 3u) * 8u;
    uint word = row + (byte >> 2);
    uint value = words[word] >> place | words[word + 1u] << (24u - place) << 8;

    return vec3(value & 0xffu, value >> 8 & 0xffu, value >> 16 & 0xffu);
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

    fColor = vec4(mix(left, right, f.x) * 0.003921568627, 1.0);
}
