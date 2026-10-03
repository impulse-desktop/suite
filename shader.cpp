#include "shader.h"

#include "error.h"

#include <std/str/view.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <string.h>
#include <video_codes.h>

using namespace stl;

namespace {
    enum class Op: u8 {
        Const,
        Input,
        Add,
        Sub,
        Neg,
        Mul,
        Div,
        Abs,
        Min,
        Max,
        Shr,
        Shl,
        And,
        Or,
        Le,
        Lt,
        Eq,
        Select,
        Floor,
        Exp2,
        Exp,
        Log2,
        Log,
        Sqrt,
        Convert,
        Load,
        Half,
        BitsFloat,
    };

    enum class Kind: u8 {
        Float,
        Uint,
        Int,
        Bool,
    };

    struct Node {
        Op op;
        Kind kind;
        u8 arity;
        Node* args[3];
        double value;
        double lo;
        double hi;
        u32 index;
        u32 id;
    };

    constexpr double word = 4294967295.;

    static bool commutative(Op op) {
        return op == Op::Add || op == Op::Mul || op == Op::Min || op == Op::Max || op == Op::And || op == Op::Or || op == Op::Eq;
    }

    static double wrapped(double value, Kind kind) {
        if (kind == Kind::Uint) {
            return (double)(u32)(u64)(i64)value;
        }

        if (kind == Kind::Int) {
            return (double)(i32)(u32)(u64)(i64)value;
        }

        if (kind == Kind::Bool) {
            return value != 0. ? 1. : 0.;
        }

        return value;
    }

    static double product(double x, double y) {
        return x == 0. || y == 0. ? 0. : x * y;
    }

    static double bitLength(double value) {
        double length = 0.;

        while (value >= 1.) {
            value = floor(value / 2.);
            length += 1.;
        }

        return length;
    }

    static u64 hashOf(Op op, Kind kind, int arity, Node* const* args, double value) {
        u64 hash = (u64)op * 0x9e3779b97f4a7c15ull ^ (u64)kind * 0xc2b2ae3d27d4eb4full;
        u64 bits;

        memcpy(&bits, &value, sizeof(bits));

        for (int i = 0; i < arity; i++) {
            hash = (hash ^ args[i]->index) * 0x100000001b3ull;
        }

        return (hash ^ bits) * 0xff51afd7ed558ccdull;
    }

    static Kind common(Node* a, Node*) {
        return a->kind;
    }

    static Kind common(Node* a, double) {
        return a->kind;
    }

    static Kind common(double, Node* b) {
        return b->kind;
    }

    struct Graph {
        ObjPool& pool;
        Node** slots;
        size_t capacity = 1024;
        u32 count = 0;

        explicit Graph(ObjPool& owner)
            : pool(owner)
            , slots(table(1024))
        {
        }

        Node** table(size_t size) {
            Node** out = (Node**)pool.allocate(size * sizeof(Node*));

            memset(out, 0, size * sizeof(Node*));

            return out;
        }

        Node* make(Op op, Kind kind, int arity, Node* a, Node* b, Node* c, double value, double lo, double hi) {
            Node* args[3] = {a, b, c};

            if (commutative(op) && args[0]->op != Op::Const && args[1]->op != Op::Const && args[1]->index < args[0]->index) {
                args[0] = b;
                args[1] = a;
            }

            size_t mask = capacity - 1;
            size_t at = hashOf(op, kind, arity, args, value) & mask;

            for (; slots[at]; at = (at + 1) & mask) {
                Node* node = slots[at];
                bool same = node->op == op && node->kind == kind && node->arity == arity && memcmp(&node->value, &value, sizeof(value)) == 0;

                for (int i = 0; same && i < arity; i++) {
                    same = node->args[i] == args[i];
                }

                if (same) {
                    return node;
                }
            }

            if (kind == Kind::Uint && (lo < 0. || hi > word)) {
                lo = 0.;
                hi = word;
            }

            if (kind == Kind::Int && (lo < -2147483648. || hi > 2147483647.)) {
                lo = -2147483648.;
                hi = 2147483647.;
            }

            Node* node = pool.make<Node>(Node{op, kind, (u8)arity, {args[0], args[1], args[2]}, value, lo, hi, count++, 0});

            slots[at] = node;

            if (count * 2 > capacity) {
                Node** old = slots;
                size_t size = capacity;

                capacity *= 2;
                slots = table(capacity);

                for (size_t i = 0; i < size; i++) {
                    if (Node* entry = old[i]) {
                        size_t place = hashOf(entry->op, entry->kind, entry->arity, entry->args, entry->value) & (capacity - 1);

                        while (slots[place]) {
                            place = (place + 1) & (capacity - 1);
                        }

                        slots[place] = entry;
                    }
                }
            }

            return node;
        }

        Node* constant(double value, Kind kind) {
            value = wrapped(value, kind);

            return make(Op::Const, kind, 0, nullptr, nullptr, nullptr, value, value, value);
        }

        Node* f(double value) {
            return constant(value, Kind::Float);
        }

        Node* u(double value) {
            return constant(value, Kind::Uint);
        }

        Node* i(double value) {
            return constant(value, Kind::Int);
        }

        Node* input(int component) {
            return make(Op::Input, Kind::Float, 0, nullptr, nullptr, nullptr, component, 0., 1.);
        }

        Node* lift(Node* x, Kind) {
            return x;
        }

        Node* lift(double x, Kind kind) {
            return constant(x, kind);
        }

        template <class A, class B>
        Node* add(A a, B b) {
            Kind kind = common(a, b);

            return addValues(lift(a, kind), lift(b, kind));
        }

        template <class A, class B>
        Node* sub(A a, B b) {
            Kind kind = common(a, b);

            return subValues(lift(a, kind), lift(b, kind));
        }

        template <class A, class B>
        Node* mul(A a, B b) {
            Kind kind = common(a, b);

            return mulValues(lift(a, kind), lift(b, kind));
        }

        template <class A, class B>
        Node* div(A a, B b) {
            return divValues(lift(a, Kind::Float), lift(b, Kind::Float));
        }

        template <class A, class B>
        Node* min(A a, B b) {
            Kind kind = common(a, b);

            return minValues(lift(a, kind), lift(b, kind));
        }

        template <class A, class B>
        Node* max(A a, B b) {
            Kind kind = common(a, b);

            return maxValues(lift(a, kind), lift(b, kind));
        }

        template <class A, class B, class C>
        Node* clamp(A x, B lo, C hi) {
            return min(max(x, lo), hi);
        }

        template <class A, class B>
        Node* shr(A a, B s) {
            return shrValues(lift(a, Kind::Uint), lift(s, Kind::Uint));
        }

        template <class A, class B>
        Node* shl(A a, B s) {
            return shlValues(lift(a, Kind::Uint), lift(s, Kind::Uint));
        }

        template <class A, class B>
        Node* band(A a, B m) {
            return andValues(lift(a, Kind::Uint), lift(m, Kind::Uint));
        }

        template <class A, class B>
        Node* bor(A a, B b) {
            return orValues(lift(a, Kind::Uint), lift(b, Kind::Uint));
        }

        template <class A, class B>
        Node* le(A a, B b) {
            Kind kind = common(a, b);

            return compare(Op::Le, lift(a, kind), lift(b, kind));
        }

        template <class A, class B>
        Node* lt(A a, B b) {
            Kind kind = common(a, b);

            return compare(Op::Lt, lift(a, kind), lift(b, kind));
        }

        template <class A, class B>
        Node* eq(A a, B b) {
            Kind kind = common(a, b);

            return compare(Op::Eq, lift(a, kind), lift(b, kind));
        }

        template <class A, class B>
        Node* select(Node* condition, A a, B b) {
            Kind kind = common(a, b);

            return selectValues(condition, lift(a, kind), lift(b, kind));
        }

