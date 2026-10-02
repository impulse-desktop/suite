{%- set fetches = ["integer", "float", "bitstream", "palette"] %}
{%- set transfers = {
    "power": [1, 2, 4, 5, 6, 7, 11, 12, 14, 15, 17],
    "srgb": [13],
    "linear": [8],
    "log": [9, 10],
    "pq": [16],
    "hlg": [18],
} %}
{%- set outputs = ["hdr", "sdr"] %}
{%- set matrices = [0, 1, 2, 4, 5, 6, 7, 8, 9, 12] %}
{%- set primaries = {
    1: [0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290],
    2: [0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290],
    4: [0.670, 0.330, 0.210, 0.710, 0.140, 0.080, 0.3100, 0.3160],
    5: [0.640, 0.330, 0.290, 0.600, 0.150, 0.060, 0.3127, 0.3290],
    6: [0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290],
    7: [0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290],
    8: [0.681, 0.319, 0.243, 0.692, 0.145, 0.049, 0.3100, 0.3160],
    9: [0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290],
    10: "xyz",
    11: [0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3140, 0.3510],
    12: [0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290],
    22: [0.630, 0.340, 0.295, 0.605, 0.155, 0.077, 0.3127, 0.3290],
} %}
{%- set ranges = [0, 1, 2] %}
{%- set locations = [0, 1, 2, 3, 4, 5, 6] %}

#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fColor;

layout(std430, set = 0, binding = 0) readonly buffer Bytes {
    uint words[];
};

layout(std140, set = 0, binding = 1) uniform Frame {
    uvec4 plane;
    uvec4 step;
    uvec4 offset;
    uvec4 shift;
    uvec4 depth;
    uvec4 planeOffset;
    uvec4 lineSize;
    uvec4 size;
    uvec4 chroma;
    uvec4 color;
    vec4 white;
} frame;

const uint BIG_ENDIAN = 1u;
const uint RGB = 2u;
const uint ALPHA = 4u;

uint frameWidth() {
    return frame.size.x;
}

uint frameHeight() {
    return frame.size.y;
}

uint componentCount() {
    return frame.size.z;
}

bool hasFlag(uint bit) {
    return (frame.size.w & bit) != 0u;
}

uint colorCount() {
    return hasFlag(ALPHA) ? componentCount() - 1u : componentCount();
}

uint matrixCode() {
    return frame.color.x;
}

uint rangeCode() {
    return frame.color.y;
}

uint transferCode() {
    return frame.color.z;
}

uint primariesCode() {
    return frame.color.w;
}

uint byteAt(uint address) {
    return (words[address >> 2] >> ((address & 3u) * 8u)) & 0xffu;
}

uint bytesAt(uint address, uint count, bool bigEndian) {
    uint value = 0u;

    for (uint i = 0u; i < count; i++) {
        value |= byteAt(address + i) << ((bigEndian ? count - 1u - i : i) * 8u);
    }

    return value;
}

uint depthMask(uint bits) {
    return bits >= 32u ? 0xffffffffu : (1u << bits) - 1u;
}

mat3 primariesToXyz() {
    switch (primariesCode()) {
{%- for code, chromaticities in primaries.items() %}
        case {{ code }}u:
            return {{ rgb_to_xyz(chromaticities) }};
{%- endfor %}
        default:
            return {{ rgb_to_xyz(primaries[1]) }};
    }
}

uint componentAddress(uint c, uvec2 at) {
    uint p = frame.plane[c];

    return frame.planeOffset[p] + at.y * frame.lineSize[p] + at.x * frame.step[c] + frame.offset[c];
}

vec2 bilinear(vec2 at, uvec2 extent, out uvec2 a, out uvec2 b) {
    vec2 base = floor(at);
    ivec2 last = ivec2(extent) - 1;

    a = uvec2(clamp(ivec2(base), ivec2(0), last));
    b = uvec2(clamp(ivec2(base) + 1, ivec2(0), last));

    return at - base;
}

{%- if fetch == "palette" %}

vec4 paletteColor(uvec2 at) {
    uint index = byteAt(frame.planeOffset[0] + at.y * frame.lineSize[0] + at.x);
    uint entry = bytesAt(frame.planeOffset[1] + index * 4u, 4u, false);

    return vec4((entry >> 16) & 255u, (entry >> 8) & 255u, entry & 255u, entry >> 24) / 255.0;
}

