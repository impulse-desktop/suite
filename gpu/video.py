"""The player's video shader, built on gpu/ir.py: a frame's words read by
the layout's reader, decoded, carried through the color system and the
transfer to the output.

A variant is the storage layout (literal) and the chain its color system,
transfer, conversion and output take. Every number the player passes in
the Frame uniform is a fact: a literal when known, another fact's path
when equal to it, a read of the uniform otherwise.
"""

import ir

M1 = [[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]]
M2_PQ = [[2048, 2048, 0], [6610, -13613, 7003], [17933, -17390, -543]]
M2_HLG = [[2048, 2048, 0], [3625, -7465, 3840], [9500, -9212, -288]]

HEADER = """#version 450

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
"""


def inverse(m):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [
        [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
        [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
        [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det],
    ]


class Shader:
    def __init__(self, layout, facts):
        self.g = ir.Graph()
        self.facts = facts or {}
        self.layout = layout
        self.components = layout["components"]
        self.big = "be" in layout["flags"]
        self.bits = "bits" in layout["flags"]

    def u(self, path, kind="float"):
        fact = self.facts.get(path)
        if isinstance(fact, str):
            return self.u(fact, kind)
        return self.g.leaf(path, kind) if fact is None else self.g.const(fact, kind)

    def swap16(self, v):
        g = self.g
        return g.bor(g.shl(g.band(v, 0xFF), 8), g.band(g.shr(v, 8), 0xFF))

    def swap32(self, v):
        g = self.g
        return g.bor(g.bor(g.shl(g.band(v, 0xFF), 24), g.shl(g.band(v, 0xFF00), 8)), g.bor(g.band(g.shr(v, 8), 0xFF00), g.shr(v, 24)))

    def start(self, c):
        plane, step, offset, shift, depth = self.components[c]
        return offset + 1 if self.big and shift + depth <= 8 else offset

    def value(self, c, window):
        g = self.g
        plane, step, offset, shift, depth = self.components[c]
        mask = 2**depth - 1 if depth < 32 else ir.U32
        if "float" in self.layout["flags"]:
            if depth == 16:
                return g.half(self.swap16(window) if self.big else window)
            return g.bits_float(self.swap32(window) if self.big else window)
        if self.bits and depth == 10:
            bits = g.band(g.shr(self.swap32(window), offset), mask)
        elif self.bits:
            bits = window
        elif self.big and shift + depth > 16:
            bits = g.band(g.shr(self.swap32(window), shift), mask)
        elif self.big and shift + depth > 8:
            bits = g.band(g.shr(self.swap16(window), shift), mask)
        else:
            bits = g.band(g.shr(window, shift), mask)
        return g.convert(bits, "float")

    def windows(self, c, row, column):
        g = self.g
        plane, step, offset, shift, depth = self.components[c]
        first = self.start(c)
        if self.bits and depth == 10:
            return g.load(g.add(row, column))
        if self.bits:
            bit = g.add(g.mul(column, step), offset)
            byte = g.shr(bit, 3)
            place = g.sub(g.add(g.shl(g.band(byte, 3), 3), 8 - depth), g.band(bit, 7))
            return g.band(g.shr(g.load(g.add(row, g.shr(byte, 2))), place), 2**depth - 1)
        if c == 0 and "luma" in self.layout:
            group, offsets = self.layout["luma"]
            lane = g.band(column, 3)
            within = g.const(offsets[3], "uint")
            for i in (2, 1, 0):
                within = g.select(g.eq(lane, i), offsets[i], within)
            byte = g.add(g.mul(g.shr(column, 2), group), within)
            return g.shr(g.load(g.add(row, g.shr(byte, 2))), g.shl(g.band(byte, 3), 3))
        if step in (1, 2):
            index = g.shr(column, 2 if step == 1 else 1)
            place = g.mul(g.band(column, 4 // step - 1), 8 * step)
            return g.shr(g.shr(g.load(g.add(row, index)), place), 8 * first)
        if step % 4 == 0:
            index = g.mul(column, step // 4)
            return g.shr(g.load(g.add(g.add(row, index), first // 4)), 8 * (first % 4))
        byte = g.mul(column, step)
        place = g.shl(g.band(byte, 3), 3)
        last = g.load(g.add(row, g.shr(g.add(byte, step - 1), 2)))
        if first >= 4:
            return g.shr(g.shr(last, place), 8 * (first - 4))
        low = g.bor(g.shr(g.load(g.add(row, g.shr(byte, 2))), place), g.shl(g.shl(last, g.sub(24, place)), 8))
        return g.shr(low, 8 * first)

    def footprint(self, at, extent):
        g = self.g
        base = [g.floor(x) for x in at]
        f = [g.sub(x, b) for x, b in zip(at, base)]
        whole = [g.convert(b, "int") for b in base]
        last = [g.sub(g.convert(e, "int"), 1) for e in extent]
        a = [g.convert(g.fmax(w, 0), "uint") for w in whole]
        b = [g.convert(g.fmin(g.add(w, 1), l), "uint") for w, l in zip(whole, last)]
        return f, a, b

    def grid(self, at, extent, members):
        g = self.g
        f, a, b = self.footprint(at, extent)
        slots = [g.const(0.0)] * 4
        for c in members:
            plane = self.components[c][0]
            offset, line = self.u(f"frame.planeOffset[{plane}]", "uint"), self.u(f"frame.lineSize[{plane}]", "uint")
            rows = [g.shr(g.add(offset, g.mul(y, line)), 2) for y in (a[1], b[1])]
            taps = [[self.value(c, self.windows(c, row, column)) for column in (a[0], b[0])] for row in rows]
            left, right = g.mix(taps[0][0], taps[1][0], f[1]), g.mix(taps[0][1], taps[1][1], f[1])
            slot = 3 if "alpha" in self.layout["flags"] and c == len(self.components) - 1 else c
            slots[slot] = g.mix(left, right, f[0])
        return slots

    def mosaic(self, y, xs):
        g = self.g
        last = [g.sub(g.convert(self.u(f"frame.size[{i}]", "uint"), "int"), 1) for i in range(2)]
        mirror = lambda v, end: g.convert(g.sub(end, g.abs(g.sub(end, g.abs(v)))), "uint")
        step = self.components[0][1]
        row = g.shr(g.add(self.u("frame.planeOffset[0]", "uint"), g.mul(mirror(y, last[1]), self.u("frame.lineSize[0]", "uint"))), 2)
        values = []
        for x in xs:
            inside = mirror(x, last[0])
            texel = g.shr(g.load(g.add(row, g.shr(inside, 2 if step == 1 else 1))), g.mul(g.band(inside, 3 if step == 1 else 1), 8 * step))
            if step == 1:
                bits = g.band(texel, 0xFF)
            elif self.big:
                bits = self.swap16(texel)
            else:
                bits = g.band(texel, 0xFFFF)
            values.append(g.convert(bits, "float"))
        return values

    def demosaic(self, above, middle, below, right, at):
        g = self.g
        red = [g.convert(self.u(f"frame.sites[{i}]"), "uint") for i in range(2)]
        phase = [g.band(g.convert(v, "uint"), 1) for v in at]
        shifted = lambda row: [g.mix(row[i], row[i + 1], right) for i in range(3)]
        u, m, d = shifted(above), shifted(middle), shifted(below)
        horizontal = g.mul(g.add(m[0], m[2]), 0.5)
        vertical = g.mul(g.add(u[1], d[1]), 0.5)
        cross = g.mul(g.add(horizontal, vertical), 0.5)
        diagonal = g.mul(g.add(g.add(u[0], u[2]), g.add(d[0], d[2])), 0.25)
        same = [g.convert(g.eq(phase[i], red[i]), "float") for i in range(2)]
        on_red = g.mul(same[0], same[1])
        on_blue = g.mul(g.sub(1.0, same[0]), g.sub(1.0, same[1]))
        on_green = g.sub(g.sub(1.0, on_red), on_blue)
        green = [g.mix(vertical, horizontal, same[1]), m[1], g.mix(horizontal, vertical, same[1])]
        red_site, blue_site = [m[1], cross, diagonal], [diagonal, cross, m[1]]
        return [g.add(g.add(g.mul(on_red, red_site[i]), g.mul(on_blue, blue_site[i])), g.mul(on_green, green[i])) for i in range(3)]

    def bayer(self, at):
        g = self.g
        base = [g.floor(x) for x in at]
        f = [g.sub(x, b) for x, b in zip(at, base)]
        whole = [g.convert(b, "int") for b in base]
        last = [g.sub(g.convert(self.u(f"frame.size[{i}]", "uint"), "int"), 1) for i in range(2)]
        a = [g.fmax(w, 0) for w in whole]
        b = [g.fmin(g.add(w, 1), l) for w, l in zip(whole, last)]
        xs = [g.add(a[0], dx) for dx in (-1, 0, 1, 2)]
        rows = [self.mosaic(g.add(a[1], dy), xs) for dy in (-1, 0, 1, 2)]
        right = g.convert(g.lt(a[0], b[0]), "float")
        down = g.convert(g.lt(a[1], b[1]), "float")
        lower = [[g.mix(rows[i][k], rows[i + 1][k], down) for k in range(4)] for i in range(3)]
        top = [g.mix(p, q, f[0]) for p, q in zip(self.demosaic(*rows[:3], 0.0, a), self.demosaic(*rows[:3], right, [b[0], a[1]]))]
        bottom = [g.mix(p, q, f[0]) for p, q in zip(self.demosaic(*lower, 0.0, [a[0], b[1]]), self.demosaic(*lower, right, b))]
        return [g.mix(p, q, f[1]) for p, q in zip(top, bottom)] + [g.const(0.0)]

    def palette(self, at):
        g = self.g
        f, a, b = self.footprint(at, [self.u("frame.size[0]", "uint"), self.u("frame.size[1]", "uint")])
        offset, line = self.u("frame.planeOffset[0]", "uint"), self.u("frame.lineSize[0]", "uint")
        palette = g.shr(self.u("frame.planeOffset[1]", "uint"), 2)
        entries = []
        for y in (a[1], b[1]):
            row = g.shr(g.add(offset, g.mul(y, line)), 2)
            for x in (a[0], b[0]):
                index = g.band(g.shr(g.load(g.add(row, g.shr(x, 2))), g.shl(g.band(x, 3), 3)), 0xFF)
                entry = g.load(g.add(palette, index))
                entries.append([g.mul(g.convert(g.band(g.shr(entry, 8 * k), 0xFF), "float"), 1.0 / 255.0) for k in (2, 1, 0, 3)])
        left = [g.mix(p, q, f[1]) for p, q in zip(entries[0], entries[2])]
        right = [g.mix(p, q, f[1]) for p, q in zip(entries[1], entries[3])]
        return [g.mix(p, q, f[0]) for p, q in zip(left, right)]

    def codes(self):
        g = self.g
        size = [self.u(f"frame.size[{i}]", "uint") for i in range(4)]
        at = [g.sub(g.mul(g.leaf(f"vUv.{axis}"), g.convert(size[i], "float")), 0.5) for i, axis in enumerate("xy")]
        model = self.layout["model"]
        if model == "palette":
            return self.palette(at)
        if model == "bayer":
            return self.bayer(at)
        if model != "yuv":
            return self.grid(at, size[:2], range(len(self.components)))
        alpha = "alpha" in self.layout["flags"]
        luma = self.grid(at, size[:2], [0, 3] if alpha else [0])
        chroma_at = [g.sub(g.mul(at[i], self.u(f"frame.chroma[{i}]")), self.u(f"frame.chroma[{i + 2}]")) for i in range(2)]
        chroma = self.grid(chroma_at, size[2:], [1, 2])
        return [g.add(x, y) for x, y in zip(luma, chroma)]

    def piece(self, v, name):
        g, u = self.g, self.u
        curved = g.sub(g.mul(u(f"frame.{name}[0][0]"), g.pow(g.add(g.mul(v, u(f"frame.{name}[0][2]")), u(f"frame.{name}[0][3]")), u(f"frame.{name}[0][1]"))), u(f"frame.{name}[2][0]"))
        straight = g.add(g.mul(v, u(f"frame.{name}[1][0]")), u(f"frame.{name}[1][1]"))
        below = g.le(g.mul(g.sub(v, u(f"frame.{name}[2][2]")), u(f"frame.{name}[1][2]")), 0.0)
        return g.select(below, straight, curved)

    def pq_light(self, s):
        g = self.g
        p = g.pow(g.clamp(s, 0.0, 1.0), 32.0 / 2523.0)
        return g.pow(g.div(g.fmax(g.sub(p, 0.8359375), 0.0), g.sub(18.8515625, g.mul(18.6875, p))), 16384.0 / 2610.0)

    def hlg_scene(self, s):
        g = self.g
        v = g.clamp(s, 0.0, 1.0)
        return g.select(g.le(v, 0.5), g.mul(g.mul(v, v), 1.0 / 3.0), g.add(g.mul(g.exp(g.sub(g.mul(v, 5.591816310), 3.130917952)), 1.0 / 12.0), 0.28466892 / 12.0))

    def hlg_display(self, scene):
        g, u = self.g, self.u
        factor = g.mul(g.pow(g.dot(scene, [u(f"frame.luminance[{i}]") for i in range(3)]), 0.2), u("frame.light[2]"))
        return [g.mul(x, factor) for x in scene]

    def log_light(self, s):
        g = self.g
        return g.select(g.le(s, 0.0), 0.0, g.exp2(g.mul(g.sub(s, 1.0), g.mul(3.32192809489, self.u("frame.light[0]")))))

    def encode(self, light, transfer):
        g, u = self.g, self.u
        if transfer in ("curve", "identity"):
            return self.piece(g.fmax(light, 0.0), "oetf")
        if transfer == "log":
            v = g.fmax(light, 1e-30)
            return g.select(g.lt(v, g.exp2(g.mul(-3.32192809489, u("frame.light[0]")))), 0.0, g.add(1.0, g.mul(g.log2(v), g.div(0.30102999566, u("frame.light[0]")))))
        if transfer == "pq":
            y = g.pow(g.clamp(light, 0.0, 1.0), 2610.0 / 16384.0)
            return g.pow(g.div(g.add(0.8359375, g.mul(18.8515625, y)), g.add(1.0, g.mul(18.6875, y))), 2523.0 / 32.0)
        v = g.fmax(light, 0.0)
        return g.select(g.le(v, 1.0 / 12.0), g.sqrt(g.mul(3.0, v)), g.add(g.mul(0.17883277, g.log(g.fmax(g.sub(g.mul(12.0, v), 0.28466892), 1e-6))), 0.55991073))

    def decode_light(self, signal, transfer):
        if transfer in ("curve", "identity"):
            return self.piece(self.g.fmax(signal, 0.0), "inverse")
        if transfer == "log":
            return self.log_light(signal)
        if transfer == "pq":
            return self.pq_light(signal)
        return self.hlg_scene(signal)

    def constant_luminance(self, ycc, transfer):
        g, u = self.g, self.u
        kr, kb = u("frame.weights[0]"), u("frame.weights[1]")
        negative = [self.encode(g.sub(1.0, kb), transfer), self.encode(g.sub(1.0, kr), transfer)]
        positive = [g.sub(1.0, self.encode(kb, transfer)), g.sub(1.0, self.encode(kr, transfer))]
        chroma = [g.mul(g.mul(2.0, ycc[1 + i]), g.select(g.le(ycc[1 + i], 0.0), negative[i], positive[i])) for i in range(2)]
        yrb = [ycc[0], g.add(ycc[0], chroma[1]), g.add(ycc[0], chroma[0])]
        light = [self.decode_light(x, transfer) for x in yrb]
        green = g.div(g.sub(g.sub(light[0], g.mul(kr, light[1])), g.mul(kb, light[2])), g.sub(g.sub(1.0, kr), kb))
        return [yrb[1], self.encode(green, transfer), yrb[2]]

    def rows(self, matrix, vector):
        return [self.g.dot([self.g.lift(value) for value in row], vector) for row in matrix]

    def color(self, codes, system, transfer, conversion, output):
        g, u = self.g, self.u
        model = self.layout["model"]
        if model == "palette":
            signal, opacity = codes[:3], codes[3]
        else:
            decode = lambda row, column: u(f"frame.decode[{column}][{row}]")
            bias = [u(f"frame.bias[{i}]") for i in range(4)]
            if model == "yuv":
                signal = [g.add(g.dot([decode(r, c) for c in range(3)], codes[:3]), bias[r]) for r in range(3)]
            elif model == "gray":
                signal = [g.add(g.mul(codes[0], decode(0, 0)), bias[r]) for r in range(3)]
            else:
                signal = [g.add(g.mul(codes[r], decode(r, r)), bias[r]) for r in range(3)]
            if system == "cl":
                signal = self.constant_luminance(signal, transfer)
            opacity = g.mul(codes[3], bias[3]) if "alpha" in self.layout["flags"] else g.const(1.0)
        for x, name in zip(signal, ("signalR", "signalG", "signalB")):
            if x.op not in ("const", "leaf"):
                x.name = x.name or name
        sdr = output == "sdr"
        if transfer == "identity":
            return [g.clamp(x, 0.0, 1.0) if sdr else g.fmax(x, 0.0) for x in signal] + [opacity]
        if transfer == "curve" and conversion == "same":
            return [self.piece(g.clamp(x, 0.0, 1.0) if sdr else g.fmax(x, 0.0), "curve") for x in signal] + [opacity]
        if system == "ictcp" and transfer == "pq":
            lms = [self.pq_light(x) for x in self.rows(inverse([[v / 4096 for v in row] for row in M2_PQ]), signal)]
            light = [g.mul(x, u("frame.light[1]")) for x in self.rows(inverse([[v / 4096 for v in row] for row in M1]), lms)]
        elif system == "ictcp":
            lms = [self.hlg_scene(x) for x in self.rows(inverse([[v / 4096 for v in row] for row in M2_HLG]), signal)]
            light = self.hlg_display(self.rows(inverse([[v / 4096 for v in row] for row in M1]), lms))
        elif transfer == "curve":
            light = [self.piece(g.fmax(x, 0.0), "curve") for x in signal]
        elif transfer == "log":
            light = [self.log_light(x) for x in signal]
        elif transfer == "pq":
            light = [g.mul(self.pq_light(x), u("frame.light[1]")) for x in signal]
        else:
            light = self.hlg_display([self.hlg_scene(x) for x in signal])
        if conversion == "convert":
            light = [g.dot([u(f"frame.toOutput[{c}][{r}]") for c in range(3)], light) for r in range(3)]
        if not sdr:
            return light + [opacity]
        display = [g.clamp(x, 0.0, 1.0) for x in light]
        return [g.select(g.le(d, 0.0031308), g.mul(d, 12.92), g.sub(g.mul(1.055, g.pow(d, 1.0 / 2.4)), 0.055)) for d in display] + [opacity]


def shader(layout, system, transfer, conversion, output, facts=None):
    built = Shader(layout, facts)
    pixel = built.color(built.codes(), system, transfer, conversion, output)
    lines, values = ir.emit(dict(zip("rgba", pixel)))
    body = "\n".join(lines + ["", f"    fColor = vec4({values['r']}, {values['g']}, {values['b']}, {values['a']});"])
    return f"{HEADER}\nvoid main() {{\n{body}\n}}\n"