        template <class A, class B>
        Node* mix(A a, B b, Node* t) {
            Kind kind = common(a, b);

            return mixValues(lift(a, kind), lift(b, kind), t);
        }

        template <class A, class B>
        Node* pow(A x, B p) {
            return powValues(lift(x, Kind::Float), lift(p, Kind::Float));
        }

        Node* addValues(Node* a, Node* b) {
            if (a->op == Op::Const && b->op == Op::Const) {
                return constant(a->value + b->value, a->kind);
            }

            if (a->op == Op::Const) {
                Node* first = a;

                a = b;
                b = first;
            }

            if (b->op == Op::Const && b->value == 0.) {
                return a;
            }

            if (b->op == Op::Neg) {
                return subValues(a, b->args[0]);
            }

            return make(Op::Add, a->kind, 2, a, b, nullptr, 0., a->lo + b->lo, a->hi + b->hi);
        }

        Node* subValues(Node* a, Node* b) {
            if (a->op == Op::Const && b->op == Op::Const) {
                return constant(a->value - b->value, a->kind);
            }

            if (b->op == Op::Const && b->value == 0.) {
                return a;
            }

            if (a->op == Op::Const && a->value == 0. && a->kind != Kind::Uint) {
                return neg(b);
            }

            if (a == b) {
                return constant(0., a->kind);
            }

            if (b->op == Op::Const && a->kind == Kind::Float) {
                return addValues(a, f(-b->value));
            }

            return make(Op::Sub, a->kind, 2, a, b, nullptr, 0., a->lo - b->hi, a->hi - b->lo);
        }

        Node* neg(Node* a) {
            if (a->op == Op::Const) {
                return constant(-a->value, a->kind);
            }

            if (a->op == Op::Neg) {
                return a->args[0];
            }

            if (a->op == Op::Sub) {
                return subValues(a->args[1], a->args[0]);
            }

            return make(Op::Neg, a->kind, 1, a, nullptr, nullptr, 0., -a->hi, -a->lo);
        }

        Node* mulValues(Node* a, Node* b) {
            if (a->op == Op::Const && b->op == Op::Const) {
                if (a->kind == Kind::Float) {
                    return constant(a->value * b->value, a->kind);
                }

                return constant((double)(u32)((u64)(i64)a->value * (u64)(i64)b->value), a->kind);
            }

            if (a->op == Op::Const) {
                Node* first = a;

                a = b;
                b = first;
            }

            if (b->op == Op::Const) {
                if (b->value == 1.) {
                    return a;
                }

                if (b->value == 0.) {
                    return constant(0., a->kind);
                }

                if (b->value == -1. && a->kind != Kind::Uint) {
                    return neg(a);
                }

                if (a->kind == Kind::Uint && b->value > 0. && ::exp2(::floor(::log2(b->value))) == b->value) {
                    return shlValues(a, u(::log2(b->value)));
                }
            }

            double corners[4] = {product(a->lo, b->lo), product(a->lo, b->hi), product(a->hi, b->lo), product(a->hi, b->hi)};
            double lo = corners[0];
            double hi = corners[0];

            for (double corner : corners) {
                lo = corner < lo ? corner : lo;
                hi = corner > hi ? corner : hi;
            }

            return make(Op::Mul, a->kind, 2, a, b, nullptr, 0., lo, hi);
        }

        Node* divValues(Node* a, Node* b) {
            if (b->op == Op::Const) {
                return mulValues(a, f(1. / b->value));
            }

            if (b->lo > 0. || b->hi < 0.) {
                double first = 1. / b->lo;
                double second = 1. / b->hi;
                Node* inverse = make(Op::Div, Kind::Float, 2, f(1.), b, nullptr, 0., first < second ? first : second, first < second ? second : first);

                return a->op == Op::Const && a->value == 1. ? inverse : mulValues(a, inverse);
            }

            return make(Op::Div, Kind::Float, 2, a, b, nullptr, 0., -INFINITY, INFINITY);
        }

        Node* abs(Node* a) {
            if (a->op == Op::Const) {
                return constant(a->value < 0. ? -a->value : a->value, a->kind);
            }

            if (a->lo >= 0.) {
                return a;
            }

            if (a->hi <= 0.) {
                return neg(a);
            }

            return make(Op::Abs, a->kind, 1, a, nullptr, nullptr, 0., 0., -a->lo > a->hi ? -a->lo : a->hi);
        }

        Node* minValues(Node* a, Node* b) {
            if (a->op == Op::Const && b->op == Op::Const) {
                return constant(a->value < b->value ? a->value : b->value, a->kind);
            }

            if (a->hi <= b->lo) {
                return a;
            }

            if (b->hi <= a->lo) {
                return b;
            }

            return make(Op::Min, a->kind, 2, a, b, nullptr, 0., a->lo < b->lo ? a->lo : b->lo, a->hi < b->hi ? a->hi : b->hi);
        }

        Node* maxValues(Node* a, Node* b) {
            if (a->op == Op::Const && b->op == Op::Const) {
                return constant(a->value > b->value ? a->value : b->value, a->kind);
            }

            if (a->lo >= b->hi) {
                return a;
            }

            if (b->lo >= a->hi) {
                return b;
            }

            return make(Op::Max, a->kind, 2, a, b, nullptr, 0., a->lo > b->lo ? a->lo : b->lo, a->hi > b->hi ? a->hi : b->hi);
        }

        Node* shrValues(Node* a, Node* s) {
            if (s->op == Op::Const && s->value == 0.) {
                return a;
            }

            if (a->op == Op::Const && s->op == Op::Const) {
                return u(s->value < 32. ? (double)((u32)a->value >> (u32)s->value) : 0.);
            }

            if (s->op == Op::Const && a->op == Op::Shr && a->args[1]->op == Op::Const) {
                double total = a->args[1]->value + s->value;

                return total < 32. ? shrValues(a->args[0], u(total)) : u(0.);
            }

            if (s->op == Op::Const) {
                double scale = ::exp2(s->value);

                return make(Op::Shr, Kind::Uint, 2, a, s, nullptr, 0., ::floor(a->lo / scale), ::floor(a->hi / scale));
            }

            return make(Op::Shr, Kind::Uint, 2, a, s, nullptr, 0., 0., a->hi);
        }

        Node* shlValues(Node* a, Node* s) {
            if (s->op == Op::Const && s->value == 0.) {
                return a;
            }

            if (a->op == Op::Const && s->op == Op::Const) {
                return u(s->value < 32. ? (double)(u32)((u64)a->value << (u32)s->value) : 0.);
            }

            if (s->op == Op::Const && a->op == Op::Shl && a->args[1]->op == Op::Const) {
                double total = a->args[1]->value + s->value;

                return total < 32. ? shlValues(a->args[0], u(total)) : u(0.);
            }

            if (s->op == Op::Const) {
                double scale = ::exp2(s->value);

                return make(Op::Shl, Kind::Uint, 2, a, s, nullptr, 0., a->lo * scale, a->hi * scale);
            }

            return make(Op::Shl, Kind::Uint, 2, a, s, nullptr, 0., 0., word);
        }

        Node* andValues(Node* a, Node* m) {
            if (a->op == Op::Const && m->op == Op::Const) {
                return u((double)((u32)a->value & (u32)m->value));
            }

            if (a->op == Op::Const) {
                Node* first = a;

                a = m;
                m = first;
            }

            if (m->op == Op::Const) {
                u32 bits = (u32)m->value;

                if (bits == 0) {
                    return u(0.);
                }

                if ((bits & (bits + 1)) == 0 && a->hi <= m->value) {
                    return a;
                }

                if (a->op == Op::And && a->args[1]->op == Op::Const) {
                    return andValues(a->args[0], u((double)((u32)a->args[1]->value & bits)));
                }
            }

            return make(Op::And, Kind::Uint, 2, a, m, nullptr, 0., 0., a->hi < m->hi ? a->hi : m->hi);
        }

