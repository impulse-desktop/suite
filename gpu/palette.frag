{%- extends "frame.frag" %}
{%- block readers %}

vec4 paletteColor(vec2 at) {
    vec2 base = floor(at);
    vec2 f = at - base;
    uvec2 a = uvec2(max(ivec2(base), 0));
    uvec2 b = uvec2(min(ivec2(base) + 1, ivec2(frame.size.xy) - 1));
    uint top0 = (frame.planeOffset[0] + a.y * frame.lineSize[0]) >> 2;
    uint bottom0 = (frame.planeOffset[0] + b.y * frame.lineSize[0]) >> 2;
    uint palette = frame.planeOffset[1] >> 2;
    uvec2 index = uvec2(a.x, b.x) >> 2;
    uvec2 place = (uvec2(a.x, b.x) & 3u) * 8u;
    uvec2 above = {{ load("top", 0, "index") }} >> place & 0xffu;
    uvec2 below = {{ load("bottom", 0, "index") }} >> place & 0xffu;
    vec4 left = mix(unpackUnorm4x8(words[palette + above.x]), unpackUnorm4x8(words[palette + below.x]), f.y);
    vec4 right = mix(unpackUnorm4x8(words[palette + above.y]), unpackUnorm4x8(words[palette + below.y]), f.y);

    return mix(left, right, f.x).zyxw;
}
{%- endblock %}
{%- block codes %}paletteColor(at){% endblock %}
{%- block signal %}
    vec4 color = paletteColor(at);
    vec3 signal = color.rgb;
    float alpha = color.a;
{%- endblock %}
