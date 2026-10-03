"""Scalar expressions a shader is built from, simplified as they are made.

Every value is a node in one Graph, of kind float, uint, int or bool;
equal nodes are one node, so common subexpressions are shared, and the
arguments of a commutative operation are kept in one order so that a + b
and b + a meet. The constructors fold constants, apply identities (x + 0,
x * 1, x * 0, x * -1, a multiply by a power of two as a shift, pow(x, 1),
shifts of shifts, a mask the value already fits, a select or mix whose
condition is known) and carry each value's interval, so a min, max, mask
or comparison the intervals decide goes away. emit() prints what the
outputs reach as GLSL statements.
"""

import math

INF = math.inf
U32 = 2**32 - 1


class Node:
    __slots__ = ("op", "args", "value", "kind", "lo", "hi", "name", "index")

    def __init__(self, op, args, value, kind, lo, hi, index):
        self.op, self.args, self.value, self.kind, self.lo, self.hi, self.name, self.index = op, args, value, kind, lo, hi, None, index


def wrap(value, kind):
    if kind == "uint":
        return int(value) & U32
    if kind == "int":
        value = int(value) & U32
        return value - 2**32 if value >= 2**31 else value
    return value


def bounds(kind, lo, hi):
    if kind == "uint" and (lo < 0 or hi > U32):
        return 0, U32
    if kind == "int" and (lo < -(2**31) or hi >= 2**31):
        return -(2**31), 2**31 - 1
    return lo, hi


def product_bounds(a, b):
    values = [x * y if x != 0 and y != 0 else 0 for x in (a.lo, a.hi) for y in (b.lo, b.hi)]
    return min(values), max(values)


COMMUTATIVE = {"add", "mul", "min", "max", "band", "bor", "eq"}