        Node* orValues(Node* a, Node* b) {
            if (a->op == Op::Const && b->op == Op::Const) {
                return u((double)((u32)a->value | (u32)b->value));
            }

            if (a->op == Op::Const && a->value == 0.) {
                return b;
            }

            if (b->op == Op::Const && b->value == 0.) {
                return a;
            }

            double top = a->hi > b->hi ? a->hi : b->hi;

            return make(Op::Or, Kind::Uint, 2, a, b, nullptr, 0., a->lo > b->lo ? a->lo : b->lo, ::exp2(bitLength(top)) - 1.);
        }

        Node* compare(Op op, Node* a, Node* b) {
            if (a->op == Op::Const && b->op == Op::Const) {
                bool result = op == Op::Le ? a->value <= b->value : op == Op::Lt ? a->value < b->value : a->value == b->value;

                return constant(result ? 1. : 0., Kind::Bool);
            }

            if ((op == Op::Le && a->hi <= b->lo) || (op == Op::Lt && a->hi < b->lo) || (op == Op::Eq && a->lo == a->hi && b->lo == b->hi && a->lo == b->lo)) {
                return constant(1., Kind::Bool);
            }

            if ((op == Op::Le && a->lo > b->hi) || (op == Op::Lt && a->lo >= b->hi) || (op == Op::Eq && (a->lo > b->hi || b->lo > a->hi))) {
                return constant(0., Kind::Bool);
            }

            if (op != Op::Eq && b->op == Op::Const && b->value == 0. && a->op == Op::Sub && a->kind != Kind::Uint) {
                return compare(op, a->args[0], a->args[1]);
            }

            if (op != Op::Eq && b->op == Op::Const && b->value == 0. && a->op == Op::Neg) {
                return compare(op, b, a->args[0]);
            }

            return make(op, Kind::Bool, 2, a, b, nullptr, 0., 0., 1.);
        }

        Node* selectValues(Node* condition, Node* a, Node* b) {
            if (condition->op == Op::Const) {
                return condition->value != 0. ? a : b;
            }

            if (a == b) {
                return a;
            }

            return make(Op::Select, a->kind, 3, condition, a, b, 0., a->lo < b->lo ? a->lo : b->lo, a->hi > b->hi ? a->hi : b->hi);
        }

        Node* mixValues(Node* a, Node* b, Node* t) {
            if (t->op == Op::Const && t->value == 0.) {
                return a;
            }

            if (t->op == Op::Const && t->value == 1.) {
                return b;
            }

            return addValues(a, mulValues(subValues(b, a), t));
        }

        Node* unary(Op op, Node* a, double lo, double hi) {
            return make(op, Kind::Float, 1, a, nullptr, nullptr, 0., lo, hi);
        }

        Node* floor(Node* a) {
            if (a->op == Op::Const) {
                return f(::floor(a->value));
            }

            return unary(Op::Floor, a, isfinite(a->lo) ? ::floor(a->lo) : a->lo, isfinite(a->hi) ? ::floor(a->hi) : a->hi);
        }

        Node* exp2(Node* a) {
            if (a->op == Op::Const) {
                return f(::exp2(a->value));
            }

            return unary(Op::Exp2, a, ::exp2(a->lo), ::exp2(a->hi));
        }

        Node* exp(Node* a) {
            if (a->op == Op::Const) {
                return f(::exp(a->value));
            }

            return unary(Op::Exp, a, ::exp(a->lo), ::exp(a->hi));
        }

        Node* log2(Node* a) {
            if (a->op == Op::Const) {
                return f(::log2(a->value));
            }

            return unary(Op::Log2, a, a->lo > 0. ? ::log2(a->lo) : -INFINITY, a->hi > 0. ? ::log2(a->hi) : -INFINITY);
        }

        Node* log(Node* a) {
            if (a->op == Op::Const) {
                return f(::log(a->value));
            }

            return unary(Op::Log, a, a->lo > 0. ? ::log(a->lo) : -INFINITY, a->hi > 0. ? ::log(a->hi) : -INFINITY);
        }

        Node* sqrt(Node* a) {
            if (a->op == Op::Const) {
                return f(::sqrt(a->value));
            }

            return unary(Op::Sqrt, a, a->lo > 0. ? ::sqrt(a->lo) : 0., a->hi > 0. ? ::sqrt(a->hi) : 0.);
        }

        Node* powValues(Node* x, Node* p) {
            if (p->op == Op::Const && p->value == 1. && x->lo >= 0.) {
                return x;
            }

            if (x->op == Op::Const && p->op == Op::Const) {
                return f(::pow(x->value, p->value));
            }

            return exp2(mulValues(p, log2(x)));
        }

        Node* convert(Node* a, Kind kind) {
            if (a->kind == kind) {
                return a;
            }

            if (a->op == Op::Const) {
                return constant(kind == Kind::Float ? a->value : ::floor(a->value), kind);
            }

            double lo = a->lo;
            double hi = a->hi;

            if (kind != Kind::Float) {
                lo = isfinite(lo) ? ::floor(lo) : -2147483648.;
                hi = isfinite(hi) ? ::floor(hi) : 2147483647.;

                if (kind == Kind::Uint && lo < 0.) {
                    lo = 0.;
                    hi = word;
                }
            }

            return make(Op::Convert, kind, 1, a, nullptr, nullptr, 0., lo, hi);
        }

        Node* load(Node* index) {
            return make(Op::Load, Kind::Uint, 1, index, nullptr, nullptr, 0., 0., word);
        }

        Node* half(Node* a) {
            return make(Op::Half, Kind::Float, 1, a, nullptr, nullptr, 0., -INFINITY, INFINITY);
        }

        Node* bitsFloat(Node* a) {
            return make(Op::BitsFloat, Kind::Float, 1, a, nullptr, nullptr, 0., -INFINITY, INFINITY);
        }
    };

    static void inverted(const double (&m)[3][3], double (&out)[3][3]) {
        double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);

