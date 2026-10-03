{%- extends "frame.frag" %}
{%- block readers %}

vec4 mosaic(int y, ivec4 x) {
    ivec2 last = ivec2(frame.size.xy) - 1;
    uvec4 inside = uvec4(last.x - abs(last.x - abs(x)));
    uint row = (frame.planeOffset[0] + uint(last.y - abs(last.y - abs(y))) * frame.lineSize[0]) >> 2;
{%- set step = components[0][1] %}
    uvec4 texels = uvec4(words[row + inside.x / {{ 4 // step }}u], words[row + inside.y / {{ 4 // step }}u], words[row + inside.z / {{ 4 // step }}u], words[row + inside.w / {{ 4 // step }}u]) >> inside % {{ 4 // step }}u * {{ 8 * step }}u;

    return vec4({{ "texels & 0xffu" if step == 1 else "(texels & 0xffu) << 8 | texels >> 8 & 0xffu" if bigEndian else "texels & 0xffffu" }});
}

vec3 demosaic(vec4 above, vec4 middle, vec4 below, float right, ivec2 at) {
    ivec2 red = ivec2(frame.sites.xy);
    ivec2 phase = at & 1;
    vec3 u = mix(above.xyz, above.yzw, right);
    vec3 m = mix(middle.xyz, middle.yzw, right);
    vec3 d = mix(below.xyz, below.yzw, right);
    float horizontal = (m.x + m.z) / 2.0;
    float vertical = (u.y + d.y) / 2.0;
    float cross = (horizontal + vertical) / 2.0;
    float diagonal = (u.x + u.z + d.x + d.z) / 4.0;
    float onRed = float(all(equal(phase, red)));
    float onBlue = float(!any(equal(phase, red)));
    vec3 onGreen = mix(vec3(vertical, m.y, horizontal), vec3(horizontal, m.y, vertical), float(phase.y == red.y));

    return onRed * vec3(m.y, cross, diagonal) + onBlue * vec3(diagonal, cross, m.y) + (1.0 - onRed - onBlue) * onGreen;
}

vec4 bayerCodes(vec2 at) {
    vec2 base = floor(at);
    vec2 f = at - base;
    ivec2 a = max(ivec2(base), 0);
    ivec2 b = min(ivec2(base) + 1, ivec2(frame.size.xy) - 1);
    ivec4 x = a.x + ivec4(-1, 0, 1, 2);
    vec4 r0 = mosaic(a.y - 1, x), r1 = mosaic(a.y, x), r2 = mosaic(a.y + 1, x), r3 = mosaic(a.y + 2, x);
    float right = float(b.x > a.x);
    float down = float(b.y > a.y);
    vec4 s0 = mix(r0, r1, down), s1 = mix(r1, r2, down), s2 = mix(r2, r3, down);
    vec3 top = mix(demosaic(r0, r1, r2, 0.0, a), demosaic(r0, r1, r2, right, ivec2(b.x, a.y)), f.x);
    vec3 bottom = mix(demosaic(s0, s1, s2, 0.0, ivec2(a.x, b.y)), demosaic(s0, s1, s2, right, b), f.x);

    return vec4(mix(top, bottom, f.y), 0.0);
}
{%- endblock %}
{%- block codes %}bayerCodes(at){% endblock %}
