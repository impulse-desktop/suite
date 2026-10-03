"""Scalar expressions a shader is built from, simplified as they are made.

Every value is a node in one Graph; equal nodes are one node, so common
subexpressions are shared. The constructors fold constants, apply
identities (x + 0, x * 1, x * 0, x * -1, pow(x, 1), a select or mix whose
condition is known) and carry each value's interval, so a min, max or
comparison whose outcome the intervals decide disappears. emit() prints
what the outputs reach as GLSL statements.
"""

import math

INF = math.inf


class Node:
    __slots__ = ("op", "args", "value", "kind", "lo", "hi", "name")

    def __init__(self, op, args, value, kind, lo, hi):
        self.op, self.args, self.value, self.kind, self.lo, self.hi, self.name = op, args, value, kind, lo, hi, None


def product_bounds(a, b):
    values = [x * y if not (x == 0 or y == 0) else 0.0 for x in (a.lo, a.hi) for y in (b.lo, b.hi)]
    return min(values), max(values)


class Graph:
    def __init__(self):
        self.nodes = {}

    def make(self, op, args=(), value=None, kind="float", lo=-INF, hi=INF):
        key = (op, tuple(id(arg) for arg in args), value if not isinstance(value, float) or not math.isnan(value) else "nan")
        node = self.nodes.get(key)
        if node is None:
            node = self.nodes[key] = Node(op, tuple(args), value, kind, lo, hi)
        return node

    def const(self, value):
        if isinstance(value, bool):
            return self.make("const", value=value, kind="bool", lo=value, hi=value)
        value = float(value)
        return self.make("const", value=value, lo=value, hi=value)

    def uniform(self, path, lo=-INF, hi=INF):
        return self.make("leaf", value=path, lo=lo, hi=hi)

    def lift(self, x):
        return x if isinstance(x, Node) else self.const(x)

    def known(self, x, value=None):
        return x.op == "const" and (value is None or x.value == value)

    def add(self, a, b):
        a, b = self.lift(a), self.lift(b)
        if self.known(a) and self.known(b):
            return self.const(a.value + b.value)
        if self.known(a):
            a, b = b, a
        if self.known(b, 0.0):
            return a
        if b.op == "neg":
            return self.sub(a, b.args[0])
        return self.make("add", (a, b), lo=a.lo + b.lo, hi=a.hi + b.hi)

    def sub(self, a, b):
        a, b = self.lift(a), self.lift(b)
        if self.known(a) and self.known(b):
            return self.const(a.value - b.value)
        if self.known(b, 0.0):
            return a
        if self.known(a, 0.0):
            return self.neg(b)
        if a is b:
            return self.const(0.0)
        if self.known(b):
            return self.add(a, -b.value)
        return self.make("sub", (a, b), lo=a.lo - b.hi, hi=a.hi - b.lo)

    def neg(self, a):
        a = self.lift(a)
        if self.known(a):
            return self.const(-a.value)
        if a.op == "neg":
            return a.args[0]
        if a.op == "sub":
            return self.sub(a.args[1], a.args[0])
        return self.make("neg", (a,), lo=-a.hi, hi=-a.lo)

    def mul(self, a, b):
        a, b = self.lift(a), self.lift(b)
        if self.known(a) and self.known(b):
            return self.const(a.value * b.value)
        if self.known(a):
            a, b = b, a
        if self.known(b, 1.0):
            return a
        if self.known(b, 0.0):
            return self.const(0.0)
        if self.known(b, -1.0):
            return self.neg(a)
        lo, hi = product_bounds(a, b)
        return self.make("mul", (a, b), lo=lo, hi=hi)

    def div(self, a, b):
        a, b = self.lift(a), self.lift(b)
        if self.known(b):
            return self.mul(a, 1.0 / b.value)
        if b.lo > 0 or b.hi < 0:
            inverse = self.make("div", (self.const(1.0), b), lo=min(1 / b.lo, 1 / b.hi), hi=max(1 / b.lo, 1 / b.hi))
            return self.mul(a, inverse) if not self.known(a, 1.0) else inverse
        return self.make("div", (a, b))

    def fmin(self, a, b):
        a, b = self.lift(a), self.lift(b)
        if self.known(a) and self.known(b):
            return self.const(min(a.value, b.value))
        if a.hi <= b.lo:
            return a
        if b.hi <= a.lo:
            return b
        if self.known(a):
            a, b = b, a
        return self.make("min", (a, b), lo=min(a.lo, b.lo), hi=min(a.hi, b.hi))

    def fmax(self, a, b):
        a, b = self.lift(a), self.lift(b)
        if self.known(a) and self.known(b):
            return self.const(max(a.value, b.value))
        if a.lo >= b.hi:
            return a
        if b.lo >= a.hi:
            return b
        if self.known(a):
            a, b = b, a
        return self.make("max", (a, b), lo=max(a.lo, b.lo), hi=max(a.hi, b.hi))

    def clamp(self, x, lo, hi):
        return self.fmin(self.fmax(x, lo), hi)

    def compare(self, op, a, b):
        a, b = self.lift(a), self.lift(b)
        if self.known(a) and self.known(b):
            return self.const({"le": a.value <= b.value, "lt": a.value < b.value}[op])
        if op == "le" and a.hi <= b.lo or op == "lt" and a.hi < b.lo:
            return self.const(True)
        if op == "le" and a.lo > b.hi or op == "lt" and a.lo >= b.hi:
            return self.const(False)
        if self.known(b, 0.0) and a.op == "sub":
            return self.compare(op, a.args[0], a.args[1])
        if self.known(b, 0.0) and a.op == "neg":
            return self.compare(op, b, a.args[0])
        return self.make(op, (a, b), kind="bool", lo=False, hi=True)

    def le(self, a, b):
        return self.compare("le", a, b)

    def lt(self, a, b):
        return self.compare("lt", a, b)

    def select(self, condition, a, b):
        condition, a, b = self.lift(condition), self.lift(a), self.lift(b)
        if self.known(condition):
            return a if condition.value else b
        if a is b:
            return a
        return self.make("select", (condition, a, b), lo=min(a.lo, b.lo), hi=max(a.hi, b.hi))

    def mix(self, a, b, t):
        t = self.lift(t)
        if self.known(t, 0.0):
            return self.lift(a)
        if self.known(t, 1.0):
            return self.lift(b)
        return self.add(a, self.mul(self.sub(b, a), t))

    def unary(self, op, a, function, monotone=True):
        a = self.lift(a)
        if self.known(a):
            return self.const(function(a.value))
        lo, hi = (function(a.lo), function(a.hi)) if monotone else (-INF, INF)
        return self.make(op, (a,), lo=lo, hi=hi)

    def exp2(self, a):
        return self.unary("exp2", a, lambda x: 2.0**x if x < 1024 else INF)

    def exp(self, a):
        return self.unary("exp", a, lambda x: math.exp(x) if x < 709 else INF)

    def log2(self, a):
        return self.unary("log2", a, lambda x: math.log2(x) if x > 0 else -INF if x == 0 else -INF)

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

    def dot(self, a, b):
        total = self.const(0.0)
        for x, y in zip(a, b):
            total = self.add(total, self.mul(x, y))
        return total

    def matrix(self, columns, vector):
        return [self.dot([column[row] for column in columns], vector) for row in range(len(vector))]


