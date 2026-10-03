#version 450 core


layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform sampler2D sTexture;

layout(push_constant) uniform PushConstant {
    vec2 scale;
    vec2 translate;
    vec4 rect;
    float sdrWhiteNits;
} pc;

vec3 pqDecode(vec3 e) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    vec3 p = pow(max(e, 0.0), vec3(1.0 / m2));
    return pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1)) * 10000.0;
}

void main() {
    fColor = vec4(pqDecode(texture(sTexture, vUv).rgb) / pc.sdrWhiteNits, 1.0);
}