vec4 sourceColor(vec2 at) {
    uvec2 a;
    uvec2 b;
    vec2 f = bilinear(at, uvec2(frameWidth(), frameHeight()), a, b);
    vec4 top = mix(paletteColor(a), paletteColor(uvec2(b.x, a.y)), f.x);
    vec4 bottom = mix(paletteColor(uvec2(a.x, b.y)), paletteColor(b), f.x);

    return mix(top, bottom, f.y);
}

{%- else %}

{%- if fetch == "integer" %}

float componentCode(uint c, uvec2 at) {
    uint bits = frame.shift[c] + frame.depth[c];
    bool bigEndian = hasFlag(BIG_ENDIAN);
    uint value = bits <= 8u ? byteAt(componentAddress(c, at) + (bigEndian ? 1u : 0u)) : bytesAt(componentAddress(c, at), bits <= 16u ? 2u : 4u, bigEndian);

    return float((value >> frame.shift[c]) & depthMask(frame.depth[c]));
}

{%- elif fetch == "float" %}

float componentCode(uint c, uvec2 at) {
    return uintBitsToFloat(bytesAt(componentAddress(c, at), 4u, hasFlag(BIG_ENDIAN)));
}

{%- elif fetch == "bitstream" %}

float componentCode(uint c, uvec2 at) {
    uint p = frame.plane[c];
    uint bit = at.x * frame.step[c] + frame.offset[c];
    uint value = byteAt(frame.planeOffset[p] + at.y * frame.lineSize[p] + (bit >> 3));

    return float((value >> (8u - frame.depth[c] - (bit & 7u))) & depthMask(frame.depth[c]));
}

{%- endif %}

float interpolated(uint c, vec2 at, uvec2 extent) {
    uvec2 a;
    uvec2 b;
    vec2 f = bilinear(at, extent, a, b);
    float top = mix(componentCode(c, a), componentCode(c, uvec2(b.x, a.y)), f.x);
    float bottom = mix(componentCode(c, uvec2(a.x, b.y)), componentCode(c, b), f.x);

    return mix(top, bottom, f.y);
}

float fullComponent(uint c, vec2 at) {
    return interpolated(c, at, uvec2(frameWidth(), frameHeight()));
}

vec2 chromaSiting() {
    uint location = frame.chroma.z;
    vec2 within = location == 2u ? vec2(0.5, 0.5)
                : location == 3u ? vec2(0.0, 0.0)
                : location == 4u ? vec2(0.5, 0.0)
                : location == 5u ? vec2(0.0, 1.0)
                : location == 6u ? vec2(0.5, 1.0)
                : vec2(0.0, 0.5);

    return within * vec2((1u << frame.chroma.x) - 1u, (1u << frame.chroma.y) - 1u);
}

float chromaComponent(uint c, vec2 at) {
    uvec2 subsampling = frame.chroma.xy;
    uvec2 extent = (uvec2(frameWidth(), frameHeight()) + (uvec2(1u) << subsampling) - 1u) >> subsampling;

    return interpolated(c, (at - chromaSiting()) / vec2(uvec2(1u) << subsampling), extent);
}

bool fullRange() {
{%- if fetch == "bitstream" %}
    return true;
{%- else %}
    return rangeCode() == 2u || (rangeCode() == 0u && (hasFlag(RGB) || colorCount() == 1u));
{%- endif %}
}

float decodeLuma(float code, uint depth) {
{%- if fetch == "float" %}
    return fullRange() ? code : (code * 255.0 - 16.0) / 219.0;
{%- else %}
    float unit = exp2(float(depth) - 8.0);

    return fullRange() ? code / (exp2(float(depth)) - 1.0) : (code - 16.0 * unit) / (219.0 * unit);
{%- endif %}
}

float decodeChroma(float code, uint depth) {
{%- if fetch == "float" %}
    return fullRange() ? code - 0.5 : (code * 255.0 - 128.0) / 224.0;
{%- else %}
    float unit = exp2(float(depth) - 8.0);

    return fullRange() ? (code - exp2(float(depth) - 1.0)) / (exp2(float(depth)) - 1.0) : (code - 128.0 * unit) / (224.0 * unit);
{%- endif %}
}

float decodeAlpha(float code, uint depth) {
{%- if fetch == "float" %}
    return code;
{%- else %}
    return code / float(depthMask(depth));
{%- endif %}
}

