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

const vec3 evens[3] = vec3[3](vec3(0.0244904064, -0.0226433958, -0.00187803571), vec3(-0.135862093, 0.142553048, -0.00669409677), vec3(0.611371687, -0.119909652, 0.00857213248));
const vec3 odds[3] = vec3[3](vec3(-0.0319397452, 0.0383419974, -0.00642154827), vec3(0.0905269779, -0.1042344, 0.0137522839), vec3(-0.668230721, 0.191258225, -0.0231172403));

struct Axis {
    int start;
    float w[6];
};

Axis axis(float at) {
    Axis a;
    float base = floor(at);
    float t = 2.0 * (at - base) - 1.0;
    float s = t * t;

    for (int k = 0; k < 3; k++) {
        float e = (evens[k].z * s + evens[k].y) * s + evens[k].x;
        float o = ((odds[k].z * s + odds[k].y) * s + odds[k].x) * t;

        a.w[k] = e + o;
        a.w[5 - k] = e - o;
    }

    a.start = int(base) - 2;

    return a;
}

void fold(Axis a, int last, out int window, out float v[6]) {
    window = clamp(a.start, 0, last - 5);

    int d = a.start - window;
    float w0 = a.w[0];
    float w1 = a.w[1];
    float w2 = a.w[2];
    float w3 = a.w[3];
    float w4 = a.w[4];
    float w5 = a.w[5];
    float p1 = w0 + w1;
    float p2 = p1 + w2;
    float p3 = p2 + w3;
    float s4 = w4 + w5;
    float s3 = w3 + s4;
    float s2 = w2 + s3;
    bool l1 = d == -1;
    bool l2 = d == -2;
    bool l3 = d == -3;
    bool r1 = d == 1;
    bool r2 = d == 2;
    bool r3 = d == 3;

    v[0] = mix(mix(mix(mix(w0, 0.0, d > 0), p1, l1), p2, l2), p3, l3);
    v[1] = mix(mix(mix(mix(mix(w1, w2, l1), w3, l2), w4, l3), w0, r1), 0.0, d > 1);
    v[2] = mix(mix(mix(mix(mix(mix(w2, w3, l1), w4, l2), w5, l3), w1, r1), w0, r2), 0.0, r3);
    v[3] = mix(mix(mix(mix(mix(mix(w3, w4, l1), w5, l2), 0.0, l3), w2, r1), w1, r2), w0, r3);
    v[4] = mix(mix(mix(mix(mix(w4, w5, l1), 0.0, d < -1), w3, r1), w2, r2), w1, r3);
    v[5] = mix(mix(mix(mix(w5, 0.0, d < 0), s4, r1), s3, r2), s2, r3);
}

float rowSum(uint address, uint s, float v[6]) {
    uint w0 = words[address];
    uint w1 = words[address + 1u];
    uint w2 = words[address + 2u];
    uint lo = mix((w1 << (32u - s)) | (w0 >> s), w0, s == 0u);
    uint hi = mix((w2 << (32u - s)) | (w1 >> s), w1, s == 0u);

    return v[0] * float(lo & 0xffu) + v[1] * float((lo >> 8u) & 0xffu) + v[2] * float((lo >> 16u) & 0xffu) + v[3] * float(lo >> 24u) + v[4] * float(hi & 0xffu) + v[5] * float((hi >> 8u) & 0xffu);
}

float rows4(uint p, int window, float v[6], Axis y, int last) {
    uint column = uint(window) >> 2u;
    uint s = (uint(window) & 3u) * 8u;
    float total = 0.0;

    for (int j = 1; j < 5; j++) {
        uint row = uint(clamp(y.start + j, 0, last));

        total += y.w[j] * rowSum(((frame.planeOffset[p] + row * frame.lineSize[p]) >> 2u) + column, s, v);
    }

    return total / (y.w[1] + y.w[2] + y.w[3] + y.w[4]);
}

float rows5(uint p, int window, float v[6], Axis y, int last) {
    uint column = uint(window) >> 2u;
    uint s = (uint(window) & 3u) * 8u;
    bool odd = ((uint(gl_FragCoord.x) ^ uint(gl_FragCoord.y)) & 1u) != 0u;
    int outer = odd ? 5 : 0;
    float total = 2.0 * mix(y.w[0], y.w[5], odd) * rowSum(((frame.planeOffset[p] + uint(clamp(y.start + outer, 0, last)) * frame.lineSize[p]) >> 2u) + column, s, v);

    for (int j = 1; j < 5; j++) {
        uint row = uint(clamp(y.start + j, 0, last));

        total += y.w[j] * rowSum(((frame.planeOffset[p] + row * frame.lineSize[p]) >> 2u) + column, s, v);
    }

    return total;
}

float plane(uint p, int window, float v[6], Axis y, int last) {
    uint column = uint(window) >> 2u;
    uint s = (uint(window) & 3u) * 8u;
    float total = 0.0;

    for (int j = 0; j < 6; j++) {
        uint row = uint(clamp(y.start + j, 0, last));

        total += y.w[j] * rowSum(((frame.planeOffset[p] + row * frame.lineSize[p]) >> 2u) + column, s, v);
    }

    return total;
}

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
    vec2 chroma = at * 0.5 - vec2(0.0, 0.25);
    ivec2 last = ivec2(frame.size.xy) - 1;
    ivec2 chromaLast = ivec2(frame.size.zw) - 1;
    Axis lx = axis(at.x);
    Axis ly = axis(at.y);
    Axis cx = axis(chroma.x);
    Axis cy = axis(chroma.y);
    int lw;
    int cw;
    float lv[6];
    float cv[6];

    fold(lx, last.x, lw, lv);
    fold(cx, chromaLast.x, cw, cv);

    float y = plane(0u, lw, lv, ly, last.y) * 0.004566210046;
    float cb = rows4(1u, cw, cv, cy, chromaLast.y);
    float cr = rows4(2u, cw, cv, cy, chromaLast.y);
    vec3 signal = clamp(vec3(
        y + cr * 0.007030357143 - 0.972945075,
        y - cb * 0.0008362690756 - cr * 0.002089840504 + 0.3014826655,
        y + cb * 0.008283928571 - 1.133402218), 0.0, 1.0);

    fColor = vec4(mix(1.055 * signal - 0.055, 12.92 * pow(signal, vec3(2.4)), lessThanEqual(signal, vec3(0.09047384596))), 1.0);
}
