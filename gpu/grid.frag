{%- extends "frame.frag" %}
{%- set bits = "bits" in source.flags %}
{%- set shifts = {1: 2, 2: 1} %}
{%- set rows = [["above", "top"], ["below", "bottom"]] %}

{%- macro start(c) -%}
{%- set plane, step, offset, shift, depth = components[c] -%}
{{ offset + 1 if bigEndian and shift + depth <= 8 else offset }}
{%- endmacro %}

{%- macro value(c, window) -%}
{%- set plane, step, offset, shift, depth = components[c] -%}
{%- set mask = "0x%xu" | format(2 ** depth - 1) -%}
{%- if "float" in source.flags and depth == 16 -%}
vec2(unpackHalf2x16({{ "swap16(" ~ window ~ ")" if bigEndian else window }}.x).x, unpackHalf2x16({{ "swap16(" ~ window ~ ")" if bigEndian else window }}.y).x)
{%- elif "float" in source.flags -%}
uintBitsToFloat({{ "swap32(" ~ window ~ ")" if bigEndian else window }})
{%- elif bits and depth == 10 -%}
vec2(swap32({{ window }}) >> {{ offset }}u & {{ mask }})
{%- elif bits -%}
vec2({{ window }} >> place{{ c }} & {{ mask }})
{%- elif bigEndian and shift + depth > 16 -%}
vec2(swap32({{ window }}) >> {{ shift }}u & {{ mask }})
{%- elif bigEndian and shift + depth > 8 -%}
vec2(swap16({{ window }}) >> {{ shift }}u & {{ mask }})
{%- else -%}
vec2({{ window }}{% if shift %} >> {{ shift }}u{% endif %} & {{ mask }})
{%- endif -%}
{%- endmacro %}

{%- macro grid(name, members, extent) %}

