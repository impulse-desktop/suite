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

float mosaic(uint row, uint x) {
    return float(words[row + (x >> 2)] >> ((x & 3u) * 8u) & 0xffu);
}

uint mirror(int at, int last) {
    return uint(last - abs(last - abs(at)));
}

vec3 demosaic(vec3 top, vec3 middle, vec3 bottom, ivec2 phase) {
    float horizontal = (middle.x + middle.z) * 0.5;
    float vertical = (top.y + bottom.y) * 0.5;
    float cross = (horizontal + vertical) * 0.5;
    float diagonal = (top.x + top.z + bottom.x + bottom.z) * 0.25;
    float onRed = float(phase.x + phase.y == 0);
    float onBlue = float(phase.x + phase.y == 2);
    vec3 onGreen = phase.y == 0 ? vec3(horizontal, middle.y, vertical) : vec3(vertical, middle.y, horizontal);

    return onRed * vec3(middle.y, cross, diagonal) + onBlue * vec3(diagonal, cross, middle.y) + (1.0 - onRed - onBlue) * onGreen;
}

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
    vec2 base = floor(at);
    vec2 f = at - base;
    ivec2 last = ivec2(frame.size.xy) - 1;
    ivec2 a = max(ivec2(base), 0);
    ivec2 b = min(ivec2(base) + 1, last);
    uvec4 xs = uvec4(mirror(a.x - 1, last.x), a.x, mirror(a.x + 1, last.x), mirror(a.x + 2, last.x));
    uvec4 ys = uvec4(mirror(a.y - 1, last.y), a.y, mirror(a.y + 1, last.y), mirror(a.y + 2, last.y));
    uvec4 rows = (frame.planeOffset[0] + ys * frame.lineSize[0]) >> 2;
    vec4 r0 = vec4(mosaic(rows.x, xs.x), mosaic(rows.x, xs.y), mosaic(rows.x, xs.z), mosaic(rows.x, xs.w));
    vec4 r1 = vec4(mosaic(rows.y, xs.x), mosaic(rows.y, xs.y), mosaic(rows.y, xs.z), mosaic(rows.y, xs.w));
    vec4 r2 = vec4(mosaic(rows.z, xs.x), mosaic(rows.z, xs.y), mosaic(rows.z, xs.z), mosaic(rows.z, xs.w));
    vec4 r3 = vec4(mosaic(rows.w, xs.x), mosaic(rows.w, xs.y), mosaic(rows.w, xs.z), mosaic(rows.w, xs.w));
    float right = float(b.x != a.x);
    float down = float(b.y != a.y);
    vec3 t0 = r0.xyz;
    vec3 t1 = r1.xyz;
    vec3 t2 = r2.xyz;
    vec3 u0 = mix(r0.xyz, r0.yzw, right);
    vec3 u1 = mix(r1.xyz, r1.yzw, right);
    vec3 u2 = mix(r2.xyz, r2.yzw, right);
    vec3 d3 = mix(t2, r3.xyz, down);
    vec3 e3 = mix(u2, mix(r3.xyz, r3.yzw, right), down);
    ivec2 pa = a & 1;
    ivec2 pb = b & 1;
    vec3 topLeft = demosaic(t0, t1, t2, pa);
    vec3 topRight = demosaic(u0, u1, u2, ivec2(pb.x, pa.y));
    vec3 bottomLeft = demosaic(mix(t0, t1, down), mix(t1, t2, down), d3, ivec2(pa.x, pb.y));
    vec3 bottomRight = demosaic(mix(u0, u1, down), mix(u1, u2, down), e3, pb);
    vec3 signal = clamp(mix(mix(topLeft, topRight, f.x), mix(bottomLeft, bottomRight, f.x), f.y) * 0.003921568627, 0.0, 1.0);

    fColor = vec4(mix(1.055 * signal - 0.055, 12.92 * pow(signal, vec3(2.4)), lessThanEqual(signal, vec3(0.09047384596))), 1.0);
}