vec2 lumaWeights() {
    switch (matrixCode()) {
        case 1u:
            return vec2(0.2126, 0.0722);
        case 4u:
            return vec2(0.30, 0.11);
        case 5u:
        case 6u:
            return vec2(0.299, 0.114);
        case 7u:
            return vec2(0.212, 0.087);
        case 9u:
            return vec2(0.2627, 0.0593);
        case 12u:
            return vec2(primariesToXyz()[0].y, primariesToXyz()[2].y);
        default:
            return frameHeight() > 576u ? vec2(0.2126, 0.0722) : vec2(0.299, 0.114);
    }
}

vec3 yuvToRgb(float y, float cb, float cr) {
    if (matrixCode() == 8u) {
        return vec3(y - cb + cr, y + cb, y - cb - cr);
    }

    vec2 k = lumaWeights();
    float kg = 1.0 - k.x - k.y;
    float r = y + 2.0 * (1.0 - k.x) * cr;
    float b = y + 2.0 * (1.0 - k.y) * cb;

    return vec3(r, (y - k.x * r - k.y * b) / kg, b);
}

vec4 sourceColor(vec2 at) {
    uint last = componentCount() - 1u;
    float alpha = hasFlag(ALPHA) ? decodeAlpha(fullComponent(last, at), frame.depth[last]) : 1.0;

    if (hasFlag(RGB)) {
        return vec4(decodeLuma(fullComponent(0u, at), frame.depth[0]), decodeLuma(fullComponent(1u, at), frame.depth[1]), decodeLuma(fullComponent(2u, at), frame.depth[2]), alpha);
    }

    float y = decodeLuma(fullComponent(0u, at), frame.depth[0]);

    if (colorCount() == 1u) {
        return vec4(y, y, y, alpha);
    }

    if (matrixCode() == 0u) {
        return vec4(decodeLuma(chromaComponent(2u, at), frame.depth[2]), y, decodeLuma(chromaComponent(1u, at), frame.depth[1]), alpha);
    }

    return vec4(yuvToRgb(y, decodeChroma(chromaComponent(1u, at), frame.depth[1]), decodeChroma(chromaComponent(2u, at), frame.depth[2])), alpha);
}

{%- endif %}

vec3 eotf(vec3 e) {
{%- if transfer == "power" %}
    float exponent = transferCode() == 4u ? 2.2 : transferCode() == 5u ? 2.8 : transferCode() == 17u ? 2.6 : 2.4;
    float scale = transferCode() == 17u ? 52.37 / 48.0 : 1.0;

    return pow(max(e, vec3(0.0)), vec3(exponent)) * scale;
{%- elif transfer == "srgb" %}
    vec3 v = max(e, vec3(0.0));

    return mix(pow((v + 0.055) / 1.055, vec3(2.4)), v / 12.92, lessThanEqual(v, vec3(0.04045)));
{%- elif transfer == "linear" %}
    return e;
{%- elif transfer == "log" %}
    float decades = transferCode() == 9u ? 2.0 : 2.5;

    return mix(pow(vec3(10.0), (e - 1.0) * decades), vec3(0.0), lessThanEqual(e, vec3(0.0)));
{%- elif transfer == "pq" %}
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));
    vec3 nits = pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1)) * 10000.0;

    return nits / frame.white.x;
{%- elif transfer == "hlg" %}
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    const float peak = 1000.0;
    const float systemGamma = 1.2;
    vec3 v = clamp(e, 0.0, 1.0);
    vec3 scene = mix((exp((v - c) / a) + b) / 12.0, v * v / 3.0, lessThanEqual(v, vec3(0.5)));
    float luminance = max(dot(scene, vec3(primariesToXyz()[0].y, primariesToXyz()[1].y, primariesToXyz()[2].y)), 0.0);
    vec3 nits = peak * pow(luminance, systemGamma - 1.0) * scene;

    return nits / frame.white.x;
{%- endif %}
}

vec4 encodeOutput(vec3 linear, float alpha) {
    vec3 xyz = primariesToXyz() * linear;
{%- if output == "hdr" %}
    vec3 rgb = {{ xyz_to_rgb(primaries[9]) }} * xyz;

    return vec4(rgb, alpha);
{%- elif output == "sdr" %}
    vec3 rgb = clamp({{ xyz_to_rgb(primaries[1]) }} * xyz, 0.0, 1.0);

    return vec4(mix(1.055 * pow(rgb, vec3(1.0 / 2.4)) - 0.055, rgb * 12.92, lessThanEqual(rgb, vec3(0.0031308))), alpha);
{%- endif %}
}

void main() {
    vec4 color = sourceColor(vUv * vec2(frameWidth(), frameHeight()) - 0.5);

    fColor = encodeOutput(eotf(color.rgb), color.a);
}
