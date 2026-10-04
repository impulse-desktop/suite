#version 450 core

layout(location = 0) out vec4 fColor;

layout(set = 0, binding = 0) uniform sampler2D sTexture;

layout(location = 0) in struct {
    vec4 Color;
    vec2 UV;
} In;

vec4 tint() {
#ifdef LINEAR
    vec3 c = In.Color.rgb;

    return vec4(mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045))), In.Color.a);
#else
    return In.Color;
#endif
}

void main() {
    fColor = tint() * texture(sTexture, In.UV.st);
}
