"""The color half of a video shader, built on gpu/ir.py: a sample's codes
to the output pixel through the decode matrix, the color system, the
transfer and the output.

Every number the player passes in the Frame uniform is a fact: a literal
when the variant knows it, a read of the uniform otherwise.
"""

import ir

M1 = [[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]]
M2_PQ = [[2048, 2048, 0], [6610, -13613, 7003], [17933, -17390, -543]]
M2_HLG = [[2048, 2048, 0], [3625, -7465, 3840], [9500, -9212, -288]]


def inverse(m):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [
        [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
        [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
        [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det],
    ]


class Color:
    def __init__(self, facts):
        self.g = ir.Graph()
        self.facts = facts or {}

    def u(self, path):
        fact = self.facts.get(path)
        if isinstance(fact, str):
            return self.u(fact)
        return self.g.uniform(path) if fact is None else self.g.const(fact)

    def rows(self, matrix, vector):
        g = self.g
        return [g.dot([g.lift(value) for value in row], vector) for row in matrix]

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
        g = self.g
        if transfer in ("curve", "identity"):
            return self.piece(g.fmax(signal, 0.0), "inverse")
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


def build(model, alpha, system, transfer, conversion, output, facts=None, indent="    "):
    color = Color(facts)
    g, u = color.g, color.u
    codes = [g.uniform(f"codes.{c}") for c in "xyzw"]
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
            signal = color.constant_luminance(signal, transfer)
        opacity = g.mul(codes[3], bias[3]) if alpha else g.const(1.0)
    sdr = output == "sdr"
    if transfer == "identity":
        rgb = [g.clamp(x, 0.0, 1.0) if sdr else g.fmax(x, 0.0) for x in signal]
    elif transfer == "curve" and conversion == "same":
        rgb = [color.piece(g.clamp(x, 0.0, 1.0) if sdr else g.fmax(x, 0.0), "curve") for x in signal]
    else:
        if system == "ictcp" and transfer == "pq":
            lms = [color.pq_light(x) for x in color.rows(inverse([[v / 4096 for v in row] for row in M2_PQ]), signal)]
            light = [g.mul(x, u("frame.light[1]")) for x in color.rows(inverse([[v / 4096 for v in row] for row in M1]), lms)]
        elif system == "ictcp":
            lms = [color.hlg_scene(x) for x in color.rows(inverse([[v / 4096 for v in row] for row in M2_HLG]), signal)]
            light = color.hlg_display(color.rows(inverse([[v / 4096 for v in row] for row in M1]), lms))
        elif transfer == "curve":
            light = [color.piece(g.fmax(x, 0.0), "curve") for x in signal]
        elif transfer == "log":
            light = [color.log_light(x) for x in signal]
        elif transfer == "pq":
            light = [g.mul(color.pq_light(x), u("frame.light[1]")) for x in signal]
        else:
            light = color.hlg_display([color.hlg_scene(x) for x in signal])
        if conversion == "convert":
            light = [g.dot([u(f"frame.toOutput[{c}][{r}]") for c in range(3)], light) for r in range(3)]
        if sdr:
            display = [g.clamp(x, 0.0, 1.0) for x in light]
            rgb = [g.select(g.le(d, 0.0031308), g.mul(d, 12.92), g.sub(g.mul(1.055, g.pow(d, 1.0 / 2.4)), 0.055)) for d in display]
        else:
            rgb = light
    for x, name in zip(signal, ("signalR", "signalG", "signalB")):
        x.name = x.name or (name if x.op not in ("const", "leaf") else None)
    lines, values = ir.emit({"r": rgb[0], "g": rgb[1], "b": rgb[2], "a": opacity}, indent)
    return "\n".join(lines + ["", f"{indent}fColor = vec4({values['r']}, {values['g']}, {values['b']}, {values['a']});"])
