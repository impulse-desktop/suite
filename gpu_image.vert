#version 450 core


layout(push_constant) uniform PushConstant {
    vec2 scale;
    vec2 translate;
    vec4 rect;
    float sdrWhiteNits;
} pc;

layout(location = 0) out vec2 vUv;

void main() {
    const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    vec2 corner = corners[gl_VertexIndex];
    vUv = corner;
    gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, corner) * pc.scale + pc.translate, 0.0, 1.0);
}
