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

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
    vec2 base = floor(at);
    vec2 f = at - base;
    uvec2 a = uvec2(max(ivec2(base), 0));
    uvec2 b = uvec2(min(ivec2(base) + 1, ivec2(frame.size.xy) - 1));
    uint top = (frame.planeOffset[0] + a.y * frame.lineSize[0]) >> 2;
    uint bottom = (frame.planeOffset[0] + b.y * frame.lineSize[0]) >> 2;
    uvec2 x = uvec2(a.x, b.x);
    uvec2 word = x >> 1;
    uvec2 shift = (x & 1u) * 16u;
    uvec2 upper = uvec2(words[top + word.x], words[top + word.y]);
    uvec2 lower = uvec2(words[bottom + word.x], words[bottom + word.y]);
    upper = ((upper & 0x00ff00ffu) << 8 | upper >> 8 & 0x00ff00ffu) >> shift;
    lower = ((lower & 0x00ff00ffu) << 8 | lower >> 8 & 0x00ff00ffu) >> shift;
    vec2 column = mix(vec2(upper & 0xffffu), vec2(lower & 0xffffu), f.y);

    fColor = vec4(vec3(mix(column.x, column.y, f.x) * 0.00001525902190), 1.0);
}