vec4 {{ name }}(vec2 at) {
    vec2 base = floor(at);
    vec2 f = at - base;
    uvec2 a = uvec2(max(ivec2(base), 0));
    uvec2 b = uvec2(min(ivec2(base) + 1, ivec2({{ extent }}) - 1));
    uvec2 column = uvec2(a.x, b.x);
{%- set planes = namespace(done=[]) %}
{%- for c in members %}
{%- set plane, step, offset, shift, depth = components[c] %}
{%- if plane not in planes.done %}
{%- set planes.done = planes.done + [plane] %}
    uint top{{ plane }} = (frame.planeOffset[{{ plane }}] + a.y * frame.lineSize[{{ plane }}]) >> 2;
    uint bottom{{ plane }} = (frame.planeOffset[{{ plane }}] + b.y * frame.lineSize[{{ plane }}]) >> 2;
{%- if bits or c == 0 and "luma" in source %}
{%- elif step in shifts %}
    uvec2 index{{ plane }} = column >> {{ shifts[step] }};
    uvec2 place{{ plane }} = (column & {{ 4 // step - 1 }}u) * {{ 8 * step }}u;
{%- for row, base in rows %}
    uvec2 {{ row }}{{ plane }} = {{ load(base, plane, "index" ~ plane) }} >> place{{ plane }};
{%- endfor %}
{%- elif step % 4 == 0 %}
    uvec2 index{{ plane }} = column * {{ step // 4 }}u;
{%- set loaded = namespace(words=[]) %}
{%- for m in members if components[m][0] == plane and (start(m) | int) // 4 not in loaded.words %}
{%- set loaded.words = loaded.words + [(start(m) | int) // 4] %}
{%- for row, base in rows %}
    uvec2 {{ row }}{{ plane }}_{{ loaded.words[-1] }} = {{ load(base, plane, "index" ~ plane, loaded.words[-1]) }};
{%- endfor %}
{%- endfor %}
{%- else %}
    uvec2 index{{ plane }} = column * {{ step }}u >> 2;
    uvec2 last{{ plane }} = (column * {{ step }}u + {{ step - 1 }}u) >> 2;
    uvec2 place{{ plane }} = (column * {{ step }}u & 3u) * 8u;
{%- for row, base in rows %}
    uvec2 {{ row }}{{ plane }}_1 = {{ load(base, plane, "last" ~ plane) }};
    uvec2 {{ row }}{{ plane }}_0 = {{ load(base, plane, "index" ~ plane) }} >> place{{ plane }} | {{ row }}{{ plane }}_1 << (24u - place{{ plane }}) << 8u;
{%- endfor %}
    above{{ plane }}_1 >>= place{{ plane }};
    below{{ plane }}_1 >>= place{{ plane }};
{%- endif %}
{%- endif %}
{%- endfor %}
{%- for c in members %}
{%- set plane, step, offset, shift, depth = components[c] %}
{%- set first = (start(c) | int) %}
{%- if bits and depth == 10 %}
    vec2 rows{{ c }} = mix({{ value(c, load("top", plane, "column")) }}, {{ value(c, load("bottom", plane, "column")) }}, f.y);
{%- elif bits or c == 0 and "luma" in source %}
{%- if bits %}
    uvec2 bit{{ c }} = column * {{ step }}u{% if offset %} + {{ offset }}u{% endif %};
    uvec2 byte{{ c }} = bit{{ c }} >> 3;
    uvec2 place{{ c }} = (byte{{ c }} & 3u) * 8u + {{ 8 - depth }}u - (bit{{ c }} & 7u);
{%- else %}
    const uint group[4] = uint[]({{ source.luma[1] | join("u, ") }}u);
    uvec2 byte{{ c }} = (column >> 2) * {{ source.luma[0] }}u + uvec2(group[column.x & 3u], group[column.y & 3u]);
    uvec2 place{{ c }} = (byte{{ c }} & 3u) * 8u;
{%- endif %}
    uvec2 word{{ c }} = byte{{ c }} >> 2;
    vec2 rows{{ c }} = mix({{ value(c, load("top", plane, "word" ~ c) ~ " >> place" ~ c if not bits else load("top", plane, "word" ~ c)) }}, {{ value(c, load("bottom", plane, "word" ~ c) ~ " >> place" ~ c if not bits else load("bottom", plane, "word" ~ c)) }}, f.y);
{%- elif step in shifts %}
    vec2 rows{{ c }} = mix({{ value(c, "(above" ~ plane ~ " >> " ~ 8 * first ~ "u)" if first else "above" ~ plane) }}, {{ value(c, "(below" ~ plane ~ " >> " ~ 8 * first ~ "u)" if first else "below" ~ plane) }}, f.y);
{%- else %}
{%- set word, within = first // 4, first % 4 %}
    vec2 rows{{ c }} = mix({{ value(c, "(above" ~ plane ~ "_" ~ word ~ " >> " ~ 8 * within ~ "u)" if within else "above" ~ plane ~ "_" ~ word) }}, {{ value(c, "(below" ~ plane ~ "_" ~ word ~ " >> " ~ 8 * within ~ "u)" if within else "below" ~ plane ~ "_" ~ word) }}, f.y);
{%- endif %}
{%- endfor %}
{%- set slots = namespace(values=["0.0", "0.0", "0.0", "0.0"]) %}
{%- for c in members %}
{%- set slot = 3 if (alpha and c == components | length - 1) else c %}
{%- set slots.values = slots.values[:slot] + ["mix(rows" ~ c ~ ".x, rows" ~ c ~ ".y, f.x)"] + slots.values[slot + 1:] %}
{%- endfor %}

    return vec4({{ slots.values | join(", ") }});
}
{%- endmacro %}
{%- block readers %}
{%- if yuv %}
{{- grid("lumaCodes", [0, 3] if alpha else [0], "frame.size.xy") }}
{{- grid("chromaCodes", [1, 2], "frame.size.zw") }}
{%- else %}
{{- grid("pixelCodes", range(components | length) | list, "frame.size.xy") }}
{%- endif %}
{%- endblock %}
{%- block codes %}{{ "lumaCodes(at) + chromaCodes(at * frame.chroma.xy - frame.chroma.zw)" if yuv else "pixelCodes(at)" }}{% endblock %}
