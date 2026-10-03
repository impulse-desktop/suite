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
    float y = bilinear(0u, at, ivec2(frame.size.xy) - 1) * 0.001141552511 - 0.07305936073;
    vec2 c = vec2(bilinear(1u, chroma, chromaLast), bilinear(2u, chroma, chromaLast)) * 0.001116071429 - 0.5714285714;
    vec2 difference = c * mix(vec2(1.581970849, 0.9938295953), vec2(1.940343306, 1.718241985), lessThanEqual(c, vec2(0.0)));
    vec3 yrb = max(vec3(y, y + difference.y, y + difference.x), 0.0);
    vec3 light = mix(pow(yrb * 0.9096724157 + 0.09032758431, vec3(2.222222222)), yrb * 0.2222222222, lessThanEqual(yrb, vec3(0.0812428583)));
    float g = max((light.x - 0.2627 * light.y - 0.0593 * light.z) * 1.474926254, 0.0);
    float green = g <= 0.01805396851 ? 4.5 * g : 1.099296827 * pow(g, 0.45) - 0.0992968268;

    fColor = vec4(pow(max(vec3(yrb.y, green, yrb.z), 0.0), vec3(2.4)), 1.0);
}
