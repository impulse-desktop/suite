{%- set components = source.components %}
{%- set bigEndian = "be" in source.flags %}
{%- set alpha = "alpha" in source.flags %}
{%- set yuv = source.model == "yuv" %}
{%- set curved = transfer in ["curve", "identity"] %}

{%- macro load(row, plane, index, word=0) -%}
uvec2(words[{{ row }}{{ plane }} + {{ index }}.x{% if word %} + {{ word }}u{% endif %}], words[{{ row }}{{ plane }} + {{ index }}.y{% if word %} + {{ word }}u{% endif %}])
{%- endmacro %}

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
    vec4 chroma;
    mat3 decode;
    vec4 bias;
    vec4 sites;
    vec4 weights;
    vec4 curve[3];
    vec4 oetf[3];
    vec4 inverse[3];
    mat3 toOutput;
    vec4 light;
    vec4 luminance;
} frame;
{%- if bigEndian %}

uvec2 swap16(uvec2 value) {
    return (value & 0xffu) << 8 | value >> 8 & 0xffu;
}

uvec2 swap32(uvec2 value) {
    return (value & 0xffu) << 24 | (value & 0xff00u) << 8 | value >> 8 & 0xff00u | value >> 24;
}
{%- endif %}
{%- block readers %}{% endblock %}
{%- if transfer == "curve" or system == "cl" and curved %}

vec3 piece(vec3 x, vec4 segments[3]) {
    vec3 v = max(x, 0.0);
    vec3 curved = segments[0].x * exp2(segments[0].y * log2(v * segments[0].z + segments[0].w)) - segments[2].x;

    return min(mix(curved, v * segments[1].x + segments[1].y, lessThanEqual((v - segments[2].z) * segments[1].z, vec3(0.0))), vec3(segments[2].w));
}
{%- endif %}
{%- if transfer == "pq" %}

vec3 pqLight(vec3 signal) {
    vec3 p = pow(clamp(signal, 0.0, 1.0), vec3(32.0 / 2523.0));

    return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), vec3(16384.0 / 2610.0));
}
{%- elif transfer == "hlg" %}

vec3 hlgScene(vec3 signal) {
    vec3 v = clamp(signal, 0.0, 1.0);

    return mix(exp(v * 5.591816310 - 3.130917952) * (1.0 / 12.0) + 0.28466892 / 12.0, v * v * (1.0 / 3.0), lessThanEqual(v, vec3(0.5)));
}

vec3 hlgDisplay(vec3 scene) {
    return scene * pow(dot(scene, frame.luminance.xyz), 0.2) * frame.light.z;
}
{%- endif %}
{%- if system == "cl" %}

vec3 encodeLight(vec3 light) {
{%- if curved %}
    return piece(light, frame.oetf);
{%- elif transfer == "log" %}
    vec3 v = max(light, vec3(1e-30));

    return mix(1.0 + log2(v) * (0.30102999566 / frame.light.x), vec3(0.0), lessThan(v, vec3(exp2(-3.32192809489 * frame.light.x))));
{%- elif transfer == "pq" %}
    vec3 y = pow(clamp(light, 0.0, 1.0), vec3(2610.0 / 16384.0));

    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(2523.0 / 32.0));
{%- elif transfer == "hlg" %}
    vec3 v = max(light, 0.0);

    return mix(0.17883277 * log(max(12.0 * v - 0.28466892, 1e-6)) + 0.55991073, sqrt(3.0 * v), lessThanEqual(v, vec3(1.0 / 12.0)));
{%- endif %}
}

vec3 decodeLight(vec3 signal) {
{%- if curved %}
    return piece(signal, frame.inverse);
{%- elif transfer == "log" %}
    return mix(exp2((signal - 1.0) * (3.32192809489 * frame.light.x)), vec3(0.0), lessThanEqual(signal, vec3(0.0)));
{%- elif transfer == "pq" %}
    return pqLight(signal);
{%- elif transfer == "hlg" %}
    return hlgScene(signal);
{%- endif %}
}

vec3 constantLuminance(vec3 ycc) {
    vec2 k = frame.weights.xy;
    vec2 negative = encodeLight(vec3(1.0 - k.y, 1.0 - k.x, 0.0)).xy;
    vec2 positive = 1.0 - encodeLight(vec3(k.y, k.x, 0.0)).xy;
    vec2 chroma = 2.0 * ycc.yz * mix(positive, negative, lessThanEqual(ycc.yz, vec2(0.0)));
    vec3 yrb = vec3(ycc.x, ycc.x + chroma.y, ycc.x + chroma.x);
    vec3 light = decodeLight(yrb);
    float g = (light.x - k.x * light.y - k.y * light.z) / (1.0 - k.x - k.y);

    return vec3(yrb.y, encodeLight(vec3(g)).x, yrb.z);
}
{%- endif %}

void main() {
    vec2 at = vUv * vec2(frame.size.xy) - 0.5;
{%- block signal %}
    vec4 codes = {% block codes %}{% endblock %};
{%- set decoded = "frame.decode * codes.xyz" if yuv else "vec3(codes.x * frame.decode[0].x)" if source.model == "gray" else "codes.xyz * vec3(frame.decode[0].x, frame.decode[1].y, frame.decode[2].z)" %}
    vec3 signal = {{ "constantLuminance(" ~ decoded ~ " + frame.bias.xyz)" if system == "cl" else decoded ~ " + frame.bias.xyz" }};
    float alpha = {{ "codes.w * frame.bias.w" if alpha else "1.0" }};
{%- endblock %}
{%- if transfer == "identity" %}

    fColor = vec4(clamp(signal, 0.0, frame.curve[2].w), alpha);
{%- elif transfer == "curve" and conversion == "same" %}

    fColor = vec4(piece(signal, frame.curve), alpha);
{%- else %}
{%- if system == "ictcp" and transfer == "pq" %}
    vec3 light = {{ inverse_matrix([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], 4096) }} * pqLight({{ inverse_matrix([[2048, 2048, 0], [6610, -13613, 7003], [17933, -17390, -543]], 4096) }} * signal) * frame.light.y;
{%- elif system == "ictcp" and transfer == "hlg" %}
    vec3 light = hlgDisplay({{ inverse_matrix([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], 4096) }} * hlgScene({{ inverse_matrix([[2048, 2048, 0], [3625, -7465, 3840], [9500, -9212, -288]], 4096) }} * signal));
{%- elif transfer == "curve" %}
    vec3 light = piece(signal, frame.curve);
{%- elif transfer == "log" %}
    vec3 light = mix(exp2((signal - 1.0) * (3.32192809489 * frame.light.x)), vec3(0.0), lessThanEqual(signal, vec3(0.0)));
{%- elif transfer == "pq" %}
    vec3 light = pqLight(signal) * frame.light.y;
{%- elif transfer == "hlg" %}
    vec3 light = hlgDisplay(hlgScene(signal));
{%- endif %}
{%- set rgb = "frame.toOutput * light" if conversion == "convert" else "light" %}
{%- if output == "hdr" %}

    fColor = vec4({{ rgb }}, alpha);
{%- else %}
    vec3 display = clamp({{ rgb }}, 0.0, 1.0);

    fColor = vec4(mix(1.055 * pow(display, vec3(1.0 / 2.4)) - 0.055, display * 12.92, lessThanEqual(display, vec3(0.0031308))), alpha);
{%- endif %}
{%- endif %}
}