        out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
        out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
        out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
        out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
        out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
        out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
        out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
        out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
        out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    }

    static void ictcp(const double (&rows)[3][3], double (&out)[3][3]) {
        double scaled[3][3];

        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                scaled[r][c] = rows[r][c] / 4096.;
            }
        }

        inverted(scaled, out);
    }

    constexpr double toLms[3][3] = {{1688, 2146, 262}, {683, 2951, 462}, {99, 309, 3688}};
    constexpr double pqToIctcp[3][3] = {{2048, 2048, 0}, {6610, -13613, 7003}, {17933, -17390, -543}};
    constexpr double hlgToIctcp[3][3] = {{2048, 2048, 0}, {3625, -7465, 3840}, {9500, -9212, -288}};

    struct Video {
        Graph& g;
        const VideoShader& s;
        const VideoLayout& l;

        bool model(const char* name) const {
            return !strcmp(l.model, name);
        }

        Node* swap16(Node* v) {
            return g.bor(g.shl(g.band(v, 0xff), 8), g.band(g.shr(v, 8), 0xff));
        }

        Node* swap32(Node* v) {
            return g.bor(g.bor(g.shl(g.band(v, 0xff), 24), g.shl(g.band(v, 0xff00), 8)), g.bor(g.band(g.shr(v, 8), 0xff00), g.shr(v, 24)));
        }

        int start(int c) const {
            const int* k = l.components[c];

            return l.bigEndian && k[3] + k[4] <= 8 ? k[2] + 1 : k[2];
        }

        Node* value(int c, Node* window) {
            const int* k = l.components[c];
            int shift = k[3];
            int depth = k[4];
            double mask = depth < 32 ? ::exp2(depth) - 1. : word;
            Node* bits;

            if (l.floating) {
                return depth == 16 ? g.half(l.bigEndian ? swap16(window) : window) : g.bitsFloat(l.bigEndian ? swap32(window) : window);
            }

            if (l.bits && depth == 10) {
                bits = g.band(g.shr(swap32(window), k[2]), mask);
            } else if (l.bits) {
                bits = window;
            } else if (l.bigEndian && shift + depth > 16) {
                bits = g.band(g.shr(swap32(window), shift), mask);
            } else if (l.bigEndian && shift + depth > 8) {
                bits = g.band(g.shr(swap16(window), shift), mask);
            } else {
                bits = g.band(g.shr(window, shift), mask);
            }

            return g.convert(bits, Kind::Float);
        }

        Node* window(int c, Node* row, Node* column) {
            const int* k = l.components[c];
            int step = k[1];
            int depth = k[4];
            int first = start(c);

            if (l.bits && depth == 10) {
                return g.load(g.add(row, column));
            }

            if (l.bits) {
                Node* bit = g.add(g.mul(column, step), k[2]);
                Node* byte = g.shr(bit, 3);
                Node* place = g.sub(g.add(g.shl(g.band(byte, 3), 3), 8 - depth), g.band(bit, 7));

                return g.band(g.shr(g.load(g.add(row, g.shr(byte, 2))), place), ::exp2(depth) - 1.);
            }

            if (c == 0 && l.luma[0]) {
                Node* lane = g.band(column, 3);
                Node* within = g.u(l.luma[4]);

                for (int i = 2; i >= 0; i--) {
                    within = g.select(g.eq(lane, i), g.u(l.luma[1 + i]), within);
                }

                Node* byte = g.add(g.mul(g.shr(column, 2), l.luma[0]), within);

                return g.shr(g.load(g.add(row, g.shr(byte, 2))), g.shl(g.band(byte, 3), 3));
            }

            if (step == 1 || step == 2) {
                Node* index = g.shr(column, step == 1 ? 2 : 1);
                Node* place = g.mul(g.band(column, 4 / step - 1), 8 * step);

                return g.shr(g.shr(g.load(g.add(row, index)), place), 8 * first);
            }

            if (step % 4 == 0) {
                return g.shr(g.load(g.add(g.add(row, g.mul(column, step / 4)), first / 4)), 8 * (first % 4));
            }

            Node* byte = g.mul(column, step);
            Node* place = g.shl(g.band(byte, 3), 3);
            Node* last = g.load(g.add(row, g.shr(g.add(byte, step - 1), 2)));

            if (first >= 4) {
                return g.shr(g.shr(last, place), 8 * (first - 4));
            }

            Node* low = g.bor(g.shr(g.load(g.add(row, g.shr(byte, 2))), place), g.shl(g.shl(last, g.sub(24, place)), 8));

            return g.shr(low, 8 * first);
        }

        void footprint(Node* const (&at)[2], Node* const (&extent)[2], Node* (&f)[2], Node* (&a)[2], Node* (&b)[2]) {
            for (int i = 0; i < 2; i++) {
                Node* base = g.floor(at[i]);
                Node* whole = g.convert(base, Kind::Int);

                f[i] = g.sub(at[i], base);
                a[i] = g.convert(g.max(whole, 0), Kind::Uint);
                b[i] = g.convert(g.min(g.add(whole, 1), g.sub(g.convert(extent[i], Kind::Int), 1)), Kind::Uint);
            }
        }

        void grid(Node* const (&at)[2], Node* const (&extent)[2], const int* members, int count, Node* (&slots)[4]) {
            Node* f[2];
            Node* a[2];
            Node* b[2];

            footprint(at, extent, f, a, b);

            for (int m = 0; m < count; m++) {
                int c = members[m];
                int plane = l.components[c][0];
                Node* offset = g.u(s.planeOffset[plane]);
                Node* line = g.u(s.lineSize[plane]);
                Node* rows[2] = {g.shr(g.add(offset, g.mul(a[1], line)), 2), g.shr(g.add(offset, g.mul(b[1], line)), 2)};
                Node* columns[2] = {a[0], b[0]};
                Node* taps[2][2];

                for (int r = 0; r < 2; r++) {
                    for (int k = 0; k < 2; k++) {
                        taps[r][k] = value(c, window(c, rows[r], columns[k]));
                    }
                }

                Node* left = g.mix(taps[0][0], taps[1][0], f[1]);
                Node* right = g.mix(taps[0][1], taps[1][1], f[1]);

                slots[l.alpha && c == l.count - 1 ? 3 : c] = g.mix(left, right, f[0]);
            }
        }

        Node* mirror(Node* v, Node* end) {
            return g.convert(g.sub(end, g.abs(g.sub(end, g.abs(v)))), Kind::Uint);
        }

        void mosaic(Node* y, Node* const (&xs)[4], Node* (&out)[4]) {
            int step = l.components[0][1];
            Node* lastX = g.sub(g.convert(g.u(s.size[0]), Kind::Int), 1);
            Node* lastY = g.sub(g.convert(g.u(s.size[1]), Kind::Int), 1);
            Node* row = g.shr(g.add(g.u(s.planeOffset[0]), g.mul(mirror(y, lastY), g.u(s.lineSize[0]))), 2);

            for (int i = 0; i < 4; i++) {
                Node* inside = mirror(xs[i], lastX);
                Node* texel = g.shr(g.load(g.add(row, g.shr(inside, step == 1 ? 2 : 1))), g.mul(g.band(inside, step == 1 ? 3 : 1), 8 * step));
                Node* bits = step == 1 ? g.band(texel, 0xff) : l.bigEndian ? swap16(texel) : g.band(texel, 0xffff);

                out[i] = g.convert(bits, Kind::Float);
            }
        }

        void demosaic(Node* const (&above)[4], Node* const (&middle)[4], Node* const (&below)[4], Node* right, Node* const (&at)[2], Node* (&out)[3]) {
            Node* u[3];
            Node* m[3];
            Node* d[3];

            for (int i = 0; i < 3; i++) {
                u[i] = g.mix(above[i], above[i + 1], right);
                m[i] = g.mix(middle[i], middle[i + 1], right);
                d[i] = g.mix(below[i], below[i + 1], right);
            }

            Node* horizontal = g.mul(g.add(m[0], m[2]), 0.5);
            Node* vertical = g.mul(g.add(u[1], d[1]), 0.5);
            Node* cross = g.mul(g.add(horizontal, vertical), 0.5);
            Node* diagonal = g.mul(g.add(g.add(u[0], u[2]), g.add(d[0], d[2])), 0.25);
            Node* same[2];

            for (int i = 0; i < 2; i++) {
                same[i] = g.convert(g.eq(g.band(g.convert(at[i], Kind::Uint), 1), g.u(s.sites[i])), Kind::Float);
            }

            Node* onRed = g.mul(same[0], same[1]);
            Node* onBlue = g.mul(g.sub(1., same[0]), g.sub(1., same[1]));
            Node* onGreen = g.sub(g.sub(1., onRed), onBlue);
            Node* green[3] = {g.mix(vertical, horizontal, same[1]), m[1], g.mix(horizontal, vertical, same[1])};
            Node* redSite[3] = {m[1], cross, diagonal};
            Node* blueSite[3] = {diagonal, cross, m[1]};

            for (int i = 0; i < 3; i++) {
                out[i] = g.add(g.add(g.mul(onRed, redSite[i]), g.mul(onBlue, blueSite[i])), g.mul(onGreen, green[i]));
            }
        }

        void bayer(Node* const (&at)[2], Node* (&codes)[4]) {
            Node* f[2];
            Node* a[2];
            Node* b[2];

            for (int i = 0; i < 2; i++) {
                Node* base = g.floor(at[i]);
                Node* whole = g.convert(base, Kind::Int);

                f[i] = g.sub(at[i], base);
                a[i] = g.max(whole, 0);
                b[i] = g.min(g.add(whole, 1), g.sub(g.convert(g.u(s.size[i]), Kind::Int), 1));
            }

            Node* xs[4] = {g.add(a[0], -1), a[0], g.add(a[0], 1), g.add(a[0], 2)};
            Node* rows[4][4];

            for (int r = 0; r < 4; r++) {
                mosaic(g.add(a[1], r - 1), xs, rows[r]);
            }

            Node* right = g.convert(g.lt(a[0], b[0]), Kind::Float);
            Node* down = g.convert(g.lt(a[1], b[1]), Kind::Float);
            Node* lower[3][4];

            for (int r = 0; r < 3; r++) {
                for (int k = 0; k < 4; k++) {
                    lower[r][k] = g.mix(rows[r][k], rows[r + 1][k], down);
                }
            }

            Node* still = g.f(0.);
            Node* topLeftAt[2] = {a[0], a[1]};
            Node* topRightAt[2] = {b[0], a[1]};
            Node* bottomLeftAt[2] = {a[0], b[1]};
            Node* bottomRightAt[2] = {b[0], b[1]};
            Node* topLeft[3];
            Node* topRight[3];
            Node* bottomLeft[3];
            Node* bottomRight[3];

            demosaic(rows[0], rows[1], rows[2], still, topLeftAt, topLeft);
            demosaic(rows[0], rows[1], rows[2], right, topRightAt, topRight);
            demosaic(lower[0], lower[1], lower[2], still, bottomLeftAt, bottomLeft);
            demosaic(lower[0], lower[1], lower[2], right, bottomRightAt, bottomRight);

            for (int i = 0; i < 3; i++) {
                codes[i] = g.mix(g.mix(topLeft[i], topRight[i], f[0]), g.mix(bottomLeft[i], bottomRight[i], f[0]), f[1]);
            }

            codes[3] = g.f(0.);
        }

        void palette(Node* const (&at)[2], Node* (&out)[4]) {
            Node* extent[2] = {g.u(s.size[0]), g.u(s.size[1])};
            Node* f[2];
            Node* a[2];
            Node* b[2];

            footprint(at, extent, f, a, b);

            Node* offset = g.u(s.planeOffset[0]);
            Node* line = g.u(s.lineSize[0]);
            Node* entries[4][4];
            Node* ys[2] = {a[1], b[1]};
            Node* xs[2] = {a[0], b[0]};
            const int bytes[4] = {2, 1, 0, 3};

            for (int r = 0; r < 2; r++) {
                Node* row = g.shr(g.add(offset, g.mul(ys[r], line)), 2);

                for (int k = 0; k < 2; k++) {
                    Node* index = g.band(g.shr(g.load(g.add(row, g.shr(xs[k], 2))), g.shl(g.band(xs[k], 3), 3)), 0xff);
                    Node* entry = g.load(g.add(g.u(s.planeOffset[1] / 4), index));

                    for (int c = 0; c < 4; c++) {
                        entries[r * 2 + k][c] = g.mul(g.convert(g.band(g.shr(entry, 8 * bytes[c]), 0xff), Kind::Float), 1. / 255.);
                    }
                }
            }

            for (int c = 0; c < 4; c++) {
                out[c] = g.mix(g.mix(entries[0][c], entries[2][c], f[1]), g.mix(entries[1][c], entries[3][c], f[1]), f[0]);
            }
        }

        void codes(Node* (&out)[4]) {
            Node* at[2];

            for (int i = 0; i < 2; i++) {
                at[i] = g.sub(g.mul(g.input(i), (double)s.size[i]), 0.5);
            }

            if (model("palette")) {
                palette(at, out);
                return;
            }

            if (model("bayer")) {
                bayer(at, out);
                return;
            }

            for (int i = 0; i < 4; i++) {
                out[i] = g.f(0.);
            }

            Node* extent[2] = {g.u(s.size[0]), g.u(s.size[1])};

            if (!model("yuv")) {
                const int members[4] = {0, 1, 2, 3};

                grid(at, extent, members, l.count, out);
                return;
            }

            const int luma[2] = {0, 3};
            const int chroma[2] = {1, 2};
            Node* chromaAt[2] = {g.sub(g.mul(at[0], s.chroma[0]), s.chroma[2]), g.sub(g.mul(at[1], s.chroma[1]), s.chroma[3])};
            Node* chromaExtent[2] = {g.u(s.size[2]), g.u(s.size[3])};
            Node* sampled[4] = {g.f(0.), g.f(0.), g.f(0.), g.f(0.)};

            grid(at, extent, luma, l.alpha ? 2 : 1, out);
            grid(chromaAt, chromaExtent, chroma, 2, sampled);

            for (int i = 0; i < 4; i++) {
                out[i] = g.add(out[i], sampled[i]);
            }
        }

        Node* segment(Node* v, const double* k) {
            return g.sub(g.mul(k[0], g.pow(g.add(g.mul(v, k[2]), k[3]), k[1])), k[4]);
        }

        Node* piece(Node* v, const double (&table)[11]) {
            if (table[10] < 0.) {
                return segment(v, table);
            }

            return g.select(g.le(v, table[10]), segment(v, table + 5), segment(v, table));
        }

        Node* pqLight(Node* signal) {
            Node* p = g.pow(g.clamp(signal, 0., 1.), 32. / 2523.);

            return g.pow(g.div(g.max(g.sub(p, 0.8359375), 0.), g.sub(18.8515625, g.mul(18.6875, p))), 16384. / 2610.);
        }

        Node* hlgScene(Node* signal) {
            Node* v = g.clamp(signal, 0., 1.);

            return g.select(g.le(v, 0.5), g.mul(g.mul(v, v), 1. / 3.), g.add(g.mul(g.exp(g.sub(g.mul(v, 5.591816310), 3.130917952)), 1. / 12.), 0.28466892 / 12.));
        }

        void hlgDisplay(Node* (&scene)[3]) {
            Node* luminance = g.f(0.);

            for (int i = 0; i < 3; i++) {
                luminance = g.add(luminance, g.mul(scene[i], s.luminance[i]));
            }

            Node* factor = g.mul(g.pow(luminance, 0.2), s.light[2]);

            for (int i = 0; i < 3; i++) {
                scene[i] = g.mul(scene[i], factor);
            }
        }

        Node* logLight(Node* signal) {
            return g.select(g.le(signal, 0.), 0., g.exp2(g.mul(g.sub(signal, 1.), 3.32192809489 * s.light[0])));
        }

        bool curved() const {
            return !strcmp(s.transfer, "curve") || !strcmp(s.transfer, "identity");
        }

        Node* encode(Node* light) {
            if (curved()) {
                return piece(g.max(light, 0.), s.oetf);
            }

            if (!strcmp(s.transfer, "log")) {
                Node* v = g.max(light, 1e-30);

                return g.select(g.lt(v, ::exp2(-3.32192809489 * s.light[0])), 0., g.add(g.mul(g.log2(v), 0.30102999566 / s.light[0]), 1.));
            }

            if (!strcmp(s.transfer, "pq")) {
                Node* y = g.pow(g.clamp(light, 0., 1.), 2610. / 16384.);

                return g.pow(g.div(g.add(g.mul(y, 18.8515625), 0.8359375), g.add(g.mul(y, 18.6875), 1.)), 2523. / 32.);
            }

            Node* v = g.max(light, 0.);

            return g.select(g.le(v, 1. / 12.), g.sqrt(g.mul(v, 3.)), g.add(g.mul(g.log(g.max(g.sub(g.mul(v, 12.), 0.28466892), 1e-6)), 0.17883277), 0.55991073));
        }

        Node* decodeLight(Node* signal) {
            if (curved()) {
                return piece(g.max(signal, 0.), s.inverse);
            }

            if (!strcmp(s.transfer, "log")) {
                return logLight(signal);
            }

            if (!strcmp(s.transfer, "pq")) {
                return pqLight(signal);
            }

            return hlgScene(signal);
        }

        void constantLuminance(Node* (&signal)[3]) {
            double kr = s.weights[0];
            double kb = s.weights[1];
            Node* negative[2] = {encode(g.f(1. - kb)), encode(g.f(1. - kr))};
            Node* positive[2] = {g.sub(1., encode(g.f(kb))), g.sub(1., encode(g.f(kr)))};
            Node* chroma[2];

            for (int i = 0; i < 2; i++) {
                chroma[i] = g.mul(g.mul(signal[1 + i], 2.), g.select(g.le(signal[1 + i], 0.), negative[i], positive[i]));
            }

            Node* yrb[3] = {signal[0], g.add(signal[0], chroma[1]), g.add(signal[0], chroma[0])};
            Node* light[3] = {decodeLight(yrb[0]), decodeLight(yrb[1]), decodeLight(yrb[2])};
            Node* green = g.mul(g.sub(g.sub(light[0], g.mul(light[1], kr)), g.mul(light[2], kb)), 1. / (1. - kr - kb));

            signal[0] = yrb[1];
            signal[1] = encode(green);
            signal[2] = yrb[2];
        }

        void rows(const double (&matrix)[3][3], Node* const (&in)[3], Node* (&out)[3]) {
            for (int r = 0; r < 3; r++) {
                out[r] = g.f(0.);

                for (int c = 0; c < 3; c++) {
                    out[r] = g.add(out[r], g.mul(in[c], matrix[r][c]));
                }
            }
        }

        void color(Node* const (&codes)[4], Node* (&out)[4]) {
            Node* signal[3];
            bool sdr = !strcmp(s.output, "sdr");

            if (model("palette")) {
                for (int i = 0; i < 3; i++) {
                    signal[i] = codes[i];
                }

                out[3] = codes[3];
            } else {
                for (int r = 0; r < 3; r++) {
                    signal[r] = g.f(s.bias[r]);

                    for (int c = 0; c < 3; c++) {
                        signal[r] = g.add(signal[r], g.mul(codes[c], s.decode[r][c]));
                    }
                }

                if (!strcmp(s.system, "cl")) {
                    constantLuminance(signal);
                }

                out[3] = l.alpha ? g.mul(codes[3], s.bias[3]) : g.f(1.);
            }

            if (!strcmp(s.transfer, "identity")) {
                for (int i = 0; i < 3; i++) {
                    out[i] = sdr ? g.clamp(signal[i], 0., 1.) : g.max(signal[i], 0.);
                }

                return;
            }

            if (!strcmp(s.transfer, "curve") && !strcmp(s.conversion, "same")) {
                for (int i = 0; i < 3; i++) {
                    out[i] = piece(sdr ? g.clamp(signal[i], 0., 1.) : g.max(signal[i], 0.), s.curve);
                }

                return;
            }

            Node* light[3];
            double toLight[3][3];
            double toSignal[3][3];

            if (!strcmp(s.system, "ictcp")) {
                bool pq = !strcmp(s.transfer, "pq");
                Node* lms[3];
                Node* scene[3];

                ictcp(pq ? pqToIctcp : hlgToIctcp, toSignal);
                ictcp(toLms, toLight);
                rows(toSignal, signal, lms);

                for (int i = 0; i < 3; i++) {
                    lms[i] = pq ? pqLight(lms[i]) : hlgScene(lms[i]);
                }

                rows(toLight, lms, scene);

                for (int i = 0; i < 3; i++) {
                    light[i] = pq ? g.mul(scene[i], s.light[1]) : scene[i];
                }

                if (!pq) {
                    hlgDisplay(light);
                }
            } else if (!strcmp(s.transfer, "curve")) {
                for (int i = 0; i < 3; i++) {
                    light[i] = piece(g.max(signal[i], 0.), s.curve);
                }
            } else if (!strcmp(s.transfer, "log")) {
                for (int i = 0; i < 3; i++) {
                    light[i] = logLight(signal[i]);
                }
            } else if (!strcmp(s.transfer, "pq")) {
                for (int i = 0; i < 3; i++) {
                    light[i] = g.mul(pqLight(signal[i]), s.light[1]);
                }
            } else {
                for (int i = 0; i < 3; i++) {
                    light[i] = hlgScene(signal[i]);
                }

                hlgDisplay(light);
            }

            if (!strcmp(s.conversion, "convert")) {
                Node* converted[3];

                rows(s.toOutput, light, converted);

                for (int i = 0; i < 3; i++) {
                    light[i] = converted[i];
                }
            }

            for (int i = 0; i < 3; i++) {
                if (!sdr) {
                    out[i] = light[i];
                    continue;
                }

                Node* display = g.clamp(light[i], 0., 1.);

                out[i] = g.select(g.le(display, 0.0031308), g.mul(display, 12.92), g.sub(g.mul(g.pow(display, 1. / 2.4), 1.055), 0.055));
            }
        }
    };

    static void order(Node* const (&roots)[4], Vector<Node*>& out) {
        Vector<Node*> stack;

        for (Node* root : roots) {
            stack.pushBack(root);

            while (!stack.empty()) {
                Node* top = stack.back();

                if (top->id == 2) {
                    stack.popBack();
                    continue;
                }

                bool ready = true;

                top->id = 1;

                for (int i = top->arity - 1; i >= 0; i--) {
                    if (!top->args[i]->id) {
                        stack.pushBack(top->args[i]);
                        ready = false;
                    }
                }

                if (ready) {
                    stack.popBack();
                    top->id = 2;
                    out.pushBack(top);
                }
            }
        }
    }
}

