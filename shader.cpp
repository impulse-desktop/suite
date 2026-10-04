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
    enum class Op : u8 {
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
        Shared,
    };

    enum class Kind : u8 {
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
        u8 mark;
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

            Node* node = pool.make<Node>(Node{op, kind, (u8)arity, {args[0], args[1], args[2]}, value, lo, hi, count++, 0, 0});

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

        Node* invocation(int which, double hi) {
            return make(Op::Input, Kind::Uint, 0, nullptr, nullptr, nullptr, which, 0., hi);
        }

        Node* origin(int which) {
            return make(Op::Input, Kind::Int, 0, nullptr, nullptr, nullptr, which, 0., 65535.);
        }

        Node* shared(Node* index, int array) {
            return make(Op::Shared, Kind::Float, 1, index, nullptr, nullptr, array, -INFINITY, INFINITY);
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

            if (a->op == Op::Convert && a->args[0]->kind != Kind::Float) {
                return a;
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

    constexpr double pi = 3.14159265358979323846;
    constexpr int lanczosRadius = 3;
    constexpr int lanczosTaps = 2 * lanczosRadius;
    constexpr int lanczosDegree = 5;
    constexpr int maxTaps = 8;

    static void lanczosWeights(double f, double (&out)[lanczosTaps]) {
        double total = 0.;

        for (int k = 0; k < lanczosTaps; k++) {
            double x = pi * (k - (lanczosRadius - 1) - f);

            out[k] = ::fabs(x) < 1e-9 ? 1. : lanczosRadius * ::sin(x) * ::sin(x / lanczosRadius) / (x * x);
            total += out[k];
        }

        for (double& w : out) {
            w /= total;
        }
    }

    struct LanczosFit {
        double c[lanczosTaps][lanczosDegree + 1];

        LanczosFit();
    };

    static const LanczosFit& lanczosFitted() {
        static const LanczosFit fit;

        return fit;
    }

    static void lanczosFit(double (&out)[lanczosTaps][lanczosDegree + 1]) {
        constexpr int n = lanczosDegree + 1;
        double m[n][n + lanczosTaps];

        for (int j = 0; j < n; j++) {
            double t = ::cos(pi * (j + 0.5) / n);
            double w[lanczosTaps];
            double power = 1.;

            lanczosWeights((t + 1.) / 2., w);

            for (int e = 0; e < n; e++) {
                m[j][e] = power;
                power *= t;
            }

            for (int k = 0; k < lanczosTaps; k++) {
                m[j][n + k] = w[k];
            }
        }

        for (int c = 0; c < n; c++) {
            int pivot = c;

            for (int r = c + 1; r < n; r++) {
                pivot = ::fabs(m[r][c]) > ::fabs(m[pivot][c]) ? r : pivot;
            }

            for (int j = 0; j < n + lanczosTaps; j++) {
                double swap = m[c][j];

                m[c][j] = m[pivot][j];
                m[pivot][j] = swap;
            }

            for (int r = 0; r < n; r++) {
                if (r == c) {
                    continue;
                }

                double k = m[r][c] / m[c][c];

                for (int j = c; j < n + lanczosTaps; j++) {
                    m[r][j] -= k * m[c][j];
                }
            }
        }

        for (int k = 0; k < lanczosTaps; k++) {
            for (int e = 0; e < n; e++) {
                out[k][e] = m[e][n + k] / m[e][e];
            }
        }
    }

    LanczosFit::LanczosFit() {
        lanczosFit(c);
    }

    struct Store {
        Node* index;
        Node* value;
        Node* guard;
    };

    constexpr int kernelArrays = 4;
    constexpr int kernelBuffers = 2;

    struct Kernel {
        Vector<Store> stores[kernelArrays];
        u32 sizes[kernelArrays] = {};
        u32 tile = 0;
        Node* at[2];
        Node* color[4];

        u32 buffer(int i) const {
            return sizes[i] > sizes[i + kernelBuffers] ? sizes[i] : sizes[i + kernelBuffers];
        }
    };

    constexpr double sigmoidCenter = 0.75;
    constexpr double sigmoidSlope = 6.5;
    constexpr double sigmoidLow = 0.007577241268;
    constexpr double sigmoidSpan = 0.8279062958;

    struct Video {
        Graph& g;
        const VideoShader& s;
        const VideoLayout& l;
        bool linear = false;

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
            Node* index = g.add(row, g.shr(byte, 2));
            Node* next = g.load(g.add(index, 1));

            if (first >= 4) {
                return g.shr(g.shr(next, place), 8 * (first - 4));
            }

            Node* low = g.bor(g.shr(g.load(index), place), g.shl(g.shl(next, g.sub(24, place)), 8));

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

        struct Axis {
            int taps;
            Node* origin;
            Node* w[maxTaps];
        };

        Axis axis(Node* at, double ratio, bool sharp) {
            Axis a;
            Node* floor = g.floor(at);
            Node* base = g.convert(floor, Kind::Int);
            Node* f = g.sub(at, floor);

            if (ratio >= 1. && sharp) {
                const LanczosFit& fit = lanczosFitted();
                Node* t = g.sub(g.mul(f, 2.), 1.);
                Node* square = g.mul(t, t);

                a.taps = lanczosTaps;

                for (int k = 0; k < lanczosRadius; k++) {
                    Node* even = g.f(0.);
                    Node* odd = g.f(0.);

                    for (int e = lanczosDegree - lanczosDegree % 2; e >= 0; e -= 2) {
                        even = g.add(g.mul(even, square), fit.c[k][e]);
                    }

                    for (int e = lanczosDegree - 1 + lanczosDegree % 2; e >= 1; e -= 2) {
                        odd = g.add(g.mul(odd, square), fit.c[k][e]);
                    }

                    odd = g.mul(odd, t);
                    a.w[k] = g.add(even, odd);
                    a.w[lanczosTaps - 1 - k] = g.sub(even, odd);
                }
            } else if (ratio >= 1.) {
                a.taps = 2;
                a.w[0] = g.sub(1., f);
                a.w[1] = f;
            } else {
                double reach = ::fmin(1. / ratio, maxTaps / 2.);
                int half = (int)::ceil(reach - 1e-9);
                Node* total = g.f(0.);

                a.taps = 2 * half;

                for (int k = 0; k < a.taps; k++) {
                    Node* x = g.min(g.mul(g.abs(g.sub(f, k + 1 - half)), 1. / reach), 1.);

                    a.w[k] = g.add(g.mul(g.mul(g.sub(g.mul(x, 2.), 3.), x), x), 1.);
                    total = g.add(total, a.w[k]);
                }

                Node* inverse = g.div(1., total);

                for (int k = 0; k < a.taps; k++) {
                    a.w[k] = g.mul(a.w[k], inverse);
                }
            }

            a.origin = g.add(base, 1 - a.taps / 2);

            return a;
        }

        Node* funnel(Node* lo, Node* hi, Node* s) {
            return g.select(g.eq(s, 0), lo, g.bor(g.shl(hi, g.sub(32, s)), g.shr(lo, s)));
        }

        int span(int c, int taps) const {
            const int* k = l.components[c];

            return (taps - 1) * k[1] + (k[3] + k[4] <= 8 ? 1 : k[3] + k[4] <= 16 ? 2 : 4);
        }

        bool windowed(int c) const {
            return !l.bits && !(c == 0 && l.luma[0]) && l.components[c][1] <= 8;
        }

        void run(int c, Node* row, Node* left, int taps, Node* (&out)[maxTaps]) {
            int step = l.components[c][1];
            int first = start(c);
            Node* byte = g.add(g.mul(left, step), first);
            Node* index = g.add(row, g.shr(byte, 2));
            bool known = step % 4 == 0;
            int count = ((known ? first % 4 : 3) + span(c, taps) + 3) / 4;
            Node* words[20];

            for (int i = 0; i < count; i++) {
                words[i] = g.load(g.add(index, i));
            }

            if (!known) {
                Node* s = g.shl(g.band(byte, 3), 3);

                for (int i = 0; i + 1 < count; i++) {
                    words[i] = funnel(words[i], words[i + 1], s);
                }

                words[count - 1] = g.shr(words[count - 1], s);
                first = 0;
            }

            for (int m = 0; m < taps; m++) {
                int at = (known ? first % 4 : 0) + m * step;

                out[m] = value(c, g.shr(words[at / 4], 8 * (at % 4)));
            }
        }

        void fold(const Axis& a, Node* left, Node* (&v)[maxTaps]) {
            int taps = a.taps;
            int half = taps / 2;
            Node* d = g.sub(a.origin, g.convert(left, Kind::Int));
            Node* prefix[maxTaps];
            Node* suffix[maxTaps];

            prefix[0] = a.w[0];
            suffix[taps - 1] = a.w[taps - 1];

            for (int k = 1; k < taps; k++) {
                prefix[k] = g.add(prefix[k - 1], a.w[k]);
                suffix[taps - 1 - k] = g.add(suffix[taps - k], a.w[taps - 1 - k]);
            }

            for (int m = 0; m < taps; m++) {
                Node* folded = a.w[m];

                for (int e = -half; e <= half; e++) {
                    int k = m + e;
                    Node* moved = k >= 0 && k < taps ? a.w[k] : g.f(0.);

                    if (e == 0) {
                        continue;
                    }

                    if (e > 0 && m == 0) {
                        moved = prefix[e < taps ? e : taps - 1];
                    }

                    if (e < 0 && m == taps - 1) {
                        moved = suffix[taps - 1 + e > 0 ? taps - 1 + e : 0];
                    }

                    folded = g.select(g.eq(d, -e), moved, folded);
                }

                v[m] = folded;
            }
        }

        void filtered(Node* const (&at)[2], Node* const (&extent)[2], const double (&ratio)[2], bool sharp, const int* members, int count, Node* (&slots)[4]) {
            Axis a[2] = {axis(at[0], ratio[0], sharp), axis(at[1], ratio[1], sharp)};
            int taps = a[0].taps;
            bool whole = extent[0]->value >= taps;

            for (int m = 0; m < count; m++) {
                whole = whole && windowed(members[m]);
            }

            Node* left = g.convert(g.clamp(a[0].origin, 0, extent[0]->value - taps), Kind::Uint);
            Node* v[maxTaps];

            if (whole) {
                fold(a[0], left, v);
            }

            for (int m = 0; m < count; m++) {
                int c = members[m];
                int plane = l.components[c][0];
                Node* offset = g.u(s.planeOffset[plane]);
                Node* line = g.u(s.lineSize[plane]);
                Node* columns[maxTaps];
                Node* total = g.f(0.);

                for (int k = 0; k < taps && !whole; k++) {
                    columns[k] = g.convert(g.clamp(g.add(a[0].origin, k), 0, g.sub(g.convert(extent[0], Kind::Int), 1)), Kind::Uint);
                }

                for (int j = 0; j < a[1].taps; j++) {
                    Node* y = g.convert(g.clamp(g.add(a[1].origin, j), 0, g.sub(g.convert(extent[1], Kind::Int), 1)), Kind::Uint);
                    Node* row = g.shr(g.add(offset, g.mul(y, line)), 2);
                    Node* samples[maxTaps];
                    Node* sum = g.f(0.);

                    if (whole) {
                        run(c, row, left, taps, samples);
                    }

                    for (int k = 0; k < taps; k++) {
                        sum = g.add(sum, g.mul(whole ? v[k] : a[0].w[k], whole ? samples[k] : value(c, window(c, row, columns[k]))));
                    }

                    total = g.add(total, g.mul(a[1].w[j], sum));
                }

                slots[l.alpha && c == l.count - 1 ? 3 : c] = total;
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

            bool sharp = !strcmp(s.filter, "lanczos");

            if (!sharp && strcmp(s.filter, "bilinear")) {
                fail(StringView(u8"a video shader has an unknown filter"));
            }

            double ratio[2] = {(double)s.target[0] / s.size[0], (double)s.target[1] / s.size[1]};
            bool shaped = sharp || ratio[0] < 1. || ratio[1] < 1.;

            if (!model("yuv")) {
                const int members[4] = {0, 1, 2, 3};

                if (shaped) {
                    filtered(at, extent, ratio, sharp, members, l.count, out);
                } else {
                    grid(at, extent, members, l.count, out);
                }

                return;
            }

            const int luma[2] = {0, 3};
            const int chroma[2] = {1, 2};
            Node* chromaAt[2] = {g.sub(g.mul(at[0], s.chroma[0]), s.chroma[2]), g.sub(g.mul(at[1], s.chroma[1]), s.chroma[3])};
            Node* chromaExtent[2] = {g.u(s.size[2]), g.u(s.size[3])};
            Node* sampled[4] = {g.f(0.), g.f(0.), g.f(0.), g.f(0.)};

            double chromaRatio[2] = {(double)s.target[0] / s.size[2], (double)s.target[1] / s.size[3]};

            if (shaped) {
                filtered(at, extent, ratio, sharp, luma, l.alpha ? 2 : 1, out);
                filtered(chromaAt, chromaExtent, chromaRatio, sharp, chroma, 2, sampled);
            } else {
                grid(at, extent, luma, l.alpha ? 2 : 1, out);
                grid(chromaAt, chromaExtent, chroma, 2, sampled);
            }

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

        void signalOf(Node* const (&codes)[4], Node* (&signal)[3], Node*& alpha) {
            if (model("palette")) {
                for (int i = 0; i < 3; i++) {
                    signal[i] = codes[i];
                }

                alpha = codes[3];

                return;
            }

            for (int r = 0; r < 3; r++) {
                signal[r] = g.f(s.bias[r]);

                for (int c = 0; c < 3; c++) {
                    signal[r] = g.add(signal[r], g.mul(codes[c], s.decode[r][c]));
                }
            }

            if (!strcmp(s.system, "cl")) {
                constantLuminance(signal);
            }

            alpha = l.alpha ? g.mul(codes[3], s.bias[3]) : g.f(1.);
        }

        void lightOf(Node* const (&signal)[3], Node* (&light)[3]) {
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
            } else if (!strcmp(s.transfer, "curve") || !strcmp(s.transfer, "identity")) {
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
        }

        void outputOf(Node* (&light)[3], Node* (&out)[4]) {
            bool sdr = !strcmp(s.output, "sdr");

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

                if (linear) {
                    out[i] = display;
                    continue;
                }

                out[i] = g.select(g.le(display, 0.0031308), g.mul(display, 12.92), g.sub(g.mul(g.pow(display, 1. / 2.4), 1.055), 0.055));
            }
        }

        void color(Node* const (&codes)[4], Node* (&out)[4]) {
            Node* signal[3];
            Node* light[3];
            bool sdr = !strcmp(s.output, "sdr");

            signalOf(codes, signal, out[3]);

            if (linear) {
                lightOf(signal, light);
                outputOf(light, out);

                return;
            }

            if (!strcmp(s.transfer, "identity")) {
                for (int i = 0; i < 3; i++) {
                    out[i] = sdr ? g.clamp(signal[i], 0., 1.) : g.max(signal[i], 0.);
                }

                return;
            }

            const double* e = s.curve;
            bool power = e[0] == e[5] && e[1] == e[6] && e[2] == 1. && e[3] == 0. && e[4] == 0. && e[10] < 0.;

            if (!strcmp(s.transfer, "curve") && !strcmp(s.conversion, "same") && (!sdr || (power && (e[1] == 1. || e[1] == 2.4)))) {
                const double fused[11] = {1.055 * ::pow(e[0], 1. / 2.4), e[1] / 2.4, 1., 0., 0.055, 12.92 * e[0], e[1], 1., 0., 0., ::pow(0.0031308 / e[0], 1. / e[1])};

                for (int i = 0; i < 3; i++) {
                    out[i] = piece(sdr ? g.clamp(signal[i], 0., 1.) : g.max(signal[i], 0.), sdr ? fused : s.curve);
                }

                return;
            }

            lightOf(signal, light);
            outputOf(light, out);
        }

        Node* sigmoidize(Node* x) {
            Node* z = g.add(g.mul(g.clamp(x, 0., 1.), sigmoidSpan), sigmoidLow);

            return g.sub(sigmoidCenter, g.mul(g.log(g.sub(g.div(1., z), 1.)), 1. / sigmoidSlope));
        }

        Node* unsigmoidize(Node* v) {
            return g.mul(g.sub(g.div(1., g.add(g.exp(g.mul(g.sub(sigmoidCenter, v), sigmoidSlope)), 1.)), sigmoidLow), 1. / sigmoidSpan);
        }

        void decodeAt(Node* const (&at)[2], Node* (&codes)[4]) {
            Node* column = g.convert(at[0], Kind::Uint);
            Node* line = g.convert(at[1], Kind::Uint);
            bool yuv = model("yuv");

            for (int i = 0; i < 4; i++) {
                codes[i] = g.f(0.);
            }

            for (int c = 0; c < l.count; c++) {
                if (yuv && (c == 1 || c == 2)) {
                    continue;
                }

                int plane = l.components[c][0];
                Node* row = g.shr(g.add(g.u(s.planeOffset[plane]), g.mul(line, g.u(s.lineSize[plane]))), 2);

                codes[l.alpha && c == l.count - 1 ? 3 : c] = value(c, window(c, row, column));
            }

            if (yuv && s.chroma[0] == 1. && s.chroma[1] == 1.) {
                const int chroma[2] = {1, 2};
                Node* chromaAt[2] = {g.sub(g.mul(g.convert(at[0], Kind::Float), s.chroma[0]), s.chroma[2]), g.sub(g.mul(g.convert(at[1], Kind::Float), s.chroma[1]), s.chroma[3])};
                Node* chromaExtent[2] = {g.u(s.size[2]), g.u(s.size[3])};
                Node* sampled[4] = {g.f(0.), g.f(0.), g.f(0.), g.f(0.)};

                grid(chromaAt, chromaExtent, chroma, 2, sampled);
                codes[1] = sampled[1];
                codes[2] = sampled[2];
            }
        }

        struct Taps {
            int count;
            int radius;
            Node* base;
            Node* w[lanczosTaps];
        };

        Taps chromaTaps(Node* at, int i) {
            Taps t;
            int factor = (int)::lround(1. / s.chroma[i]);
            int shift = factor == 4 ? 2 : factor == 2 ? 1 : 0;
            Node* index = g.convert(at, Kind::Uint);
            Node* residue = g.band(index, (double)(factor - 1));

            t.count = factor > 1 ? lanczosTaps : 1;
            t.radius = factor > 1 ? lanczosRadius - 1 : 0;
            t.base = nullptr;

            for (int r = factor - 1; r >= 0; r--) {
                double place = r * s.chroma[i] - s.chroma[2 + i];
                double whole = ::floor(place);
                double w[lanczosTaps] = {1.};
                Node* first = g.i(whole - t.radius);

                if (factor > 1) {
                    lanczosWeights(place - whole, w);
                }

                Node* match = g.eq(residue, (double)r);

                t.base = t.base ? g.select(match, first, t.base) : first;

                for (int k = 0; k < t.count; k++) {
                    t.w[k] = r == factor - 1 ? g.f(w[k]) : g.select(match, g.f(w[k]), t.w[k]);
                }
            }

            t.base = g.add(g.convert(g.shr(index, (double)shift), Kind::Int), t.base);

            return t;
        }

        Node* partial(Node* index, int pass, int count, int capacity) {
            return (pass + 1) * capacity > count ? g.lt(index, count) : nullptr;
        }

        void kernel(Kernel& k) {
            const int tile = (int)s.tile;
            bool sdr = !strcmp(s.output, "sdr");
            bool subsampled = model("yuv") && (s.chroma[0] < 1. || s.chroma[1] < 1.);
            double ratio[2];
            int reach[2];
            int chromaReach[2] = {0, 0};
            Node* local[2] = {g.invocation(2, tile - 1), g.invocation(3, tile - 1)};
            Node* first[2];
            Node* corner[2];
            Node* chromaCorner[2] = {nullptr, nullptr};
            Node* last[2];

            for (int i = 0; i < 2; i++) {
                ratio[i] = (double)s.size[i] / s.target[i];
                reach[i] = (int)::ceil((tile - 1) * ratio[i]) + 7;
                first[i] = linear ? g.sub(g.origin(6 + i), (double)s.origin[i]) : g.mul(g.invocation(4 + i, (s.target[i] + tile - 1) / tile - 1), tile);
                corner[i] = g.add(g.convert(g.floor(g.sub(g.mul(g.add(g.convert(first[i], Kind::Float), 0.5), ratio[i]), 0.5)), Kind::Int), -(lanczosRadius - 1));
                last[i] = g.i(s.size[i] - 1.);

                if (subsampled) {
                    int radius = s.chroma[i] < 1. ? lanczosRadius - 1 : 0;

                    chromaCorner[i] = g.add(g.convert(g.floor(g.sub(g.mul(g.convert(corner[i], Kind::Float), s.chroma[i]), s.chroma[2 + i])), Kind::Int), -radius);
                    chromaReach[i] = (int)::ceil((reach[i] - 1) * s.chroma[i]) + 3 + 2 * radius;
                }
            }

            Node* lane = g.add(g.mul(local[1], tile), local[0]);

            if (subsampled) {
                int positions = chromaReach[0] * chromaReach[1];

                for (int pass = 0; pass * tile * tile < positions; pass++) {
                    Node* index = g.add(lane, pass * tile * tile);
                    Node* y = g.convert(g.floor(g.mul(g.add(g.convert(index, Kind::Float), 0.5), 1. / chromaReach[0])), Kind::Uint);
                    Node* x = g.sub(index, g.mul(y, chromaReach[0]));
                    Node* column = g.convert(g.clamp(g.add(chromaCorner[0], g.convert(x, Kind::Int)), 0, s.size[2] - 1.), Kind::Uint);
                    Node* line = g.convert(g.clamp(g.add(chromaCorner[1], g.convert(y, Kind::Int)), 0, s.size[3] - 1.), Kind::Uint);
                    Node* slot = g.mul(index, 2);
                    Node* guard = partial(index, pass, positions, tile * tile);

                    for (int c = 1; c <= 2; c++) {
                        int plane = l.components[c][0];
                        Node* row = g.shr(g.add(g.u(s.planeOffset[plane]), g.mul(line, g.u(s.lineSize[plane]))), 2);

                        k.stores[0].pushBack(Store{g.add(slot, c - 1), value(c, window(c, row, column)), guard});
                    }
                }

                k.sizes[0] = positions * 2;
                positions = reach[0] * chromaReach[1];

                for (int pass = 0; pass * tile * tile < positions; pass++) {
                    Node* index = g.add(lane, pass * tile * tile);
                    Node* y = g.convert(g.floor(g.mul(g.add(g.convert(index, Kind::Float), 0.5), 1. / reach[0])), Kind::Uint);
                    Node* x = g.sub(index, g.mul(y, reach[0]));
                    Node* row = y;
                    Taps across = chromaTaps(g.clamp(g.add(corner[0], g.convert(x, Kind::Int)), 0, last[0]), 0);
                    Node* start = g.convert(g.sub(across.base, chromaCorner[0]), Kind::Uint);
                    Node* slot = g.mul(index, 2);
                    Node* guard = partial(index, pass, positions, tile * tile);

                    for (int c = 1; c <= 2; c++) {
                        Node* sum = g.f(0.);

                        for (int t = 0; t < across.count; t++) {
                            sum = g.add(sum, g.mul(across.w[t], g.shared(g.add(g.mul(g.add(g.mul(row, chromaReach[0]), g.add(start, t)), 2), c - 1), 0)));
                        }

                        k.stores[1].pushBack(Store{g.add(slot, c - 1), sum, guard});
                    }
                }

                k.sizes[1] = positions * 2;
            }

            int positions = reach[0] * reach[1];

            for (int pass = 0; pass * tile * tile < positions; pass++) {
                Node* index = g.add(lane, pass * tile * tile);
                Node* y = g.convert(g.floor(g.mul(g.add(g.convert(index, Kind::Float), 0.5), 1. / reach[0])), Kind::Uint);
                Node* x = g.sub(index, g.mul(y, reach[0]));
                Node* at[2] = {g.clamp(g.add(corner[0], g.convert(x, Kind::Int)), 0, last[0]), g.clamp(g.add(corner[1], g.convert(y, Kind::Int)), 0, last[1])};
                Node* codes[4];
                Node* signal[3];
                Node* light[3];
                Node* alpha;
                Node* slot = g.mul(index, 3);
                Node* guard = partial(index, pass, positions, tile * tile);

                decodeAt(at, codes);

                if (subsampled) {
                    Taps down = chromaTaps(at[1], 1);
                    Node* start = g.convert(g.sub(down.base, chromaCorner[1]), Kind::Uint);

                    for (int c = 1; c <= 2; c++) {
                        Node* sum = g.f(0.);

                        for (int t = 0; t < down.count; t++) {
                            sum = g.add(sum, g.mul(down.w[t], g.shared(g.add(g.mul(g.add(g.mul(g.add(start, t), reach[0]), x), 2), c - 1), 1)));
                        }

                        codes[c] = sum;
                    }
                }

                signalOf(codes, signal, alpha);
                lightOf(signal, light);

                for (int c = 0; c < 3; c++) {
                    k.stores[2].pushBack(Store{g.add(slot, c), sdr ? sigmoidize(light[c]) : light[c], guard});
                }
            }

            k.sizes[2] = positions * 3;

            Node* across = g.convert(g.add(first[0], linear ? g.convert(local[0], Kind::Int) : local[0]), Kind::Float);
            Axis horizontal = axis(g.sub(g.mul(g.add(across, 0.5), ratio[0]), 0.5), 1. / ratio[0], true);
            Node* start = g.convert(g.sub(horizontal.origin, corner[0]), Kind::Uint);

            for (int pass = 0; pass * tile < reach[1]; pass++) {
                Node* row = g.add(local[1], pass * tile);
                Node* base = g.add(g.mul(row, reach[0]), start);
                Node* slot = g.mul(g.add(g.mul(row, tile), local[0]), 3);
                Node* guard = partial(row, pass, reach[1], tile);

                for (int c = 0; c < 3; c++) {
                    Node* sum = g.f(0.);

                    for (int t = 0; t < horizontal.taps; t++) {
                        sum = g.add(sum, g.mul(horizontal.w[t], g.shared(g.add(g.mul(g.add(base, t), 3), c), 2)));
                    }

                    k.stores[3].pushBack(Store{g.add(slot, c), sum, guard});
                }
            }

            k.sizes[3] = reach[1] * tile * 3;

            Node* pixel[2];
            Node* light[3];
            Node* centre[2];

            for (int i = 0; i < 2; i++) {
                pixel[i] = linear ? g.clamp(g.add(first[i], g.convert(local[i], Kind::Int)), 0., s.target[i] - 1.) : g.min(g.add(first[i], local[i]), s.target[i] - 1.);
                centre[i] = g.add(g.convert(pixel[i], Kind::Float), 0.5);
            }

            Node* column = linear ? g.convert(g.clamp(g.sub(pixel[0], first[0]), 0., tile - 1.), Kind::Uint) : g.sub(pixel[0], first[0]);
            Axis vertical = axis(g.sub(g.mul(centre[1], ratio[1]), 0.5), 1. / ratio[1], true);
            Node* top = g.convert(g.sub(vertical.origin, corner[1]), Kind::Uint);

            for (int c = 0; c < 3; c++) {
                Node* total = g.f(0.);

                for (int t = 0; t < vertical.taps; t++) {
                    total = g.add(total, g.mul(vertical.w[t], g.shared(g.add(g.mul(g.add(g.mul(g.add(top, t), tile), column), 3), c), 3)));
                }

                light[c] = sdr ? unsigmoidize(total) : total;
            }

            outputOf(light, k.color);
            k.color[3] = g.f(1.);

            if (linear) {
                return;
            }

            dither(centre, k.color);

            for (int i = 0; i < 2; i++) {
                k.at[i] = g.convert(g.add(pixel[i], s.origin[i]), Kind::Int);
            }
        }

        void pixelLayer(Kernel& k) {
            Node* values[4];

            codes(values);
            color(values, k.color);
        }

        void dither(Node* const (&pixel)[2], Node* (&out)[4]) {
            if (!s.dither) {
                return;
            }

            Node* at[2];

            for (int i = 0; i < 2; i++) {
                at[i] = g.add(pixel[i], 5.588238 * s.phase);
            }

            Node* inner = g.add(g.mul(at[0], 0.06711056), g.mul(at[1], 0.00583715));
            Node* outer = g.mul(g.sub(inner, g.floor(inner)), 52.9829189);
            Node* step = g.mul(g.sub(g.sub(outer, g.floor(outer)), 0.5), 1. / (::exp2(s.dither) - 1.));

            for (int i = 0; i < 3; i++) {
                out[i] = g.add(out[i], step);
            }
        }
    };

    static void order(Node* const* roots, size_t count, Vector<Node*>& out) {
        Vector<Node*> stack;

        for (size_t r = 0; r < count; r++) {
            stack.pushBack(roots[r]);

            while (!stack.empty()) {
                Node* top = stack.back();

                if (top->mark == 2) {
                    stack.popBack();
                    continue;
                }

                bool ready = true;

                top->mark = 1;

                for (int i = top->arity - 1; i >= 0; i--) {
                    if (!top->args[i]->mark) {
                        stack.pushBack(top->args[i]);
                        ready = false;
                    }
                }

                if (ready) {
                    stack.popBack();
                    top->mark = 2;
                    out.pushBack(top);
                }
            }
        }
    }

    static void forget(const Vector<Node*>& nodes) {
        for (Node* node : nodes) {
            if (node->op != Op::Const && node->op != Op::Input) {
                node->mark = 0;
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
        const char* inputs[8] = {"uv.x", "uv.y", "local.x", "local.y", "group.x", "group.y", "origin.x", "origin.y"};

        if (node->op == Op::Input) {
            out << StringView(inputs[(int)node->value]);
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

    const char* const sharedNames[kernelBuffers] = {"sharedA", "sharedB"};

    static void statements(StringBuilder& out, const Vector<Node*>& nodes, u32& next) {
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
            } else if (node->op == Op::Shared) {
                out << StringView(sharedNames[(int)node->value % kernelBuffers]) << StringView(u8"[");
                operand(out, node->args[0]);
                out << StringView(u8"]");
            } else if (node->op == Op::BitsFloat) {
                out << StringView(u8"as_type<float>(");
                operand(out, node->args[0]);
                out << StringView(u8")");
            } else {
                fail(StringView(u8"a video shader node has no Metal form"));
            }

            out << StringView(u8";\n");
        }
    }

    static void finish(StringBuilder& out, Node* const* color, const char* tail) {
        out << StringView(u8"float4(");

        for (int i = 0; i < 4; i++) {
            operand(out, color[i]);
            out << StringView(i < 3 ? ", " : ")");
        }

        out << StringView(tail);
    }

    static StringView copied(ObjPool& pool, StringBuilder& out) {
        u8* bytes = (u8*)pool.allocate(out.used());

        memcpy(bytes, out.data(), out.used());

        return StringView(bytes, out.used());
    }

    static StringView msl(ObjPool& pool, Node* const (&color)[4]) {
        Vector<Node*> nodes;
        StringBuilder out;
        u32 next = 3;

        order(color, 4, nodes);
        out << StringView(u8"#include <metal_stdlib>\nusing namespace metal;\nstruct In {\n    float2 uv [[user(locn0)]];\n};\nfragment float4 main0(In in [[stage_in]], const device uint* words [[buffer(0)]]) {\n    float2 uv = in.uv;\n");
        statements(out, nodes, next);
        out << StringView(u8"    return ");
        finish(out, color, ";\n}\n");

        return copied(pool, out);
    }

    static void phases(StringBuilder& out, const Kernel& k, u32& next) {
        for (int phase = 0; phase < kernelArrays; phase++) {
            const Vector<Store>& stores = k.stores[phase];

            if (stores.empty()) {
                continue;
            }

            for (size_t first = 0; first < stores.length();) {
                Node* guard = stores[first].guard;
                size_t end = first;
                Vector<Node*> roots;
                Vector<Node*> nodes;

                for (; end < stores.length() && stores[end].guard == guard; end++) {
                    roots.pushBack(stores[end].index);
                    roots.pushBack(stores[end].value);
                }

                if (guard) {
                    Vector<Node*> condition;

                    order(&guard, 1, condition);
                    statements(out, condition, next);
                    out << StringView(u8"    if (");
                    operand(out, guard);
                    out << StringView(u8") {\n");
                }

                order(roots.data(), roots.length(), nodes);
                statements(out, nodes, next);

                for (size_t i = first; i < end; i++) {
                    out << StringView(u8"    ") << StringView(sharedNames[phase % kernelBuffers]) << StringView(u8"[");
                    operand(out, stores[i].index);
                    out << StringView(u8"] = ");
                    operand(out, stores[i].value);
                    out << StringView(u8";\n");
                }

                if (guard) {
                    out << StringView(u8"    }\n");
                    forget(nodes);
                }

                first = end;
            }

            out << StringView(u8"    threadgroup_barrier(mem_flags::mem_threadgroup);\n");
        }
    }

    static StringView mslLayer(ObjPool& pool, const Kernel& k, Node* const* uv) {
        StringBuilder out;
        u32 next = 3;

        out << StringView(u8"#define LAYER_SHARED");

        for (int i = 0; i < kernelBuffers; i++) {
            out << StringView(u8" threadgroup float ") << StringView(sharedNames[i]) << StringView(u8"[") << (u64)(k.buffer(i) ? k.buffer(i) : 1) << StringView(u8"];");
        }

        out << StringView(u8"\n#define LAYER_CALL(local, origin, words) layer(local, origin, words, sharedA, sharedB)\n");
        out << StringView(u8"float4 layer(uint2 local, int2 origin, const device uint* words, threadgroup float* sharedA, threadgroup float* sharedB) {\n");

        if (uv) {
            Vector<Node*> nodes;

            order(uv, 2, nodes);
            statements(out, nodes, next);
            out << StringView(u8"    float2 uv = float2(");
            operand(out, uv[0]);
            out << StringView(u8", ");
            operand(out, uv[1]);
            out << StringView(u8");\n");
        }

        phases(out, k, next);

        Vector<Node*> nodes;

        order(k.color, 4, nodes);
        statements(out, nodes, next);
        out << StringView(u8"    return ");
        finish(out, k.color, ";\n}\n");

        return copied(pool, out);
    }

    static StringView mslKernel(ObjPool& pool, const Kernel& k) {
        StringBuilder out;
        u32 next = 3;

        out << StringView(u8"#include <metal_stdlib>\nusing namespace metal;\nkernel void main0(const device uint* words [[buffer(0)]], texture2d<float, access::write> target [[texture(0)]], uint3 local [[thread_position_in_threadgroup]], uint3 group [[threadgroup_position_in_grid]]) {\n");
        for (int i = 0; i < kernelBuffers; i++) {
            if (k.buffer(i)) {
                out << StringView(u8"    threadgroup float ") << StringView(sharedNames[i]) << StringView(u8"[") << (u64)k.buffer(i) << StringView(u8"];\n");
            }
        }

        phases(out, k, next);

        Node* roots[6] = {k.at[0], k.at[1], k.color[0], k.color[1], k.color[2], k.color[3]};
        Vector<Node*> nodes;

        order(roots, 6, nodes);
        statements(out, nodes, next);
        out << StringView(u8"    target.write(");
        finish(out, k.color, ", uint2(");
        operand(out, k.at[0]);
        out << StringView(u8", ");
        operand(out, k.at[1]);
        out << StringView(u8"));\n}\n");

        return copied(pool, out);
    }
}
#else
namespace {
    enum : u32 {
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

    struct Emitter {
        Spirv s;
        u32 glsl = s.fresh();
        u32 typeVoid = s.fresh();
        u32 typeFunction = s.fresh();
        u32 typeBool = s.fresh();
        u32 typeFloat = s.fresh();
        u32 typeUint = s.fresh();
        u32 typeInt = s.fresh();
        u32 typeVec2 = s.fresh();
        u32 typeVec4 = s.fresh();
        u32 typeWords = s.fresh();
        u32 typeBytes = s.fresh();
        u32 storageBytes = s.fresh();
        u32 storageWord = s.fresh();
        u32 bytes = s.fresh();
        u32 main = s.fresh();
        u32 label = s.fresh();
        u32 sharedFloat = s.fresh();
        u32 arrays[kernelBuffers] = {s.fresh(), s.fresh()};
        u32 inputs[8] = {s.fresh(), s.fresh(), s.fresh(), s.fresh(), s.fresh(), s.fresh(), s.fresh(), s.fresh()};
        u32 zero = 0;
        u32 one = 0;
        u32 none = 0;

        explicit Emitter(Graph& g) {
            Node* extras[3] = {g.i(0.), g.f(1.), g.f(0.)};
            Vector<Node*> nodes;

            order(extras, 3, nodes);
            emit(nodes);
            zero = extras[0]->id;
            one = extras[1]->id;
            none = extras[2]->id;
        }

        u32 type(Kind kind) const {
            return kind == Kind::Float ? typeFloat : kind == Kind::Uint ? typeUint : kind == Kind::Int ? typeInt : typeBool;
        }

        void emit(const Vector<Node*>& nodes) {
            for (Node* node : nodes) {
                if (node->op == Op::Input) {
                    node->id = inputs[(int)node->value];
                    continue;
                }

                node->id = s.fresh();

                if (node->op == Op::Const) {
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
                            Spirv::op(s.body, 169, result, node->id, a, one, none);
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

                        Spirv::op(s.body, 65, storageWord, pointer, bytes, zero, a);
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
                    case Op::Shared: {
                        u32 pointer = s.fresh();

                        Spirv::op(s.body, 65, sharedFloat, pointer, arrays[(int)node->value % kernelBuffers], a);
                        Spirv::op(s.body, 61, typeFloat, node->id, pointer);
                        break;
                    }
                    default:
                        fail(StringView(u8"a video shader node has no SPIR-V form"));
                }
            }
        }

        void phases(const Kernel& k, u32 scope, u32 semantics) {
            for (int phase = 0; phase < kernelArrays; phase++) {
                const Vector<Store>& stores = k.stores[phase];

                if (stores.empty()) {
                    continue;
                }

                for (size_t first = 0; first < stores.length();) {
                    Node* guard = stores[first].guard;
                    size_t end = first;
                    u32 merge = 0;
                    Vector<Node*> roots;
                    Vector<Node*> nodes;

                    for (; end < stores.length() && stores[end].guard == guard; end++) {
                        roots.pushBack(stores[end].index);
                        roots.pushBack(stores[end].value);
                    }

                    if (guard) {
                        Vector<Node*> condition;
                        u32 inside = s.fresh();

                        merge = s.fresh();
                        order(&guard, 1, condition);
                        emit(condition);
                        Spirv::op(s.body, 247, merge, 0);
                        Spirv::op(s.body, 250, guard->id, inside, merge);
                        Spirv::op(s.body, 248, inside);
                    }

                    order(roots.data(), roots.length(), nodes);
                    emit(nodes);

                    for (size_t i = first; i < end; i++) {
                        u32 pointer = s.fresh();

                        Spirv::op(s.body, 65, sharedFloat, pointer, arrays[phase % kernelBuffers], stores[i].index->id);
                        Spirv::op(s.body, 62, pointer, stores[i].value->id);
                    }

                    if (guard) {
                        Spirv::op(s.body, 249, merge);
                        Spirv::op(s.body, 248, merge);
                        forget(nodes);
                    }

                    first = end;
                }

                Spirv::op(s.body, 224, scope, scope, semantics);
            }
        }

        void common(Vector<u32>& module, u32 set) {
            Spirv::op(module, 71, typeWords, 6, 4);
            Spirv::op(module, 72, typeBytes, 0, 24);
            Spirv::op(module, 72, typeBytes, 0, 35, 0);
            Spirv::op(module, 71, typeBytes, 2);
            Spirv::op(module, 71, bytes, 34, set);
            Spirv::op(module, 71, bytes, 33, 0);
        }

        void types(Vector<u32>& module) {
            Spirv::op(module, 19, typeVoid);
            Spirv::op(module, 33, typeFunction, typeVoid);
            Spirv::op(module, 20, typeBool);
            Spirv::op(module, 22, typeFloat, 32);
            Spirv::op(module, 21, typeUint, 32, 0);
            Spirv::op(module, 21, typeInt, 32, 1);
            Spirv::op(module, 23, typeVec2, typeFloat, 2);
            Spirv::op(module, 23, typeVec4, typeFloat, 4);
            Spirv::op(module, 29, typeWords, typeUint);
            Spirv::op(module, 30, typeBytes, typeWords);
            Spirv::op(module, 32, storageBytes, 12, typeBytes);
            Spirv::op(module, 32, storageWord, 12, typeUint);
        }

        StringView finish(ObjPool& pool, Vector<u32>& module) {
            module.append(s.globals.data(), s.globals.length());
            module.append(s.body.data(), s.body.length());
            module.mut(3) = s.next;

            size_t size = module.length() * sizeof(u32);
            u8* out = (u8*)pool.allocate(size);

            memcpy(out, module.data(), size);

            return StringView(out, size);
        }

        void head(Vector<u32>& module, bool kernel) {
            Vector<u32> entry;

            module.pushBack(0x07230203u);
            module.pushBack(0x00010300u);
            module.pushBack(0);
            module.pushBack(0);
            module.pushBack(0);
            Spirv::op(module, 17, 1);

            if (kernel) {
                Spirv::op(module, 17, 56);
            }

            entry.pushBack(glsl);
            Spirv::string(entry, "GLSL.std.450");
            Spirv::put(module, 11, entry.data(), (u32)entry.length());
            Spirv::op(module, 14, 0, 1);
        }
    };

    static StringView spirv(ObjPool& pool, Graph& g, Node* const (&color)[4]) {
        Emitter e(g);
        Spirv& s = e.s;
        Vector<Node*> nodes;
        u32 inputVec2 = s.fresh();
        u32 outputVec4 = s.fresh();
        u32 uv = s.fresh();
        u32 fColor = s.fresh();
        u32 coordinates = s.fresh();
        u32 pixel = s.fresh();

        Spirv::op(s.body, 54, e.typeVoid, e.main, 0, e.typeFunction);
        Spirv::op(s.body, 248, e.label);
        Spirv::op(s.body, 61, e.typeVec2, coordinates, uv);
        Spirv::op(s.body, 81, e.typeFloat, e.inputs[0], coordinates, 0);
        Spirv::op(s.body, 81, e.typeFloat, e.inputs[1], coordinates, 1);
        order(color, 4, nodes);
        e.emit(nodes);
        Spirv::op(s.body, 80, e.typeVec4, pixel, color[0]->id, color[1]->id, color[2]->id, color[3]->id);
        Spirv::op(s.body, 62, fColor, pixel);
        Spirv::op(s.body, 253);
        Spirv::op(s.body, 56);

        Vector<u32> module;
        Vector<u32> entry;

        e.head(module, false);
        entry.pushBack(4);
        entry.pushBack(e.main);
        Spirv::string(entry, "main");
        entry.pushBack(uv);
        entry.pushBack(fColor);
        Spirv::put(module, 15, entry.data(), (u32)entry.length());
        Spirv::op(module, 16, e.main, 7);
        Spirv::op(module, 71, uv, 30, 0);
        Spirv::op(module, 71, fColor, 30, 0);
        e.common(module, 0);
        e.types(module);
        Spirv::op(module, 32, inputVec2, 1, e.typeVec2);
        Spirv::op(module, 32, outputVec4, 3, e.typeVec4);
        Spirv::op(module, 59, inputVec2, uv, 1);
        Spirv::op(module, 59, outputVec4, fColor, 3);
        Spirv::op(module, 59, e.storageBytes, e.bytes, 12);

        return e.finish(pool, module);
    }

    static StringView spirvLayer(ObjPool& pool, Graph& g, const Kernel& k, Node* const* uv) {
        Emitter e(g);
        Spirv& s = e.s;
        u32 typeUvec2 = s.fresh();
        u32 typeIvec2 = s.fresh();
        u32 pointerUvec2 = s.fresh();
        u32 pointerIvec2 = s.fresh();
        u32 typeLayer = s.fresh();
        u32 lengths[kernelBuffers] = {s.fresh(), s.fresh()};
        u32 arrayTypes[kernelBuffers] = {s.fresh(), s.fresh()};
        u32 arrayPointers[kernelBuffers] = {s.fresh(), s.fresh()};
        u32 scope = s.fresh();
        u32 semantics = s.fresh();
        u32 local = s.fresh();
        u32 origin = s.fresh();
        u32 loaded[2] = {s.fresh(), s.fresh()};
        u32 color = s.fresh();

        Spirv::op(s.body, 54, e.typeVec4, e.main, 0, typeLayer);
        Spirv::op(s.body, 55, pointerUvec2, local);
        Spirv::op(s.body, 55, pointerIvec2, origin);
        Spirv::op(s.body, 248, e.label);
        Spirv::op(s.body, 61, typeUvec2, loaded[0], local);
        Spirv::op(s.body, 81, e.typeUint, e.inputs[2], loaded[0], 0);
        Spirv::op(s.body, 81, e.typeUint, e.inputs[3], loaded[0], 1);
        Spirv::op(s.body, 61, typeIvec2, loaded[1], origin);
        Spirv::op(s.body, 81, e.typeInt, e.inputs[6], loaded[1], 0);
        Spirv::op(s.body, 81, e.typeInt, e.inputs[7], loaded[1], 1);

        if (uv) {
            Vector<Node*> nodes;

            order(uv, 2, nodes);
            e.emit(nodes);
            e.inputs[0] = uv[0]->id;
            e.inputs[1] = uv[1]->id;
        }

        e.phases(k, scope, semantics);

        Vector<Node*> nodes;

        order(k.color, 4, nodes);
        e.emit(nodes);
        Spirv::op(s.body, 80, e.typeVec4, color, k.color[0]->id, k.color[1]->id, k.color[2]->id, k.color[3]->id);
        Spirv::op(s.body, 254, color);
        Spirv::op(s.body, 56);

        Vector<u32> module;

        e.head(module, false);
        e.common(module, 1);
        e.types(module);
        Spirv::op(module, 23, typeUvec2, e.typeUint, 2);
        Spirv::op(module, 23, typeIvec2, e.typeInt, 2);
        Spirv::op(module, 32, pointerUvec2, 7, typeUvec2);
        Spirv::op(module, 32, pointerIvec2, 7, typeIvec2);
        Spirv::op(module, 33, typeLayer, e.typeVec4, pointerUvec2, pointerIvec2);

        for (int i = 0; i < kernelBuffers; i++) {
            if (k.buffer(i)) {
                Spirv::op(module, 43, e.typeUint, lengths[i], k.buffer(i));
                Spirv::op(module, 28, arrayTypes[i], e.typeFloat, lengths[i]);
                Spirv::op(module, 32, arrayPointers[i], 4, arrayTypes[i]);
            }
        }

        Spirv::op(module, 32, e.sharedFloat, 4, e.typeFloat);
        Spirv::op(module, 43, e.typeUint, scope, 2);
        Spirv::op(module, 43, e.typeUint, semantics, 264);
        Spirv::op(module, 59, e.storageBytes, e.bytes, 12);

        for (int i = 0; i < kernelBuffers; i++) {
            if (k.buffer(i)) {
                Spirv::op(module, 59, arrayPointers[i], e.arrays[i], 4);
            }
        }

        return e.finish(pool, module);
    }

    static StringView spirvKernel(ObjPool& pool, Graph& g, const Kernel& k) {
        Emitter e(g);
        Spirv& s = e.s;
        u32 typeUvec3 = s.fresh();
        u32 typeIvec2 = s.fresh();
        u32 inputUvec3 = s.fresh();
        u32 typeImage = s.fresh();
        u32 imagePointer = s.fresh();
        u32 image = s.fresh();
        u32 local = s.fresh();
        u32 group = s.fresh();
        u32 lengths[kernelBuffers] = {s.fresh(), s.fresh()};
        u32 arrayTypes[kernelBuffers] = {s.fresh(), s.fresh()};
        u32 arrayPointers[kernelBuffers] = {s.fresh(), s.fresh()};
        u32 scope = s.fresh();
        u32 semantics = s.fresh();
        u32 ids[2] = {s.fresh(), s.fresh()};

        Spirv::op(s.body, 54, e.typeVoid, e.main, 0, e.typeFunction);
        Spirv::op(s.body, 248, e.label);
        Spirv::op(s.body, 61, typeUvec3, ids[0], local);
        Spirv::op(s.body, 81, e.typeUint, e.inputs[2], ids[0], 0);
        Spirv::op(s.body, 81, e.typeUint, e.inputs[3], ids[0], 1);
        Spirv::op(s.body, 61, typeUvec3, ids[1], group);
        Spirv::op(s.body, 81, e.typeUint, e.inputs[4], ids[1], 0);
        Spirv::op(s.body, 81, e.typeUint, e.inputs[5], ids[1], 1);

        e.phases(k, scope, semantics);

        Node* roots[6] = {k.at[0], k.at[1], k.color[0], k.color[1], k.color[2], k.color[3]};
        Vector<Node*> nodes;
        u32 coordinate = s.fresh();
        u32 loaded = s.fresh();
        u32 texel = s.fresh();

        order(roots, 6, nodes);
        e.emit(nodes);
        Spirv::op(s.body, 80, typeIvec2, coordinate, k.at[0]->id, k.at[1]->id);
        Spirv::op(s.body, 61, typeImage, loaded, image);
        Spirv::op(s.body, 80, e.typeVec4, texel, k.color[0]->id, k.color[1]->id, k.color[2]->id, k.color[3]->id);
        Spirv::op(s.body, 99, loaded, coordinate, texel);
        Spirv::op(s.body, 253);
        Spirv::op(s.body, 56);

        Vector<u32> module;
        Vector<u32> entry;

        e.head(module, true);
        entry.pushBack(5);
        entry.pushBack(e.main);
        Spirv::string(entry, "main");
        entry.pushBack(local);
        entry.pushBack(group);
        Spirv::put(module, 15, entry.data(), (u32)entry.length());
        Spirv::op(module, 16, e.main, 17, k.tile, k.tile, 1);
        Spirv::op(module, 71, local, 11, 27);
        Spirv::op(module, 71, group, 11, 26);
        e.common(module, 0);
        Spirv::op(module, 71, image, 34, 0);
        Spirv::op(module, 71, image, 33, 1);
        Spirv::op(module, 71, image, 25);
        e.types(module);
        Spirv::op(module, 23, typeUvec3, e.typeUint, 3);
        Spirv::op(module, 23, typeIvec2, e.typeInt, 2);
        Spirv::op(module, 32, inputUvec3, 1, typeUvec3);
        Spirv::op(module, 25, typeImage, e.typeFloat, 1, 0, 0, 0, 2, 0);
        Spirv::op(module, 32, imagePointer, 0, typeImage);

        for (int i = 0; i < kernelBuffers; i++) {
            if (k.buffer(i)) {
                Spirv::op(module, 43, e.typeUint, lengths[i], k.buffer(i));
                Spirv::op(module, 28, arrayTypes[i], e.typeFloat, lengths[i]);
                Spirv::op(module, 32, arrayPointers[i], 4, arrayTypes[i]);
            }
        }

        Spirv::op(module, 32, e.sharedFloat, 4, e.typeFloat);
        Spirv::op(module, 43, e.typeUint, scope, 2);
        Spirv::op(module, 43, e.typeUint, semantics, 264);
        Spirv::op(module, 59, inputUvec3, local, 1);
        Spirv::op(module, 59, inputUvec3, group, 1);
        Spirv::op(module, 59, e.storageBytes, e.bytes, 12);
        Spirv::op(module, 59, imagePointer, image, 0);

        for (int i = 0; i < kernelBuffers; i++) {
            if (k.buffer(i)) {
                Spirv::op(module, 59, arrayPointers[i], e.arrays[i], 4);
            }
        }

        return e.finish(pool, module);
    }
}
#endif

bool kernelable(const VideoShader& shader) {
    const VideoLayout& layout = *shader.layout;

    return strcmp(layout.model, "palette") && strcmp(layout.model, "bayer") && !layout.alpha && shader.target[0] >= shader.size[0] && shader.target[1] >= shader.size[1];
}

StringView compile(ObjPool& pool, const VideoShader& shader) {
    Graph g(pool);
    Video video{g, shader, *shader.layout};

    if (!strcmp(shader.stage, "layer")) {
        Kernel layer;
        Node* uv[2];
        bool lds = !strcmp(shader.filter, "lanczos") && kernelable(shader);

        if (!shader.tile || shader.tile > 32) {
            fail(StringView(u8"a video layer has an invalid tile"));
        }

        layer.tile = shader.tile;
        video.linear = true;

        if (lds) {
            video.kernel(layer);
        } else {
            for (int i = 0; i < 2; i++) {
                Node* pixel = g.add(g.sub(g.origin(6 + i), (double)shader.origin[i]), g.convert(g.invocation(2 + i, shader.tile - 1.), Kind::Int));

                uv[i] = g.clamp(g.div(g.add(g.convert(pixel, Kind::Float), 0.5), (double)shader.target[i]), 0., 1.);
            }

            video.pixelLayer(layer);
        }

#if defined(__APPLE__)
        return mslLayer(pool, layer, lds ? nullptr : uv);
#else
        return spirvLayer(pool, g, layer, lds ? nullptr : uv);
#endif
    }

    if (!strcmp(shader.stage, "kernel")) {
        Kernel kernel;

        if (!kernelable(shader)) {
            fail(StringView(u8"a video kernel cannot draw this frame"));
        }

        if (!shader.tile || shader.tile > 32) {
            fail(StringView(u8"a video kernel has an invalid tile"));
        }

        kernel.tile = shader.tile;

        video.kernel(kernel);

#if defined(__APPLE__)
        return mslKernel(pool, kernel);
#else
        return spirvKernel(pool, g, kernel);
#endif
    }

    if (strcmp(shader.stage, "fragment")) {
        fail(StringView(u8"a video shader has an unknown stage"));
    }

    Node* codes[4];
    Node* color[4];
    Node* pixel[2] = {g.mul(g.input(0), (double)shader.target[0]), g.mul(g.input(1), (double)shader.target[1])};

    video.codes(codes);
    video.color(codes, color);
    video.dither(pixel, color);

#if defined(__APPLE__)
    return msl(pool, color);
#else
    return spirv(pool, g, color);
#endif
}