class Graph:
    def __init__(self):
        self.nodes = {}

    def make(self, op, args=(), value=None, kind="float", lo=-INF, hi=INF):
        if op in COMMUTATIVE and len(args) == 2 and args[0].op != "const" and args[1].op != "const" and args[1].index < args[0].index:
            args = (args[1], args[0])
        key = (op, tuple(arg.index for arg in args), value, kind)
        node = self.nodes.get(key)
        if node is None:
            lo, hi = bounds(kind, lo, hi) if kind in ("uint", "int") else (lo, hi)
            node = self.nodes[key] = Node(op, tuple(args), value, kind, lo, hi, len(self.nodes))
        return node

    def const(self, value, kind=None):
        if isinstance(value, bool) or kind == "bool":
            return self.make("const", value=bool(value), kind="bool", lo=bool(value), hi=bool(value))
        kind = kind or "float"
        value = wrap(value, kind) if kind != "float" else float(value)
        return self.make("const", value=value, kind=kind, lo=value, hi=value)

    def leaf(self, path, kind="float", lo=None, hi=None):
        default = {"float": (-INF, INF), "uint": (0, U32), "int": (-(2**31), 2**31 - 1), "bool": (False, True)}[kind]
        return self.make("leaf", value=path, kind=kind, lo=default[0] if lo is None else lo, hi=default[1] if hi is None else hi)

    def uniform(self, path, lo=None, hi=None):
        return self.leaf(path, "float", lo, hi)

    def lift(self, x, kind="float"):
        return x if isinstance(x, Node) else self.const(x, kind)

    def pair(self, a, b):
        kind = a.kind if isinstance(a, Node) else b.kind if isinstance(b, Node) else "float"
        return self.lift(a, kind), self.lift(b, kind)

    def known(self, x, value=None):
        return x.op == "const" and (value is None or x.value == value)

    def add(self, a, b):
        a, b = self.pair(a, b)
        if self.known(a) and self.known(b):
            return self.const(a.value + b.value, a.kind)
        if self.known(a):
            a, b = b, a
        if self.known(b, 0):
            return a
        if b.op == "neg":
            return self.sub(a, b.args[0])
        return self.make("add", (a, b), kind=a.kind, lo=a.lo + b.lo, hi=a.hi + b.hi)

    def sub(self, a, b):
        a, b = self.pair(a, b)
        if self.known(a) and self.known(b):
            return self.const(a.value - b.value, a.kind)
        if self.known(b, 0):
            return a
        if self.known(a, 0) and a.kind != "uint":
            return self.neg(b)
        if a is b:
            return self.const(0, a.kind)
        if self.known(b) and a.kind == "float":
            return self.add(a, -b.value)
        return self.make("sub", (a, b), kind=a.kind, lo=a.lo - b.hi, hi=a.hi - b.lo)

    def neg(self, a):
        a = self.lift(a)
        if self.known(a):
            return self.const(-a.value, a.kind)
        if a.op == "neg":
            return a.args[0]
        if a.op == "sub":
            return self.sub(a.args[1], a.args[0])
        return self.make("neg", (a,), kind=a.kind, lo=-a.hi, hi=-a.lo)

    def mul(self, a, b):
        a, b = self.pair(a, b)
        if self.known(a) and self.known(b):
            return self.const(a.value * b.value, a.kind)
        if self.known(a):
            a, b = b, a
        if self.known(b, 1):
            return a
        if self.known(b, 0):
            return self.const(0, a.kind)
        if self.known(b, -1) and a.kind != "uint":
            return self.neg(a)
        if a.kind == "uint" and self.known(b) and b.value & (b.value - 1) == 0:
            return self.shl(a, b.value.bit_length() - 1)
        lo, hi = product_bounds(a, b)
        return self.make("mul", (a, b), kind=a.kind, lo=lo, hi=hi)

    def div(self, a, b):
        a, b = self.lift(a), self.lift(b)
        if self.known(b):
            return self.mul(a, 1.0 / b.value)
        if b.lo > 0 or b.hi < 0:
            inverse = self.make("div", (self.const(1.0), b), lo=min(1 / b.lo, 1 / b.hi), hi=max(1 / b.lo, 1 / b.hi))
            return inverse if self.known(a, 1.0) else self.mul(a, inverse)
        return self.make("div", (a, b))

    def abs(self, a):
        a = self.lift(a)
        if self.known(a):
            return self.const(abs(a.value), a.kind)
        if a.lo >= 0:
            return a
        if a.hi <= 0:
            return self.neg(a)
        return self.make("abs", (a,), kind=a.kind, lo=0, hi=max(-a.lo, a.hi))

    def fmin(self, a, b):
        a, b = self.pair(a, b)
        if self.known(a) and self.known(b):
            return self.const(min(a.value, b.value), a.kind)
        if a.hi <= b.lo:
            return a
        if b.hi <= a.lo:
            return b
        return self.make("min", (a, b), kind=a.kind, lo=min(a.lo, b.lo), hi=min(a.hi, b.hi))

    def fmax(self, a, b):
        a, b = self.pair(a, b)
        if self.known(a) and self.known(b):
            return self.const(max(a.value, b.value), a.kind)
        if a.lo >= b.hi:
            return a
        if b.lo >= a.hi:
            return b
        return self.make("max", (a, b), kind=a.kind, lo=max(a.lo, b.lo), hi=max(a.hi, b.hi))

    def clamp(self, x, lo, hi):
        return self.fmin(self.fmax(x, lo), hi)

    def shr(self, a, s):
        a, s = self.lift(a, "uint"), self.lift(s, "uint")
        if self.known(s, 0):
            return a
        if self.known(a) and self.known(s):
            return self.const(a.value >> s.value if s.value < 32 else 0, "uint")
        if self.known(s) and a.op == "shr" and self.known(a.args[1]):
            total = a.args[1].value + s.value
            return self.shr(a.args[0], total) if total < 32 else self.const(0, "uint")
        if self.known(s):
            return self.make("shr", (a, s), kind="uint", lo=a.lo >> s.value, hi=a.hi >> s.value)
        return self.make("shr", (a, s), kind="uint", lo=0, hi=a.hi)

    def shl(self, a, s):
        a, s = self.lift(a, "uint"), self.lift(s, "uint")
        if self.known(s, 0):
            return a
        if self.known(a) and self.known(s):
            return self.const(a.value << s.value if s.value < 32 else 0, "uint")
        if self.known(s) and a.op == "shl" and self.known(a.args[1]):
            total = a.args[1].value + s.value
            return self.shl(a.args[0], total) if total < 32 else self.const(0, "uint")
        if self.known(s):
            return self.make("shl", (a, s), kind="uint", lo=a.lo << s.value, hi=a.hi << s.value)
        return self.make("shl", (a, s), kind="uint")

    def band(self, a, m):
        a, m = self.lift(a, "uint"), self.lift(m, "uint")
        if self.known(a) and self.known(m):
            return self.const(a.value & m.value, "uint")
        if self.known(a):
            a, m = m, a
        if self.known(m, 0):
            return self.const(0, "uint")
        if self.known(m) and m.value & (m.value + 1) == 0 and a.hi <= m.value:
            return a
        if self.known(m) and a.op == "band" and self.known(a.args[1]):
            return self.band(a.args[0], a.args[1].value & m.value)
        return self.make("band", (a, m), kind="uint", lo=0, hi=min(a.hi, m.hi))

    def bor(self, a, b):
        a, b = self.lift(a, "uint"), self.lift(b, "uint")
        if self.known(a) and self.known(b):
            return self.const(a.value | b.value, "uint")
        if self.known(a, 0):
            return b
        if self.known(b, 0):
            return a
        return self.make("bor", (a, b), kind="uint", lo=max(a.lo, b.lo), hi=(1 << max(a.hi, b.hi).bit_length()) - 1)

    def compare(self, op, a, b):
        a, b = self.pair(a, b)
        if self.known(a) and self.known(b):
            return self.const({"le": a.value <= b.value, "lt": a.value < b.value, "eq": a.value == b.value}[op])
        if op == "le" and a.hi <= b.lo or op == "lt" and a.hi < b.lo or op == "eq" and a.lo == a.hi == b.lo == b.hi:
            return self.const(True)
        if op == "le" and a.lo > b.hi or op == "lt" and a.lo >= b.hi or op == "eq" and (a.lo > b.hi or b.lo > a.hi):
            return self.const(False)
        if op != "eq" and self.known(b, 0) and a.op == "sub" and a.kind != "uint":
            return self.compare(op, a.args[0], a.args[1])
        if op != "eq" and self.known(b, 0) and a.op == "neg":
            return self.compare(op, b, a.args[0])
        return self.make(op, (a, b), kind="bool", lo=False, hi=True)

    def le(self, a, b):
        return self.compare("le", a, b)

    def lt(self, a, b):
        return self.compare("lt", a, b)

    def eq(self, a, b):
        return self.compare("eq", a, b)

    def select(self, condition, a, b):
        condition = self.lift(condition, "bool")
        a, b = self.pair(a, b)
        if self.known(condition):
            return a if condition.value else b
        if a is b:
            return a
        return self.make("select", (condition, a, b), kind=a.kind, lo=min(a.lo, b.lo), hi=max(a.hi, b.hi))

    def mix(self, a, b, t):
        t = self.lift(t)
        if self.known(t, 0.0):
            return self.lift(a)
        if self.known(t, 1.0):
            return self.lift(b)
        return self.add(a, self.mul(self.sub(b, a), t))

    def unary(self, op, a, function, kind="float"):
        a = self.lift(a)
        if self.known(a):
            return self.const(function(a.value), kind)
        return self.make(op, (a,), kind=kind, lo=function(a.lo), hi=function(a.hi))

    def floor(self, a):
        return self.unary("floor", a, lambda x: math.floor(x) if math.isfinite(x) else x)

    def exp2(self, a):
        return self.unary("exp2", a, lambda x: 2.0**x if x < 1024 else INF)

    def exp(self, a):
        return self.unary("exp", a, lambda x: math.exp(x) if x < 709 else INF)

    def log2(self, a):
        return self.unary("log2", a, lambda x: math.log2(x) if x > 0 else -INF)

    def log(self, a):
        return self.unary("log", a, lambda x: math.log(x) if x > 0 else -INF)

    def sqrt(self, a):
        return self.unary("sqrt", a, lambda x: math.sqrt(x) if x > 0 else 0.0)

    def pow(self, x, p):
        x, p = self.lift(x), self.lift(p)
        if self.known(p, 1.0) and x.lo >= 0:
            return x
        if self.known(x) and self.known(p):
            return self.const(x.value**p.value)
        return self.exp2(self.mul(p, self.log2(x)))

    def convert(self, a, kind):
        a = self.lift(a)
        if a.kind == kind:
            return a
        if self.known(a):
            value = float(a.value) if kind == "float" else wrap(math.floor(a.value) if a.kind == "float" else a.value, kind)
            return self.const(value, kind)
        lo, hi = (a.lo, a.hi) if a.kind != "bool" else (0, 1)
        if kind != "float":
            lo = math.floor(lo) if math.isfinite(lo) else -(2**31)
            hi = math.floor(hi) if math.isfinite(hi) else 2**31 - 1
            if kind == "uint" and lo < 0:
                lo, hi = 0, U32
        else:
            lo, hi = float(lo), float(hi)
        return self.make("convert", (a,), kind=kind, lo=lo, hi=hi)

    def load(self, index):
        return self.make("load", (self.lift(index, "uint"),), kind="uint", lo=0, hi=U32)

    def half(self, a):
        return self.make("half", (self.lift(a, "uint"),))

    def bits_float(self, a):
        return self.make("bitsfloat", (self.lift(a, "uint"),))

    def dot(self, a, b):
        total = self.const(0.0)
        for x, y in zip(a, b):
            total = self.add(total, self.mul(x, y))
        return total