#if defined(__APPLE__)
namespace {
    static const char* typeName(Kind kind) {
        return kind == Kind::Float ? "float" : kind == Kind::Uint ? "uint" : kind == Kind::Int ? "int" : "bool";
    }

    static void operand(StringBuilder& out, const Node* node) {
        if (node->op == Op::Input) {
            out << StringView(node->value == 0. ? "in.uv.x" : "in.uv.y");
        } else if (node->op != Op::Const) {
            out << StringView(u8"t") << (u64)node->id;
        } else if (node->kind == Kind::Bool) {
            out << StringView(node->value != 0. ? "true" : "false");
        } else if (node->kind == Kind::Uint) {
            out << (u64)node->value << StringView(u8"u");
        } else if (node->kind == Kind::Int) {
            out << StringView(u8"int(") << (i64)node->value << StringView(u8")");
        } else {
            float value = (float)node->value;
            u32 bits;

            memcpy(&bits, &value, sizeof(bits));
            out << StringView(u8"as_type<float>(") << (u64)bits << StringView(u8"u)");
        }
    }

    static StringView msl(ObjPool& pool, Node* const (&color)[4]) {
        Vector<Node*> nodes;
        StringBuilder out;
        u32 next = 3;

        order(color, nodes);
        out << StringView(u8"#include <metal_stdlib>\nusing namespace metal;\nstruct In {\n    float2 uv [[user(locn0)]];\n};\nfragment float4 main0(In in [[stage_in]], const device uint* words [[buffer(0)]]) {\n");

        for (Node* node : nodes) {
            if (node->op == Op::Const || node->op == Op::Input) {
                continue;
            }

            node->id = next++;

            const char* infix = nullptr;
            const char* call = nullptr;

            switch (node->op) {
                case Op::Add:
                    infix = " + ";
                    break;
                case Op::Sub:
                    infix = " - ";
                    break;
                case Op::Mul:
                    infix = " * ";
                    break;
                case Op::Div:
                    infix = " / ";
                    break;
                case Op::Shr:
                    infix = " >> ";
                    break;
                case Op::Shl:
                    infix = " << ";
                    break;
                case Op::And:
                    infix = " & ";
                    break;
                case Op::Or:
                    infix = " | ";
                    break;
                case Op::Le:
                    infix = " <= ";
                    break;
                case Op::Lt:
                    infix = " < ";
                    break;
                case Op::Eq:
                    infix = " == ";
                    break;
                case Op::Abs:
                    call = "abs";
                    break;
                case Op::Min:
                    call = "min";
                    break;
                case Op::Max:
                    call = "max";
                    break;
                case Op::Floor:
                    call = "floor";
                    break;
                case Op::Exp2:
                    call = "exp2";
                    break;
                case Op::Exp:
                    call = "exp";
                    break;
                case Op::Log2:
                    call = "log2";
                    break;
                case Op::Log:
                    call = "log";
                    break;
                case Op::Sqrt:
                    call = "sqrt";
                    break;
                default:
                    break;
            }

            out << StringView(u8"    ") << StringView(typeName(node->kind)) << StringView(u8" ");
            operand(out, node);
            out << StringView(u8" = ");

            if (infix) {
                operand(out, node->args[0]);
                out << StringView(infix);
                operand(out, node->args[1]);
            } else if (call) {
                out << StringView(call) << StringView(u8"(");
                operand(out, node->args[0]);

                if (node->arity == 2) {
                    out << StringView(u8", ");
                    operand(out, node->args[1]);
                }

                out << StringView(u8")");
            } else if (node->op == Op::Neg) {
                out << StringView(u8"-");
                operand(out, node->args[0]);
            } else if (node->op == Op::Select) {
                out << StringView(u8"select(");
                operand(out, node->args[2]);
                out << StringView(u8", ");
                operand(out, node->args[1]);
                out << StringView(u8", ");
                operand(out, node->args[0]);
                out << StringView(u8")");
            } else if (node->op == Op::Convert) {
                out << StringView(typeName(node->kind)) << StringView(u8"(");
                operand(out, node->args[0]);
                out << StringView(u8")");
            } else if (node->op == Op::Load) {
                out << StringView(u8"words[");
                operand(out, node->args[0]);
                out << StringView(u8"]");
            } else if (node->op == Op::Half) {
                out << StringView(u8"float(as_type<half2>(");
                operand(out, node->args[0]);
                out << StringView(u8").x)");
            } else if (node->op == Op::BitsFloat) {
                out << StringView(u8"as_type<float>(");
                operand(out, node->args[0]);
                out << StringView(u8")");
            } else {
                fail(StringView(u8"a video shader node has no Metal form"));
            }

            out << StringView(u8";\n");
        }

        out << StringView(u8"    return float4(");

        for (int i = 0; i < 4; i++) {
            operand(out, color[i]);
            out << StringView(i < 3 ? ", " : ");\n}\n");
        }

        u8* bytes = (u8*)pool.allocate(out.used());

        memcpy(bytes, out.data(), out.used());

        return StringView(bytes, out.used());
    }
}
#else
namespace {
    enum: u32 {
        GlslFAbs = 4,
        GlslSAbs = 5,
        GlslFloor = 8,
        GlslExp = 27,
        GlslLog = 28,
        GlslExp2 = 29,
        GlslLog2 = 30,
        GlslSqrt = 31,
        GlslFMin = 37,
        GlslUMin = 38,
        GlslSMin = 39,
        GlslFMax = 40,
        GlslUMax = 41,
        GlslSMax = 42,
        GlslUnpackHalf2x16 = 62,
    };

