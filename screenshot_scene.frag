#version 450 core

// The HDR viewer's ImGui fragment stage (ImGui_ImplVulkan_InitInfo's
// CustomShaderFragCreateInfo): the interface draws into a linear BT.2020
// scene with SDR white at 1.0; the output stage scales it to nits. The
// screenshot itself is drawn by screenshot_image.frag.

layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform sampler2D sTexture;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

vec3 srgbToLinear(vec3 c) {
    bvec3 lo = lessThanEqual(c, vec3(0.04045));
    vec3 a = c / 12.92;
    vec3 b = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(b, a, lo);
}

vec3 bt709ToBt2020(vec3 c) {
    return mat3(
        0.627404, 0.069097, 0.016391,
        0.329283, 0.919540, 0.088013,
        0.043313, 0.011362, 0.895595
    ) * c;
}

void main() {
    vec4 sampled = texture(sTexture, In.UV.st);
    vec3 tint = srgbToLinear(In.Color.rgb);
    fColor = vec4(bt709ToBt2020(srgbToLinear(sampled.rgb) * tint), sampled.a * In.Color.a);
}