def literal(value, kind="float"):
    if kind == "bool":
        return "true" if value else "false"
    if kind == "uint":
        return f"{value}u"
    if kind == "int":
        return str(value)
    if math.isinf(value):
        return "uintBitsToFloat(0x7f800000u)" if value > 0 else "uintBitsToFloat(0xff800000u)"
    text = f"{value:.9g}"
    return text if any(c in text for c in ".e") else text + ".0"


INFIX = {"add": "+", "sub": "-", "mul": "*", "div": "/", "le": "<=", "lt": "<", "eq": "==", "shr": ">>", "shl": "<<", "band": "&", "bor": "|"}
HEAVY = {"exp2", "exp", "log2", "log", "sqrt", "select", "div", "load", "floor"}


def emit(outputs, indent="    "):
    uses, order = {}, []

    def visit(node):
        for arg in node.args:
            uses[arg.index] = uses.get(arg.index, 0) + 1
            if uses[arg.index] == 1:
                visit(arg)
        order.append(node)

    for node in outputs.values():
        uses[node.index] = uses.get(node.index, 0) + 1
        if uses[node.index] == 1:
            visit(node)
    names, lines, taken = {}, [], set()

    def text(node):
        if node.index in names:
            return names[node.index]
        if node.op == "const":
            return literal(node.value, node.kind)
        if node.op == "leaf":
            return node.value
        args = [text(arg) for arg in node.args]
        if node.op == "add" and node.args[1].op == "const" and node.args[1].kind == "float" and node.args[1].value < 0:
            return f"({args[0]} - {literal(-node.args[1].value)})"
        if node.op in INFIX:
            return f"({args[0]} {INFIX[node.op]} {args[1]})"
        if node.op == "neg":
            return f"-{args[0]}"
        if node.op == "select":
            return f"mix({args[2]}, {args[1]}, {args[0]})"
        if node.op == "convert":
            return f"{node.kind}({args[0]})"
        if node.op == "load":
            return f"words[{args[0]}]"
        if node.op == "half":
            return f"unpackHalf2x16({args[0]}).x"
        if node.op == "bitsfloat":
            return f"uintBitsToFloat({args[0]})"
        return f"{node.op}({', '.join(args)})"

    for node in order:
        if node.op in ("const", "leaf"):
            continue
        if node.name or node.op in HEAVY or uses[node.index] > 1:
            name = node.name or f"t{len(names)}"
            while name in taken:
                name = f"{name}_"
            taken.add(name)
            value = text(node)
            names[node.index] = name
            if node.op in INFIX:
                value = value[1:-1]
            lines.append(f"{indent}{node.kind} {name} = {value};")
    return lines, {key: text(node) for key, node in outputs.items()}