def literal(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if math.isinf(value):
        return "uintBitsToFloat(0x7f800000u)" if value > 0 else "uintBitsToFloat(0xff800000u)"
    text = f"{value:.9g}"
    return text if any(c in text for c in ".e") else text + ".0"


INFIX = {"add": "+", "sub": "-", "mul": "*", "div": "/", "le": "<=", "lt": "<"}
HEAVY = {"exp2", "exp", "log2", "log", "sqrt", "select", "div"}


def emit(outputs, indent="    "):
    uses, order = {}, []

    def visit(node):
        for arg in node.args:
            uses[id(arg)] = uses.get(id(arg), 0) + 1
            if uses[id(arg)] == 1:
                visit(arg)
        order.append(node)

    for node in outputs.values():
        uses[id(node)] = uses.get(id(node), 0) + 1
        if uses[id(node)] == 1:
            visit(node)
    names, lines = {}, []

    def text(node):
        if id(node) in names:
            return names[id(node)]
        if node.op == "const":
            return literal(node.value)
        if node.op == "leaf":
            return node.value
        args = [text(arg) for arg in node.args]
        if node.op == "add" and node.args[1].op == "const" and node.args[1].value < 0:
            return f"({args[0]} - {literal(-node.args[1].value)})"
        if node.op in INFIX:
            return f"({args[0]} {INFIX[node.op]} {args[1]})"
        if node.op == "neg":
            return f"-{args[0]}"
        if node.op == "select":
            return f"mix({args[2]}, {args[1]}, {args[0]})"
        return f"{node.op}({', '.join(args)})"

    for node in order:
        if node.op in ("const", "leaf"):
            continue
        if node.name or node.op in HEAVY or uses[id(node)] > 1:
            name = node.name or f"t{len(names)}"
            if any(existing == name for existing in names.values()):
                name = f"{name}{len(names)}"
            value = text(node)
            names[id(node)] = name
            if node.op in INFIX:
                value = value[1:-1]
            lines.append(f"{indent}{'bool' if node.kind == 'bool' else 'float'} {name} = {value};")
    return lines, {key: text(node) for key, node in outputs.items()}
