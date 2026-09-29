#version 450 core

layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform sampler2D scene;

vec3 pqEncode(vec3 nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    vec3 p = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

vec3 dither(vec3 encoded) {
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy,
        vec2(0.06711056, 0.00583715)))) - 0.5;

    return clamp(encoded + noise / 1023.0, 0.0, 1.0);
}

void main() {
    ivec2 size = textureSize(scene, 0);
    vec2 uv = gl_FragCoord.xy / vec2(size);
    fColor = vec4(dither(pqEncode(texture(scene, uv).rgb)), 1.0);
}