    struct Spirv {
        Vector<u32> head;
        Vector<u32> globals;
        Vector<u32> body;
        u32 next = 1;

        u32 fresh() {
            return next++;
        }

        static void put(Vector<u32>& out, u32 opcode, const u32* operands, u32 count) {
            out.pushBack((count + 1) << 16 | opcode);
            out.append(operands, count);
        }

        template <class... A>
        static void op(Vector<u32>& out, u32 opcode, A... a) {
            u32 operands[sizeof...(A) + 1] = {(u32)a...};

            put(out, opcode, operands, sizeof...(A));
        }

        static void string(Vector<u32>& out, const char* s) {
            size_t length = strlen(s) + 1;
            u32 words = (u32)((length + 3) / 4);
            u32 packed[8] = {};

            memcpy(packed, s, length);
            out.append(packed, words);
        }
    };

    static StringView spirv(ObjPool& pool, Graph& g, Node* const (&color)[4]) {
        Node* zero = g.i(0.);
        Node* one = g.f(1.);
        Node* none = g.f(0.);
        Vector<Node*> nodes;
        Spirv s;

        order(color, nodes);

        Node* extras[3] = {zero, one, none};

        for (Node* extra : extras) {
            if (!extra->id) {
                extra->id = 2;
                nodes.pushBack(extra);
            }
        }

        u32 glsl = s.fresh();
        u32 typeVoid = s.fresh();
        u32 typeFunction = s.fresh();
        u32 typeBool = s.fresh();
        u32 typeFloat = s.fresh();
        u32 typeUint = s.fresh();
        u32 typeInt = s.fresh();
        u32 typeVec2 = s.fresh();
        u32 typeVec4 = s.fresh();
        u32 inputVec2 = s.fresh();
        u32 outputVec4 = s.fresh();
        u32 typeWords = s.fresh();
        u32 typeBytes = s.fresh();
        u32 storageBytes = s.fresh();
        u32 storageWord = s.fresh();
        u32 uv = s.fresh();
        u32 fColor = s.fresh();
        u32 bytes = s.fresh();
        u32 main = s.fresh();
        u32 label = s.fresh();
        u32 coordinates = s.fresh();
        u32 coordinate[2] = {s.fresh(), s.fresh()};

        auto type = [&](Kind kind) {
            return kind == Kind::Float ? typeFloat : kind == Kind::Uint ? typeUint : kind == Kind::Int ? typeInt : typeBool;
        };

        for (Node* node : nodes) {
            node->id = node->op == Op::Input ? coordinate[(int)node->value] : s.fresh();
        }

        for (Node* node : nodes) {
            if (node->op != Op::Const) {
                continue;
            }

            if (node->kind == Kind::Bool) {
                Spirv::op(s.globals, node->value != 0. ? 41 : 42, typeBool, node->id);
            } else if (node->kind == Kind::Float) {
                float value = (float)node->value;
                u32 bits;

                memcpy(&bits, &value, sizeof(bits));
                Spirv::op(s.globals, 43, typeFloat, node->id, bits);
            } else {
                Spirv::op(s.globals, 43, type(node->kind), node->id, node->kind == Kind::Uint ? (u32)node->value : (u32)(i32)node->value);
            }
        }

        Spirv::op(s.body, 54, typeVoid, main, 0, typeFunction);
        Spirv::op(s.body, 248, label);
        Spirv::op(s.body, 61, typeVec2, coordinates, uv);
        Spirv::op(s.body, 81, typeFloat, coordinate[0], coordinates, 0);
        Spirv::op(s.body, 81, typeFloat, coordinate[1], coordinates, 1);

        for (Node* node : nodes) {
            if (node->op == Op::Const || node->op == Op::Input) {
                continue;
            }

            bool real = node->kind == Kind::Float;
            bool sign = node->kind == Kind::Int;
            u32 result = type(node->kind);
            u32 a = node->arity > 0 ? node->args[0]->id : 0;
            u32 b = node->arity > 1 ? node->args[1]->id : 0;
            Kind from = node->arity > 0 ? node->args[0]->kind : Kind::Float;

            switch (node->op) {
                case Op::Add:
                    Spirv::op(s.body, real ? 129 : 128, result, node->id, a, b);
                    break;
                case Op::Sub:
                    Spirv::op(s.body, real ? 131 : 130, result, node->id, a, b);
                    break;
                case Op::Neg:
                    Spirv::op(s.body, real ? 127 : 126, result, node->id, a);
                    break;
                case Op::Mul:
                    Spirv::op(s.body, real ? 133 : 132, result, node->id, a, b);
                    break;
                case Op::Div:
                    Spirv::op(s.body, 136, result, node->id, a, b);
                    break;
                case Op::Abs:
                    Spirv::op(s.body, 12, result, node->id, glsl, real ? GlslFAbs : GlslSAbs, a);
                    break;
                case Op::Min:
                    Spirv::op(s.body, 12, result, node->id, glsl, real ? GlslFMin : sign ? GlslSMin : GlslUMin, a, b);
                    break;
                case Op::Max:
                    Spirv::op(s.body, 12, result, node->id, glsl, real ? GlslFMax : sign ? GlslSMax : GlslUMax, a, b);
                    break;
                case Op::Shr:
                    Spirv::op(s.body, 194, result, node->id, a, b);
                    break;
                case Op::Shl:
                    Spirv::op(s.body, 196, result, node->id, a, b);
                    break;
                case Op::And:
                    Spirv::op(s.body, 199, result, node->id, a, b);
                    break;
                case Op::Or:
                    Spirv::op(s.body, 197, result, node->id, a, b);
                    break;
                case Op::Le:
                    Spirv::op(s.body, from == Kind::Float ? 188 : from == Kind::Int ? 179 : 178, typeBool, node->id, a, b);
                    break;
                case Op::Lt:
                    Spirv::op(s.body, from == Kind::Float ? 184 : from == Kind::Int ? 177 : 176, typeBool, node->id, a, b);
                    break;
                case Op::Eq:
                    Spirv::op(s.body, from == Kind::Float ? 180 : 170, typeBool, node->id, a, b);
                    break;
                case Op::Select:
                    Spirv::op(s.body, 169, result, node->id, a, b, node->args[2]->id);
                    break;
                case Op::Floor:
                    Spirv::op(s.body, 12, result, node->id, glsl, GlslFloor, a);
                    break;
                case Op::Exp2:
                    Spirv::op(s.body, 12, result, node->id, glsl, GlslExp2, a);
                    break;
                case Op::Exp:
                    Spirv::op(s.body, 12, result, node->id, glsl, GlslExp, a);
                    break;
                case Op::Log2:
                    Spirv::op(s.body, 12, result, node->id, glsl, GlslLog2, a);
                    break;
                case Op::Log:
                    Spirv::op(s.body, 12, result, node->id, glsl, GlslLog, a);
                    break;
                case Op::Sqrt:
                    Spirv::op(s.body, 12, result, node->id, glsl, GlslSqrt, a);
                    break;
                case Op::Convert:
                    if (from == Kind::Bool && node->kind == Kind::Float) {
                        Spirv::op(s.body, 169, result, node->id, a, one->id, none->id);
                    } else if (from == Kind::Float) {
                        Spirv::op(s.body, sign ? 110 : 109, result, node->id, a);
                    } else if (node->kind == Kind::Float) {
                        Spirv::op(s.body, from == Kind::Int ? 111 : 112, result, node->id, a);
                    } else if (from != Kind::Bool) {
                        Spirv::op(s.body, 124, result, node->id, a);
                    } else {
                        fail(StringView(u8"a video shader converts a condition to an integer"));
                    }
                    break;
                case Op::Load: {
                    u32 pointer = s.fresh();

                    Spirv::op(s.body, 65, storageWord, pointer, bytes, zero->id, a);
                    Spirv::op(s.body, 61, typeUint, node->id, pointer);
                    break;
                }
                case Op::Half: {
                    u32 pair = s.fresh();

                    Spirv::op(s.body, 12, typeVec2, pair, glsl, GlslUnpackHalf2x16, a);
                    Spirv::op(s.body, 81, typeFloat, node->id, pair, 0);
                    break;
                }
                case Op::BitsFloat:
                    Spirv::op(s.body, 124, typeFloat, node->id, a);
                    break;
                default:
                    fail(StringView(u8"a video shader node has no SPIR-V form"));
            }
        }

        u32 pixel = s.fresh();

        Spirv::op(s.body, 80, typeVec4, pixel, color[0]->id, color[1]->id, color[2]->id, color[3]->id);
        Spirv::op(s.body, 62, fColor, pixel);
        Spirv::op(s.body, 253);
        Spirv::op(s.body, 56);

        Vector<u32> module;
        Vector<u32> entry;

        module.pushBack(0x07230203u);
        module.pushBack(0x00010300u);
        module.pushBack(0);
        module.pushBack(s.next);
        module.pushBack(0);
        Spirv::op(module, 17, 1);
        entry.pushBack(glsl);
        Spirv::string(entry, "GLSL.std.450");
        Spirv::put(module, 11, entry.data(), (u32)entry.length());
        Spirv::op(module, 14, 0, 1);
        entry.clear();
        entry.pushBack(4);
        entry.pushBack(main);
        Spirv::string(entry, "main");
        entry.pushBack(uv);
        entry.pushBack(fColor);
        Spirv::put(module, 15, entry.data(), (u32)entry.length());
        Spirv::op(module, 16, main, 7);
        Spirv::op(module, 71, uv, 30, 0);
        Spirv::op(module, 71, fColor, 30, 0);
        Spirv::op(module, 71, typeWords, 6, 4);
        Spirv::op(module, 72, typeBytes, 0, 24);
        Spirv::op(module, 72, typeBytes, 0, 35, 0);
        Spirv::op(module, 71, typeBytes, 2);
        Spirv::op(module, 71, bytes, 34, 0);
        Spirv::op(module, 71, bytes, 33, 0);
        Spirv::op(module, 19, typeVoid);
        Spirv::op(module, 33, typeFunction, typeVoid);
        Spirv::op(module, 20, typeBool);
        Spirv::op(module, 22, typeFloat, 32);
        Spirv::op(module, 21, typeUint, 32, 0);
        Spirv::op(module, 21, typeInt, 32, 1);
        Spirv::op(module, 23, typeVec2, typeFloat, 2);
        Spirv::op(module, 23, typeVec4, typeFloat, 4);
        Spirv::op(module, 32, inputVec2, 1, typeVec2);
        Spirv::op(module, 32, outputVec4, 3, typeVec4);
        Spirv::op(module, 29, typeWords, typeUint);
        Spirv::op(module, 30, typeBytes, typeWords);
        Spirv::op(module, 32, storageBytes, 12, typeBytes);
        Spirv::op(module, 32, storageWord, 12, typeUint);
        Spirv::op(module, 59, inputVec2, uv, 1);
        Spirv::op(module, 59, outputVec4, fColor, 3);
        Spirv::op(module, 59, storageBytes, bytes, 12);
        module.append(s.globals.data(), s.globals.length());
        module.append(s.body.data(), s.body.length());
        module.mut(3) = s.next;

        size_t size = module.length() * sizeof(u32);
        u8* out = (u8*)pool.allocate(size);

        memcpy(out, module.data(), size);

        return StringView(out, size);
    }
}
#endif

StringView compile(ObjPool& pool, const VideoShader& shader) {
    Graph g(pool);
    Video video{g, shader, *shader.layout};
    Node* codes[4];
    Node* color[4];

    video.codes(codes);
    video.color(codes, color);

#if defined(__APPLE__)
    return msl(pool, color);
#else
    return spirv(pool, g, color);
#endif
}
