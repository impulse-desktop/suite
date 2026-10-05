#include "shader.h"

#include "error.h"
#include "renderer.h"

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
        bool uniform;
        bool varies;
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

            bool uniform = op != Op::Input || (value >= 4. && value < 16.) || value >= 32.;
            bool varies = op == Op::Shared || (op == Op::Input && value >= 8. && value < 32.);

            for (int i = 0; i < arity; i++) {
                uniform = uniform && args[i]->uniform;
                varies = varies || args[i]->varies;
            }

            Node* node = pool.make<Node>(Node{op, kind, (u8)arity, {args[0], args[1], args[2]}, value, lo, hi, count++, 0, 0, uniform, varies});

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

        Node* invocation(int which, double hi) {
            return make(Op::Input, Kind::Uint, 0, nullptr, nullptr, nullptr, which, 0., hi);
        }

        Node* origin(int which) {
            return make(Op::Input, Kind::Int, 0, nullptr, nullptr, nullptr, which, 0., 65535.);
        }

        Node* frame(int which, Kind kind, double hi) {
            return make(Op::Input, kind, 0, nullptr, nullptr, nullptr, 32. + which, 0., hi);
        }

        Node* portion(int count) {
            return make(Op::Input, Kind::Int, 0, nullptr, nullptr, nullptr, 8., 0., count - 1.);
        }

        Node* carried(int which) {
            return make(Op::Input, Kind::Float, 0, nullptr, nullptr, nullptr, 16. + which, -INFINITY, INFINITY);
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

            if (b->op == Op::Const && a->op == Op::Add && a->args[1]->op == Op::Const) {
                return addValues(a->args[0], addValues(a->args[1], b));
            }

            if (b->op == Op::Const && a->op == Op::Sub && a->args[1]->op == Op::Const) {
                return addValues(a->args[0], subValues(b, a->args[1]));
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

                if (a->op == Op::Mul && a->args[1]->op == Op::Const) {
                    return mulValues(a->args[0], mulValues(a->args[1], b));
                }

                if ((a->op == Op::Add || a->op == Op::Sub) && a->args[1]->op == Op::Const) {
                    Node* scaled = mulValues(a->args[0], b);
                    Node* offset = mulValues(a->args[1], b);

                    return a->op == Op::Add ? addValues(scaled, offset) : subValues(scaled, offset);
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

        static bool aligned(const Node* x, double step) {
            if (x->op == Op::Const) {
                return ::fmod(x->value, step) == 0.;
            }

            if (x->op == Op::Shl && x->args[1]->op == Op::Const) {
                return ::exp2(x->args[1]->value) >= step;
            }

            if (x->op == Op::Mul && x->args[1]->op == Op::Const) {
                return ::fmod(x->args[1]->value, step) == 0.;
            }

            return x->op == Op::Add && aligned(x->args[0], step) && aligned(x->args[1], step);
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

            if (s->op == Op::Const && a->arity == 2 && a->args[1]->op == Op::Const) {
                double step = ::exp2(s->value);
                double k = a->args[1]->value;

                if (a->op == Op::Add && k >= 0. && aligned(a->args[0], step)) {
                    return addValues(shrValues(a->args[0], s), u(::floor(k / step)));
                }

                if (a->op == Op::Shl) {
                    return k >= s->value ? shlValues(a->args[0], u(k - s->value)) : shrValues(a->args[0], u(s->value - k));
                }

                if (a->op == Op::Mul && ::fmod(k, step) == 0.) {
                    return convert(mulValues(a->args[0], constant(k / step, a->kind)), Kind::Uint);
                }
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

            if ((a->op == Op::Convert && a->args[0]->kind != Kind::Float) || a->op == Op::Floor) {
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

            if (a->op == Op::Convert && a->kind != Kind::Bool && a->args[0]->kind != Kind::Bool && kind != Kind::Bool) {
                Node* b = a->args[0];

                if (b->kind != Kind::Float || kind != Kind::Float) {
                    return convert(b, kind);
                }

                if (b->op == Op::Floor) {
                    return b;
                }
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

        Node* fetch(int buffer, Node* index) {
            return make(Op::Load, Kind::Uint, 1, index, nullptr, nullptr, buffer, 0., word);
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
    constexpr int maxTaps = 64;

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

    constexpr int kernelPhases = 512;
    constexpr int kernelBuffers = 2;

    struct Loop {
        int first = -1;
        int last = -1;
        int count = 0;
        int carried = 0;
        Node* start[4];
        Node* next[4];
    };

    constexpr u32 composeFill = 0x80000000u;
    constexpr int headerWords = 8;
    constexpr u8 transferSrgb = 13;
    constexpr int opWords = 20;

    enum ComposeBuffer {
        Words,
        Headers,
        List,
        Ops,
        Tiles,
    };

    struct Kernel {
        Vector<Store> stores[kernelPhases];
        Vector<Node*> before[kernelPhases];
        Loop loop;
        int count = 0;
        u32 sizes[kernelBuffers] = {};
        u32 tile = 0;
        Node* shown[4];
        Node* encoded[4];
        Node* pixel[2];
        Node* drawn;

        int open(u32 size) {
            u32& buffer = sizes[count % kernelBuffers];

            buffer = buffer > size ? buffer : size;

            return count++;
        }

        u32 buffer(int i) const {
            return sizes[i];
        }
    };

    static double radiusOf(double ratio) {
        return ratio <= 1. ? lanczosRadius : ::fmin(ratio, maxTaps / 2.);
    }

    static int tapsOf(double ratio) {
        return (int)::ceil(2. * radiusOf(ratio) - 1e-9);
    }

    static int reachOf(double ratio, int rows) {
        return (int)::ceil((rows - 1) * ratio) + tapsOf(ratio) + 1;
    }

    static bool shrinking(const VideoShader& s) {
        return s.size[1] > s.target[1];
    }

    static int boxOf(const VideoShader& s, int i) {
        double ratio = (double)s.size[i] / s.target[i];

        return ratio > maxTaps / 2. ? (int)::ceil(ratio / (maxTaps / 2.) - 1e-9) : 1;
    }

    static double ratioOf(const VideoShader& s, int i) {
        return (double)s.size[i] / s.target[i] / boxOf(s, i);
    }

    static int footprintOf(const VideoShader& s) {
        return reachOf(ratioOf(s, 1), (int)s.tile);
    }

    static int blockRows(const VideoShader& s, bool portions) {
        const int tile = (int)s.tile;
        const int channels = s.layout->alpha ? 4 : 3;
        bool boxed = boxOf(s, 0) > 1 || boxOf(s, 1) > 1;
        bool subsampled = !boxed && !strcmp(s.layout->model, "yuv") && (s.chroma[0] < 1. || s.chroma[1] < 1.);
        double ratio[2] = {ratioOf(s, 0), ratioOf(s, 1)};
        int most = portions ? footprintOf(s) : tile;

        for (int rows = most; rows >= 1; rows--) {
            if (!portions && tile % rows) {
                continue;
            }

            int reach[2] = {reachOf(ratio[0], tile), portions ? rows : reachOf(ratio[1], rows)};
            int a = 0;
            int b = 0;
            int c = reach[0] * reach[1] * channels;
            int d = reach[1] * tile * channels;

            if (subsampled) {
                int chroma[2];

                for (int i = 0; i < 2; i++) {
                    int radius = s.chroma[i] < 1. ? lanczosRadius - 1 : 0;

                    chroma[i] = (int)::ceil((reach[i] - 1) * s.chroma[i]) + 3 + 2 * radius;
                }

                a = chroma[0] * chroma[1] * 2;
                b = chroma[0] * reach[1] * 2;
            }

            if (((a > c ? a : c) + (b > d ? b : d)) * 4 <= (int)kernelShared) {
                return rows;
            }
        }

        return 0;
    }

    constexpr double sigmoidCenter = 0.75;
    constexpr double sigmoidSlope = 6.5;
    constexpr double sigmoidLow = 0.007577241268;
    constexpr double sigmoidSpan = 0.8279062958;

    struct Video {
        Graph& g;
        const VideoShader& s;
        const VideoLayout& l;
        const ShaderOptions& o;

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
            Node* at;
            Node* inverse;
            double reach;
            Node* w[maxTaps];
        };

        Axis axis(Node* at, double ratio) {
            Axis a;
            Node* floor = g.floor(at);
            Node* base = g.convert(floor, Kind::Int);
            Node* f = g.sub(at, floor);

            a.at = at;
            a.inverse = nullptr;
            a.reach = 0.;

            if (ratio == 1.) {
                a.taps = 1;
                a.w[0] = g.f(1.);
                a.origin = g.convert(g.floor(g.add(at, 0.5)), Kind::Int);

                return a;
            }

            if (ratio > 1.) {
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
            } else {
                double reach = radiusOf(1. / ratio);
                Node* low = g.sub(at, reach);
                Node* start = g.floor(low);
                Node* offset = g.add(g.sub(low, start), reach - 1.);
                Node* total = g.f(0.);

                a.taps = tapsOf(1. / ratio);
                base = g.add(g.convert(start, Kind::Int), a.taps / 2);

                for (int k = 0; k < a.taps; k++) {
                    Node* x = g.min(g.mul(g.abs(g.sub(offset, k)), 1. / reach), 1.);

                    a.w[k] = g.add(g.mul(g.mul(g.sub(g.mul(x, 2.), 3.), x), x), 1.);
                    total = g.add(total, a.w[k]);
                }

                Node* inverse = g.div(1., total);

                for (int k = 0; k < a.taps; k++) {
                    a.w[k] = g.mul(a.w[k], inverse);
                }

                a.inverse = inverse;
                a.reach = reach;
            }

            a.origin = g.add(base, 1 - a.taps / 2);

            return a;
        }

        int span(int c, int taps) const {
            const int* k = l.components[c];

            return (taps - 1) * k[1] + (k[3] + k[4] <= 8 ? 1 : k[3] + k[4] <= 16 ? 2 : 4);
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

                out[i] = g.clamp(light[i], 0., 1.);
            }
        }

        Node* sigmoidize(Node* x) {
            Node* z = g.add(g.mul(g.clamp(x, 0., 1.), sigmoidSpan), sigmoidLow);

            return g.sub(sigmoidCenter, g.mul(g.log(g.sub(g.div(1., z), 1.)), 1. / sigmoidSlope));
        }

        Node* unsigmoidize(Node* v) {
            return g.mul(g.sub(g.div(1., g.add(g.exp(g.mul(g.sub(sigmoidCenter, v), sigmoidSlope)), 1.)), sigmoidLow), 1. / sigmoidSpan);
        }

        Node* windowStart(Node* head, double ratio) {
            Node* at = g.sub(g.mul(g.add(g.convert(head, Kind::Float), 0.5), ratio), 0.5);

            return g.add(g.convert(g.floor(g.sub(at, radiusOf(ratio))), Kind::Int), 1);
        }

        Node* weightAt(const Axis& a, Node* position) {
            Node* x = g.min(g.mul(g.abs(g.sub(position, a.at)), 1. / a.reach), 1.);

            return g.mul(g.add(g.mul(g.mul(g.sub(g.mul(x, 2.), 3.), x), x), 1.), a.inverse);
        }

        void paletteAt(Node* column, Node* line, Node* (&codes)[4]) {
            Node* row = g.shr(g.add(g.u(s.planeOffset[0]), g.mul(line, g.u(s.lineSize[0]))), 2);
            Node* index = g.band(g.shr(g.load(g.add(row, g.shr(column, 2))), g.shl(g.band(column, 3), 3)), 0xff);
            Node* entry = g.load(g.add(g.u(s.planeOffset[1] / 4), index));
            const int bytes[4] = {2, 1, 0, 3};

            for (int c = 0; c < 4; c++) {
                codes[c] = g.mul(g.convert(g.band(g.shr(entry, 8 * bytes[c]), 0xff), Kind::Float), 1. / 255.);
            }
        }

        void bayerAt(Node* const (&at)[2], Node* (&codes)[4]) {
            Node* xs[4] = {g.add(at[0], -1), at[0], g.add(at[0], 1), g.add(at[0], 2)};
            Node* rows[3][4];
            Node* rgb[3];

            for (int r = 0; r < 3; r++) {
                mosaic(g.add(at[1], r - 1), xs, rows[r]);
            }

            demosaic(rows[0], rows[1], rows[2], g.f(0.), at, rgb);

            for (int i = 0; i < 3; i++) {
                codes[i] = rgb[i];
            }

            codes[3] = g.f(0.);
        }

        void nearChroma(Node* const (&at)[2], Node* (&codes)[4]) {
            Node* position[2];

            for (int i = 0; i < 2; i++) {
                Node* place = g.sub(g.mul(g.convert(at[i], Kind::Float), s.chroma[i]), s.chroma[2 + i]);

                position[i] = g.convert(g.clamp(g.convert(g.floor(g.add(place, 0.5)), Kind::Int), 0, s.size[2 + i] - 1.), Kind::Uint);
            }

            for (int c = 1; c <= 2; c++) {
                int plane = l.components[c][0];
                Node* row = g.shr(g.add(g.u(s.planeOffset[plane]), g.mul(position[1], g.u(s.lineSize[plane]))), 2);

                codes[c] = value(c, window(c, row, position[0]));
            }
        }

        void decodeAt(Node* const (&at)[2], Node* (&codes)[4]) {
            Node* column = g.convert(at[0], Kind::Uint);
            Node* line = g.convert(at[1], Kind::Uint);
            bool yuv = model("yuv");

            if (model("palette")) {
                paletteAt(column, line, codes);
                return;
            }

            if (model("bayer")) {
                bayerAt(at, codes);
                return;
            }

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

        struct ChromaRows {
            int phase = 0;
            Node* corner[2] = {nullptr, nullptr};
            int reach[2] = {0, 0};
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

        ChromaRows chromaRows(Kernel& k, Node* const (&corner)[2], const int (&reach)[2], Node* const (&last)[2], Node* lane, Vector<Node*>& pending) {
            const int tile = (int)s.tile;
            ChromaRows chroma;

            for (int i = 0; i < 2; i++) {
                int radius = s.chroma[i] < 1. ? lanczosRadius - 1 : 0;

                chroma.corner[i] = g.add(g.convert(g.floor(g.sub(g.mul(g.convert(g.clamp(corner[i], 0, last[i]), Kind::Float), s.chroma[i]), s.chroma[2 + i])), Kind::Int), -radius);
                chroma.reach[i] = (int)::ceil((reach[i] - 1) * s.chroma[i]) + 3 + 2 * radius;
            }

            int positions = chroma.reach[0] * chroma.reach[1];
            int phase = k.open(positions * 2);

            k.before[phase].xchg(pending);

            for (int pass = 0; pass * tile * tile < positions; pass++) {
                Node* index = g.add(lane, pass * tile * tile);
                Node* y = g.convert(g.floor(g.mul(g.add(g.convert(index, Kind::Float), 0.5), 1. / chroma.reach[0])), Kind::Uint);
                Node* x = g.sub(index, g.mul(y, chroma.reach[0]));
                Node* across = g.convert(g.clamp(g.add(chroma.corner[0], g.convert(x, Kind::Int)), 0, s.size[2] - 1.), Kind::Uint);
                Node* line = g.convert(g.clamp(g.add(chroma.corner[1], g.convert(y, Kind::Int)), 0, s.size[3] - 1.), Kind::Uint);
                Node* slot = g.mul(index, 2);
                Node* guard = partial(index, pass, positions, tile * tile);

                for (int c = 1; c <= 2; c++) {
                    int plane = l.components[c][0];
                    Node* row = g.shr(g.add(g.u(s.planeOffset[plane]), g.mul(line, g.u(s.lineSize[plane]))), 2);

                    k.stores[phase].pushBack(Store{g.add(slot, c - 1), value(c, window(c, row, across)), guard});
                }
            }

            positions = chroma.reach[0] * reach[1];
            chroma.phase = k.open(positions * 2);

            for (int pass = 0; pass * tile * tile < positions; pass++) {
                Node* index = g.add(lane, pass * tile * tile);
                Node* y = g.convert(g.floor(g.mul(g.add(g.convert(index, Kind::Float), 0.5), 1. / chroma.reach[0])), Kind::Uint);
                Node* x = g.sub(index, g.mul(y, chroma.reach[0]));
                Taps down = chromaTaps(g.clamp(g.add(corner[1], g.convert(y, Kind::Int)), 0, last[1]), 1);
                Node* start = g.convert(g.sub(down.base, chroma.corner[1]), Kind::Uint);
                Node* slot = g.mul(index, 2);
                Node* guard = partial(index, pass, positions, tile * tile);

                for (int c = 1; c <= 2; c++) {
                    Node* sum = g.f(0.);

                    for (int t = 0; t < down.count; t++) {
                        sum = g.add(sum, g.mul(down.w[t], g.shared(g.add(g.mul(g.add(g.mul(g.add(start, t), chroma.reach[0]), x), 2), c - 1), phase)));
                    }

                    k.stores[chroma.phase].pushBack(Store{g.add(slot, c - 1), sum, guard});
                }
            }

            return chroma;
        }

        void chromaAt(const ChromaRows& chroma, Node* at, Node* y, Node* (&codes)[4]) {
            Taps across = chromaTaps(at, 0);
            Node* start = g.convert(g.sub(across.base, chroma.corner[0]), Kind::Uint);

            for (int c = 1; c <= 2; c++) {
                Node* sum = g.f(0.);

                for (int t = 0; t < across.count; t++) {
                    sum = g.add(sum, g.mul(across.w[t], g.shared(g.add(g.mul(g.add(g.mul(y, chroma.reach[0]), g.add(start, t)), 2), c - 1), chroma.phase)));
                }

                codes[c] = sum;
            }
        }

        Node* placeTile(Node* (&origin)[2]) {
            Node* placed = o.tiles == ShaderTiles::Mixed ? nullptr : g.fetch(Tiles, g.add(g.frame(3, Kind::Uint, word), g.invocation(4, 65535.)));

            origin[0] = g.origin(6);
            origin[1] = g.origin(7);

            if (placed) {
                Node* tilesAcross = g.frame(2, Kind::Uint, 65535.);
                Node* tileRow = g.convert(g.floor(g.div(g.add(g.convert(placed, Kind::Float), 0.5), g.convert(tilesAcross, Kind::Float))), Kind::Uint);

                origin[0] = g.convert(g.mul(g.sub(placed, g.mul(tileRow, tilesAcross)), (double)s.tile), Kind::Int);
                origin[1] = g.convert(g.mul(tileRow, (double)s.tile), Kind::Int);
            }

            return placed;
        }

        void kernel(Kernel& k) {
            const int tile = (int)s.tile;
            const int channels = l.alpha ? 4 : 3;
            bool sdr = !strcmp(s.output, "sdr");
            int box[2] = {boxOf(s, 0), boxOf(s, 1)};
            bool boxed = box[0] > 1 || box[1] > 1;
            bool chromaNear = boxed && model("yuv") && (s.chroma[0] < 1. || s.chroma[1] < 1.);
            bool subsampled = !boxed && model("yuv") && (s.chroma[0] < 1. || s.chroma[1] < 1.);
            bool portions = shrinking(s) && !blockRows(s, false);
            const int rows = blockRows(s, portions);
            bool sigmoid = sdr && s.size[0] <= s.target[0] && s.size[1] <= s.target[1];
            double ratio[2];
            Node* local[2] = {g.invocation(2, tile - 1), g.invocation(3, tile - 1)};
            Node* first[2];
            Node* last[2];
            Node* light[4] = {g.f(0.), g.f(0.), g.f(0.), g.f(0.)};
            Node* pixel[2];
            Vector<Node*> pending;

            if (!rows) {
                fail(StringView(u8"a video layer does not fit its tile"));
            }

            Node* origin[2];
            Node* placed = placeTile(origin);

            for (int i = 0; i < 2; i++) {
                ratio[i] = ratioOf(s, i);
                first[i] = g.min(g.sub(origin[i], (double)s.origin[i]), s.target[i] - 1.);
                last[i] = g.i(::ceil((double)s.size[i] / box[i]) - 1.);
                pixel[i] = g.clamp(g.add(first[i], g.convert(local[i], Kind::Int)), 0., s.target[i] - 1.);
            }

            Node* lane = g.add(g.mul(local[1], tile), local[0]);
            Node* column = g.convert(g.clamp(g.sub(pixel[0], first[0]), 0., tile - 1.), Kind::Uint);
            int blocks = portions ? 1 : tile / rows;
            Node* tileCorner = nullptr;
            Node* portion = nullptr;
            Axis upright{};

            if (portions) {
                upright = axis(g.sub(g.mul(g.add(g.convert(pixel[1], Kind::Float), 0.5), ratio[1]), 0.5), 1. / ratio[1]);
                tileCorner = windowStart(first[1], ratio[1]);
                k.loop.count = (footprintOf(s) + rows - 1) / rows;
                k.loop.carried = channels;
                k.loop.first = k.count;
                portion = g.portion(k.loop.count);

                for (int c = 0; c < channels; c++) {
                    k.loop.start[c] = g.f(0.);
                    light[c] = g.carried(c);
                }
            }

            for (int block = 0; block < blocks; block++) {
                int top = block * rows;
                Node* opening = g.clamp(g.add(first[1], top), 0., s.target[1] - 1.);
                Node* head[2] = {first[0], rows == tile ? first[1] : opening};
                int reach[2] = {reachOf(ratio[0], tile), portions ? rows : reachOf(ratio[1], rows)};
                Node* corner[2];
                ChromaRows chroma;
                int lightPhase = 0;

                corner[0] = windowStart(head[0], ratio[0]);
                corner[1] = portions ? g.add(tileCorner, g.mul(portion, rows)) : windowStart(head[1], ratio[1]);

                if (subsampled) {
                    chroma = chromaRows(k, corner, reach, last, lane, pending);
                }

                auto lightAt = [&](Node* x, Node* y, Node*(&out)[4]) {
                    Node* at[2] = {g.clamp(g.add(corner[0], g.convert(x, Kind::Int)), 0, last[0]), g.clamp(g.add(corner[1], g.convert(y, Kind::Int)), 0, last[1])};
                    Node* codes[4];
                    Node* signal[3];
                    Node* rgb[3];

                    if (boxed) {
                        Node* sum[4] = {g.f(0.), g.f(0.), g.f(0.), g.f(0.)};

                        for (int j = 0; j < box[1]; j++) {
                            for (int i = 0; i < box[0]; i++) {
                                Node* source[2] = {g.min(g.add(g.mul(at[0], box[0]), i), s.size[0] - 1.), g.min(g.add(g.mul(at[1], box[1]), j), s.size[1] - 1.)};
                                Node* cover;

                                decodeAt(source, codes);

                                if (chromaNear) {
                                    nearChroma(source, codes);
                                }

                                signalOf(codes, signal, cover);
                                lightOf(signal, rgb);

                                for (int c = 0; c < 3; c++) {
                                    sum[c] = g.add(sum[c], channels == 4 ? g.mul(rgb[c], cover) : rgb[c]);
                                }

                                sum[3] = g.add(sum[3], cover);
                            }
                        }

                        double count = (double)box[0] * box[1];

                        out[3] = g.mul(sum[3], 1. / count);

                        for (int c = 0; c < 3; c++) {
                            out[c] = channels == 4 ? g.select(g.lt(0., sum[3]), g.div(sum[c], sum[3]), g.f(0.)) : g.mul(sum[c], 1. / count);
                        }

                        return;
                    }

                    decodeAt(at, codes);

                    if (subsampled) {
                        chromaAt(chroma, at[0], y, codes);
                    }

                    signalOf(codes, signal, out[3]);
                    lightOf(signal, rgb);

                    for (int c = 0; c < 3; c++) {
                        out[c] = rgb[c];
                    }
                };

                int positions = reach[0] * reach[1];

                lightPhase = k.open(positions * channels);

                if (!subsampled) {
                    k.before[lightPhase].xchg(pending);
                }

                for (int pass = 0; pass * tile * tile < positions; pass++) {
                    Node* index = g.add(lane, pass * tile * tile);
                    Node* y = g.convert(g.floor(g.mul(g.add(g.convert(index, Kind::Float), 0.5), 1. / reach[0])), Kind::Uint);
                    Node* x = g.sub(index, g.mul(y, reach[0]));
                    Node* slot = g.mul(index, channels);
                    Node* guard = partial(index, pass, positions, tile * tile);
                    Node* decoded[4];

                    lightAt(x, y, decoded);

                    for (int c = 0; c < channels; c++) {
                        Node* value = c < 3 && sigmoid ? sigmoidize(decoded[c]) : decoded[c];

                        k.stores[lightPhase].pushBack(Store{g.add(slot, c), c < 3 && channels == 4 ? g.mul(value, decoded[3]) : value, guard});
                    }
                }

                Node* across = g.convert(g.add(head[0], g.convert(local[0], Kind::Int)), Kind::Float);
                Axis horizontal = axis(g.sub(g.mul(g.add(across, 0.5), ratio[0]), 0.5), 1. / ratio[0]);
                Node* start = g.convert(g.sub(horizontal.origin, corner[0]), Kind::Uint);
                int filterPhase = k.open(reach[1] * tile * channels);

                for (int pass = 0; pass * tile < reach[1]; pass++) {
                    Node* row = g.add(local[1], pass * tile);
                    Node* base = g.add(g.mul(row, reach[0]), start);
                    Node* slot = g.mul(g.add(g.mul(row, tile), local[0]), channels);
                    Node* guard = partial(row, pass, reach[1], tile);

                    for (int c = 0; c < channels; c++) {
                        Node* sum = g.f(0.);

                        for (int t = 0; t < horizontal.taps; t++) {
                            sum = g.add(sum, g.mul(horizontal.w[t], g.shared(g.add(g.mul(g.add(base, t), channels), c), lightPhase)));
                        }

                        k.stores[filterPhase].pushBack(Store{g.add(slot, c), sum, guard});
                    }
                }

                if (portions) {
                    for (int r = 0; r < reach[1]; r++) {
                        Node* w = weightAt(upright, g.convert(g.add(corner[1], r), Kind::Float));

                        for (int c = 0; c < channels; c++) {
                            light[c] = g.add(light[c], g.mul(w, g.shared(g.add(g.mul(g.add(column, r * tile), channels), c), filterPhase)));
                        }
                    }

                    for (int c = 0; c < channels; c++) {
                        k.loop.next[c] = light[c];
                        light[c] = g.carried(c);
                    }

                    k.loop.last = k.count - 1;

                    continue;
                }

                Node* row = rows == tile ? pixel[1] : g.clamp(pixel[1], head[1], g.min(g.add(head[1], rows - 1), s.target[1] - 1.));
                Axis vertical = axis(g.sub(g.mul(g.add(g.convert(row, Kind::Float), 0.5), ratio[1]), 0.5), 1. / ratio[1]);
                Node* down = g.convert(g.sub(vertical.origin, corner[1]), Kind::Uint);
                Node* strip = rows == tile ? nullptr : g.lt(g.sub(local[1], top), rows);

                for (int c = 0; c < channels; c++) {
                    Node* total = g.f(0.);

                    for (int t = 0; t < vertical.taps; t++) {
                        total = g.add(total, g.mul(vertical.w[t], g.shared(g.add(g.mul(g.add(g.mul(g.add(down, t), tile), column), channels), c), filterPhase)));
                    }

                    light[c] = strip && top ? g.select(strip, total, light[c]) : total;
                    pending.pushBack(light[c]);
                }
            }

            Node* alpha = channels == 4 ? g.clamp(light[3], 0., 1.) : g.f(1.);
            Node* shown[3];

            for (int c = 0; c < 3; c++) {
                Node* straight = channels == 4 ? g.select(g.lt(0., light[3]), g.div(light[c], light[3]), g.f(0.)) : light[c];

                shown[c] = sigmoid ? unsigmoidize(straight) : straight;
            }

            Node* layer[4];

            outputOf(shown, layer);
            layer[3] = alpha;
            compose(k, placed, origin, local, layer);
        }

        Node* fract(Node* x) {
            return g.sub(x, g.floor(x));
        }

        void pixels(Kernel& k, Node* const (&origin)[2], Node* const (&local)[2]) {
            for (int i = 0; i < 2; i++) {
                k.pixel[i] = g.add(origin[i], g.convert(local[i], Kind::Int));

                Node* within = g.convert(g.lt(k.pixel[i], g.frame(i, Kind::Int, 65535.)), Kind::Float);

                k.drawn = i ? g.mul(k.drawn, within) : within;
            }

            k.drawn = g.lt(0.5, k.drawn);
        }

        Node* dither(const Kernel& k) {
            Node* at[2] = {g.add(g.convert(k.pixel[0], Kind::Float), 0.5), g.add(g.convert(k.pixel[1], Kind::Float), 0.5)};

            return g.sub(fract(g.mul(fract(g.add(g.mul(at[0], 0.06711056), g.mul(at[1], 0.00583715))), 52.9829189)), 0.5);
        }

        bool srgbCurve() const {
            for (const VideoTransfer& known : videoTransfers) {
                if (known.code != transferSrgb) {
                    continue;
                }

                for (int i = 0; i < 11; i++) {
                    if (::fabs(s.curve[i] - known.eotf[i]) > 1e-9 * (1. + ::fabs(known.eotf[i]))) {
                        return false;
                    }
                }

                return true;
            }

            return false;
        }

        bool exactCodes() const {
            if (l.floating || model("palette") || model("bayer") || !strcmp(s.system, "cl")) {
                return false;
            }

            for (int r = 0; r < 3; r++) {
                int nonzero = 0;

                for (int c = 0; c < 3; c++) {
                    if (s.decode[r][c] == 0.) {
                        continue;
                    }

                    if (::fabs(s.decode[r][c] * 255. - 1.) > 1e-9 || l.components[c][4] != 8) {
                        return false;
                    }

                    nonzero++;
                }

                if (nonzero != 1 || s.bias[r] != 0.) {
                    return false;
                }
            }

            return true;
        }

        bool unchanged(Kernel& k, Node* const (&signal)[3], Node* alpha, Node* const (&origin)[2], Node* const (&local)[2]) {
            bool power = s.curve[10] < 0. && s.curve[0] == 1. && s.curve[2] == 1. && s.curve[3] == 0. && s.curve[4] == 0.;
            bool srgb = srgbCurve();

            if (o.tiles != ShaderTiles::Inside || o.output != ShaderOutput::Srgb || alpha->op != Op::Const || alpha->value != 1.) {
                return false;
            }

            if (strcmp(s.output, "sdr") || strcmp(s.conversion, "same") || !strcmp(s.system, "ictcp") || !curved() || (!srgb && !power)) {
                return false;
            }

            bool exact = srgb && exactCodes();

            pixels(k, origin, local);

            Node* noise = exact ? nullptr : g.mul(dither(k), 1. / 255.);

            for (int c = 0; c < 3; c++) {
                Node* v = g.clamp(signal[c], 0., 1.);
                double gamma = s.curve[1];
                Node* coded = srgb ? v : g.select(g.le(v, ::pow(0.0031308, 1. / gamma)), g.mul(g.pow(v, gamma), 12.92), g.sub(g.mul(g.pow(v, gamma / 2.4), 1.055), 0.055));

                k.encoded[c] = noise ? g.add(coded, noise) : coded;
            }

            k.encoded[3] = g.f(1.);

            return true;
        }

        void native(Kernel& k) {
            const int tile = (int)s.tile;
            bool subsampled = model("yuv") && (s.chroma[0] < 1. || s.chroma[1] < 1.);
            Node* local[2] = {g.invocation(2, tile - 1), g.invocation(3, tile - 1)};
            Node* origin[2];
            Node* placed = placeTile(origin);
            Node* first[2];
            Node* last[2];
            Node* at[2];
            Node* codes[4];
            Node* signal[3];
            Node* alpha;

            for (int i = 0; i < 2; i++) {
                first[i] = g.sub(origin[i], (double)s.origin[i]);
                last[i] = g.i(s.size[i] - 1.);
                at[i] = g.clamp(g.add(first[i], g.convert(local[i], Kind::Int)), 0, last[i]);
            }

            decodeAt(at, codes);

            if (subsampled) {
                Vector<Node*> pending;
                const int reach[2] = {tile, tile};
                ChromaRows chroma = chromaRows(k, first, reach, last, g.add(g.mul(local[1], tile), local[0]), pending);

                chromaAt(chroma, at[0], local[1], codes);
            }

            signalOf(codes, signal, alpha);

            if (unchanged(k, signal, alpha, origin, local)) {
                return;
            }

            Node* light[3];
            Node* layer[4];

            lightOf(signal, light);
            outputOf(light, layer);
            layer[3] = alpha;
            compose(k, placed, origin, local, layer);
        }

        void compose(Kernel& k, Node* tile, Node* const (&origin)[2], Node* const (&local)[2], Node* const (&layer)[4]) {
            static const double widen[3][3] = {{0.627404, 0.329283, 0.043313}, {0.069097, 0.919540, 0.011362}, {0.016391, 0.088013, 0.895595}};
            static const double narrow[3][3] = {{1.660491, -0.587641, -0.072850}, {-0.124550, 1.132900, -0.008349}, {-0.018151, -0.100579, 1.118730}};
            bool layerWide = strcmp(s.output, "sdr");
            bool wide = o.output == ShaderOutput::Pq || o.output == ShaderOutput::WideLinear;

            for (int c = 0; c < 3; c++) {
                Node* toWide = g.f(0.);
                Node* toNarrow = g.f(0.);

                for (int j = 0; j < 3; j++) {
                    toWide = g.add(toWide, g.mul(layer[j], widen[c][j]));
                    toNarrow = g.add(toNarrow, g.mul(layer[j], narrow[c][j]));
                }

                k.shown[c] = layerWide ? (wide ? layer[c] : g.clamp(toNarrow, 0., 1.)) : (wide ? toWide : layer[c]);
            }

            k.shown[3] = layer[3];

            if (o.tiles == ShaderTiles::Mixed) {
                return;
            }

            Node* header = g.mul(tile, headerWords);
            Node* inside = g.f(1.);

            pixels(k, origin, local);

            if (o.tiles == ShaderTiles::Edge) {
                Node* video = g.band(g.fetch(List, g.fetch(Headers, g.add(header, 1))), (double)~composeFill);

                for (int i = 0; i < 2; i++) {
                    Node* low = g.convert(g.fetch(Ops, g.add(g.mul(video, opWords), 4 + i)), Kind::Int);
                    Node* high = g.convert(g.fetch(Ops, g.add(g.mul(video, opWords), 6 + i)), Kind::Int);

                    inside = g.mul(inside, g.mul(g.convert(g.le(low, k.pixel[i]), Kind::Float), g.convert(g.lt(k.pixel[i], high), Kind::Float)));
                }
            }

            Node* cover = g.mul(layer[3], inside);
            Node* keep = g.sub(1., cover);
            Node* acc[4];

            for (int c = 0; c < 4; c++) {
                Node* color = g.bitsFloat(g.fetch(Headers, g.add(header, 4 + c)));

                acc[c] = g.add(c < 3 ? g.mul(k.shown[c], cover) : cover, g.mul(color, keep));
            }

            Node* noise = dither(k);
            Node* visible = g.lt(0., acc[3]);

            for (int c = 0; c < 3; c++) {
                if (o.output == ShaderOutput::Srgb) {
                    Node* v = g.clamp(acc[c], 0., 1.);

                    k.encoded[c] = g.add(g.select(g.le(v, 0.0031308), g.mul(v, 12.92), g.sub(g.mul(g.pow(v, 1. / 2.4), 1.055), 0.055)), g.mul(noise, 1. / 255.));
                } else if (o.output == ShaderOutput::Pq) {
                    Node* y = g.pow(g.clamp(g.mul(g.mul(acc[c], g.frame(4, Kind::Float, 100000.)), 1. / 10000.), 0., 1.), 2610. / 16384.);

                    k.encoded[c] = g.add(g.pow(g.div(g.add(g.mul(y, 18.8515625), 0.8359375), g.add(g.mul(y, 18.6875), 1.)), 2523. / 32.), g.mul(noise, 1. / 1023.));
                } else {
                    k.encoded[c] = g.select(visible, g.div(acc[c], acc[3]), g.f(0.));
                }
            }

            k.encoded[3] = o.output == ShaderOutput::Srgb || o.output == ShaderOutput::Pq ? g.f(1.) : g.select(visible, acc[3], g.f(0.));
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

    static void hoist(Vector<Node*>& nodes, Vector<Node*>& inside) {
        Vector<Node*> uniform;

        for (Node* node : nodes) {
            (node->uniform ? uniform : inside).pushBack(node);
        }

        nodes.xchg(uniform);
    }

    static void forget(const Vector<Node*>& nodes) {
        for (Node* node : nodes) {
            if (node->op != Op::Const && (node->op != Op::Input || (node->value >= 8. && node->value < 32.))) {
                node->mark = 0;
            }
        }
    }

    static void steadyFirst(const Kernel& k, Vector<Node*>& steady, Vector<Node*>& varying) {
        const Loop& loop = k.loop;
        Vector<Node*> roots;
        Vector<Node*> nodes;

        for (int phase = loop.first; phase <= loop.last; phase++) {
            roots.append(k.before[phase].data(), k.before[phase].length());

            for (const Store& store : k.stores[phase]) {
                roots.pushBack(store.index);
                roots.pushBack(store.value);

                if (store.guard) {
                    roots.pushBack(store.guard);
                }
            }
        }

        roots.append(loop.start, loop.carried);
        roots.append(loop.next, loop.carried);
        order(roots.data(), roots.length(), nodes);

        for (Node* node : nodes) {
            (node->varies ? varying : steady).pushBack(node);
        }

        forget(varying);
    }
}

#if defined(__APPLE__)
namespace {
    static const char* typeName(Kind kind) {
        return kind == Kind::Float ? "float" : kind == Kind::Uint ? "uint" : kind == Kind::Int ? "int" : "bool";
    }

    static void operand(StringBuilder& out, const Node* node) {
        const char* inputs[8] = {"uv.x", "uv.y", "local.x", "local.y", "group.x", "group.y", "origin.x", "origin.y"};

        if (node->op == Op::Input && node->value >= 32.) {
            const char* frames[5] = {"frame.size.x", "frame.size.y", "frame.tilesX", "frame.first", "frame.white"};

            out << StringView(frames[(int)node->value - 32]);
        } else if (node->op == Op::Input && node->value >= 16.) {
            out << StringView(u8"carried") << (u64)(node->value - 16.);
        } else if (node->op == Op::Input && node->value >= 8.) {
            out << StringView(u8"portion");
        } else if (node->op == Op::Input) {
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
                const char* buffers[5] = {"words[", "headers[", "list[", "ops[", "tiles["};

                out << StringView(buffers[(int)node->value]);
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

    static StringView copied(ObjPool& pool, StringBuilder& out) {
        u8* bytes = (u8*)pool.allocate(out.used());

        memcpy(bytes, out.data(), out.used());

        return StringView(bytes, out.used());
    }

    static void phase(StringBuilder& out, const Kernel& k, int phase, u32& next) {
        const Vector<Store>& stores = k.stores[phase];

        if (!k.before[phase].empty()) {
            Vector<Node*> nodes;

            order(k.before[phase].data(), k.before[phase].length(), nodes);
            statements(out, nodes, next);
        }

        if (stores.empty()) {
            return;
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

            Vector<Node*> condition;

            if (guard) {
                order(&guard, 1, condition);
            }

            order(roots.data(), roots.length(), nodes);

            if (guard) {
                Vector<Node*> inside;

                statements(out, condition, next);
                hoist(nodes, inside);
                statements(out, nodes, next);
                nodes.xchg(inside);
                out << StringView(u8"    if (");
                operand(out, guard);
                out << StringView(u8") {\n");
            }

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

    static void loop(StringBuilder& out, const Kernel& k, u32& next) {
        const Loop& l = k.loop;
        Vector<Node*> steady;
        Vector<Node*> varying;
        Vector<Node*> updates;

        steadyFirst(k, steady, varying);
        statements(out, steady, next);

        for (int c = 0; c < l.carried; c++) {
            out << StringView(u8"    float carried") << (u64)c << StringView(u8" = ");
            operand(out, l.start[c]);
            out << StringView(u8";\n");
        }

        out << StringView(u8"    for (int portion = 0; portion < ") << (u64)l.count << StringView(u8"; portion++) {\n");

        for (int p = l.first; p <= l.last; p++) {
            phase(out, k, p, next);
        }

        order(l.next, l.carried, updates);
        statements(out, updates, next);

        for (int c = 0; c < l.carried; c++) {
            out << StringView(u8"    carried") << (u64)c << StringView(u8" = ");
            operand(out, l.next[c]);
            out << StringView(u8";\n");
        }

        out << StringView(u8"    }\n");
        forget(varying);
        forget(updates);
    }

    static void phases(StringBuilder& out, const Kernel& k, u32& next) {
        for (int p = 0; p < k.count; p++) {
            if (p == k.loop.first) {
                loop(out, k, next);
                p = k.loop.last;
                continue;
            }

            phase(out, k, p, next);
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

    static StringView mslLayer(ObjPool& pool, const Kernel& k) {
        StringBuilder out;
        u32 next = 3;

        out << StringView(u8"#define LAYER_SHARED");

        for (int i = 0; i < kernelBuffers; i++) {
            out << StringView(u8" threadgroup float ") << StringView(sharedNames[i]) << StringView(u8"[") << (u64)(k.buffer(i) ? k.buffer(i) : 1) << StringView(u8"];");
        }

        out << StringView(u8"\n#define LAYER_CALL(local, origin, words) layer(local, origin, words, sharedA, sharedB)\n");
        out << StringView(u8"float4 layer(uint2 local, int2 origin, const device uint* words, threadgroup float* sharedA, threadgroup float* sharedB) {\n");

        phases(out, k, next);

        Vector<Node*> nodes;

        order(k.shown, 4, nodes);
        statements(out, nodes, next);
        out << StringView(u8"    return ");
        finish(out, k.shown, ";\n}\n");

        return copied(pool, out);
    }

    static StringView mslKernel(ObjPool& pool, const Kernel& k) {
        StringBuilder out;
        u32 next = 3;
        Node* roots[7] = {k.encoded[0], k.encoded[1], k.encoded[2], k.encoded[3], k.pixel[0], k.pixel[1], k.drawn};

        out << StringView(u8"struct Frame {\n    int2 size;\n    uint tilesX;\n    uint first;\n    float white;\n};\n");
        out << StringView(u8"kernel void compose(device const uint* headers [[buffer(0)]], device const uint* list [[buffer(1)]], device const uint* ops [[buffer(2)]], device const uint* tiles [[buffer(4)]], constant Frame& frame [[buffer(5)]], const device uint* words [[buffer(7)]], texture2d<float, access::write> target [[texture(0)]], uint3 group [[threadgroup_position_in_grid]], uint3 local [[thread_position_in_threadgroup]]) {\n");

        for (int i = 0; i < kernelBuffers; i++) {
            out << StringView(u8"    threadgroup float ") << StringView(sharedNames[i]) << StringView(u8"[") << (u64)(k.buffer(i) ? k.buffer(i) : 1) << StringView(u8"];\n");
        }

        phases(out, k, next);

        Vector<Node*> nodes;

        order(roots, 7, nodes);
        statements(out, nodes, next);
        out << StringView(u8"    if (");
        operand(out, k.drawn);
        out << StringView(u8") {\n        target.write(");
        finish(out, k.encoded, ", uint2(uint(");
        operand(out, k.pixel[0]);
        out << StringView(u8"), uint(");
        operand(out, k.pixel[1]);
        out << StringView(u8")));\n    }\n}\n");

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
        u32 inputs[48] = {};
        u32 variables[5] = {};
        u32 buffers[5] = {bytes, s.fresh(), s.fresh(), s.fresh(), s.fresh()};
        u32 zero = 0;
        u32 one = 0;
        u32 none = 0;
        u32 unit = 0;

        explicit Emitter(Graph& g) {
            Node* extras[4] = {g.i(0.), g.f(1.), g.f(0.), g.i(1.)};
            Vector<Node*> nodes;

            order(extras, 4, nodes);
            emit(nodes);
            zero = extras[0]->id;
            one = extras[1]->id;
            none = extras[2]->id;
            unit = extras[3]->id;
        }

        u32 type(Kind kind) const {
            return kind == Kind::Float ? typeFloat : kind == Kind::Uint ? typeUint : kind == Kind::Int ? typeInt : typeBool;
        }

        void emit(const Vector<Node*>& nodes) {
            for (Node* node : nodes) {
                if (node->op == Op::Input && node->value >= 8. && node->value < 32.) {
                    node->id = s.fresh();
                    Spirv::op(s.body, 61, type(node->kind), node->id, variables[node->value == 8. ? 0 : (int)node->value - 15]);
                    continue;
                }

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

                        Spirv::op(s.body, 65, storageWord, pointer, buffers[(int)node->value], zero, a);
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

        void phase(const Kernel& k, int phase, u32 scope, u32 semantics) {
            const Vector<Store>& stores = k.stores[phase];

            if (!k.before[phase].empty()) {
                Vector<Node*> nodes;

                order(k.before[phase].data(), k.before[phase].length(), nodes);
                emit(nodes);
            }

            if (stores.empty()) {
                return;
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

                Vector<Node*> condition;

                if (guard) {
                    order(&guard, 1, condition);
                }

                order(roots.data(), roots.length(), nodes);

                if (guard) {
                    Vector<Node*> inside;
                    u32 label = s.fresh();

                    merge = s.fresh();
                    emit(condition);
                    hoist(nodes, inside);
                    emit(nodes);
                    nodes.xchg(inside);
                    Spirv::op(s.body, 247, merge, 0);
                    Spirv::op(s.body, 250, guard->id, label, merge);
                    Spirv::op(s.body, 248, label);
                }

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

        void loop(const Kernel& k, u32 scope, u32 semantics) {
            const Loop& l = k.loop;
            Vector<Node*> steady;
            Vector<Node*> varying;
            Vector<Node*> updates;
            u32 count = s.fresh();
            u32 header = s.fresh();
            u32 body = s.fresh();
            u32 next = s.fresh();
            u32 merge = s.fresh();
            u32 at = s.fresh();
            u32 more = s.fresh();
            u32 was = s.fresh();
            u32 now = s.fresh();

            steadyFirst(k, steady, varying);
            emit(steady);
            Spirv::op(s.globals, 43, typeInt, count, (u32)l.count);
            Spirv::op(s.body, 62, variables[0], zero);

            for (int c = 0; c < l.carried; c++) {
                Spirv::op(s.body, 62, variables[1 + c], l.start[c]->id);
            }

            Spirv::op(s.body, 249, header);
            Spirv::op(s.body, 248, header);
            Spirv::op(s.body, 61, typeInt, at, variables[0]);
            Spirv::op(s.body, 177, typeBool, more, at, count);
            Spirv::op(s.body, 246, merge, next, 0);
            Spirv::op(s.body, 250, more, body, merge);
            Spirv::op(s.body, 248, body);

            for (int p = l.first; p <= l.last; p++) {
                phase(k, p, scope, semantics);
            }

            order(l.next, l.carried, updates);
            emit(updates);

            for (int c = 0; c < l.carried; c++) {
                Spirv::op(s.body, 62, variables[1 + c], l.next[c]->id);
            }

            Spirv::op(s.body, 249, next);
            Spirv::op(s.body, 248, next);
            Spirv::op(s.body, 61, typeInt, was, variables[0]);
            Spirv::op(s.body, 128, typeInt, now, was, unit);
            Spirv::op(s.body, 62, variables[0], now);
            Spirv::op(s.body, 249, header);
            Spirv::op(s.body, 248, merge);
            forget(varying);
            forget(updates);
        }

        void phases(const Kernel& k, u32 scope, u32 semantics) {
            for (int p = 0; p < k.count; p++) {
                if (p == k.loop.first) {
                    loop(k, scope, semantics);
                    p = k.loop.last;
                    continue;
                }

                phase(k, p, scope, semantics);
            }
        }

        void common(Vector<u32>& module, bool kernel) {
            Spirv::op(module, 71, typeWords, 6, 4);
            Spirv::op(module, 72, typeBytes, 0, 24);
            Spirv::op(module, 72, typeBytes, 0, 35, 0);
            Spirv::op(module, 71, typeBytes, 2);
            Spirv::op(module, 71, bytes, 34, 1);
            Spirv::op(module, 71, bytes, 33, 0);

            const u32 bindings[5] = {0, 0, 1, 2, 4};

            for (int i = 1; kernel && i < 5; i++) {
                Spirv::op(module, 71, buffers[i], 34, 0);
                Spirv::op(module, 71, buffers[i], 33, bindings[i]);
                Spirv::op(module, 71, buffers[i], 24);
            }
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

    static StringView spirvLayer(ObjPool& pool, Graph& g, const Kernel& k) {
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
        u32 pointerFloat = s.fresh();
        u32 pointerInt = s.fresh();

        const int given[4] = {2, 3, 6, 7};

        for (int which : given) {
            e.inputs[which] = s.fresh();
        }

        Spirv::op(s.body, 54, e.typeVec4, e.main, 0, typeLayer);
        Spirv::op(s.body, 55, pointerUvec2, local);
        Spirv::op(s.body, 55, pointerIvec2, origin);
        Spirv::op(s.body, 248, e.label);

        for (int i = 0; k.loop.count && i <= k.loop.carried; i++) {
            e.variables[i] = s.fresh();
            Spirv::op(s.body, 59, i ? pointerFloat : pointerInt, e.variables[i], 7);
        }

        Spirv::op(s.body, 61, typeUvec2, loaded[0], local);
        Spirv::op(s.body, 81, e.typeUint, e.inputs[2], loaded[0], 0);
        Spirv::op(s.body, 81, e.typeUint, e.inputs[3], loaded[0], 1);
        Spirv::op(s.body, 61, typeIvec2, loaded[1], origin);
        Spirv::op(s.body, 81, e.typeInt, e.inputs[6], loaded[1], 0);
        Spirv::op(s.body, 81, e.typeInt, e.inputs[7], loaded[1], 1);

        e.phases(k, scope, semantics);

        Vector<Node*> nodes;

        order(k.shown, 4, nodes);
        e.emit(nodes);
        Spirv::op(s.body, 80, e.typeVec4, color, k.shown[0]->id, k.shown[1]->id, k.shown[2]->id, k.shown[3]->id);
        Spirv::op(s.body, 254, color);
        Spirv::op(s.body, 56);

        Vector<u32> module;

        e.head(module, false);
        e.common(module, false);
        e.types(module);

        if (k.loop.count) {
            Spirv::op(module, 32, pointerFloat, 7, e.typeFloat);
            Spirv::op(module, 32, pointerInt, 7, e.typeInt);
        }

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
        u32 typeFrame = s.fresh();
        u32 framePointer = s.fresh();
        u32 frameInt = s.fresh();
        u32 frameUint = s.fresh();
        u32 frameFloat = s.fresh();
        u32 frame = s.fresh();
        u32 local = s.fresh();
        u32 group = s.fresh();
        u32 lengths[kernelBuffers] = {s.fresh(), s.fresh()};
        u32 arrayTypes[kernelBuffers] = {s.fresh(), s.fresh()};
        u32 arrayPointers[kernelBuffers] = {s.fresh(), s.fresh()};
        u32 scope = s.fresh();
        u32 semantics = s.fresh();
        u32 pointerFloat = s.fresh();
        u32 pointerInt = s.fresh();
        u32 ids[2] = {s.fresh(), s.fresh()};
        Node* indices[4] = {g.i(0.), g.i(1.), g.i(2.), g.i(3.)};
        Vector<Node*> constants;

        order(indices, 4, constants);
        e.emit(constants);

        const int given[9] = {2, 3, 4, 5, 32, 33, 34, 35, 36};

        for (int which : given) {
            e.inputs[which] = s.fresh();
        }

        Spirv::op(s.body, 54, e.typeVoid, e.main, 0, e.typeFunction);
        Spirv::op(s.body, 248, e.label);

        for (int i = 0; k.loop.count && i <= k.loop.carried; i++) {
            e.variables[i] = s.fresh();
            Spirv::op(s.body, 59, i ? pointerFloat : pointerInt, e.variables[i], 7);
        }

        Spirv::op(s.body, 61, typeUvec3, ids[0], local);
        Spirv::op(s.body, 81, e.typeUint, e.inputs[2], ids[0], 0);
        Spirv::op(s.body, 81, e.typeUint, e.inputs[3], ids[0], 1);
        Spirv::op(s.body, 61, typeUvec3, ids[1], group);
        Spirv::op(s.body, 81, e.typeUint, e.inputs[4], ids[1], 0);
        Spirv::op(s.body, 81, e.typeUint, e.inputs[5], ids[1], 1);

        const u32 members[5][2] = {{0, 0}, {0, 1}, {1, 0}, {2, 0}, {3, 0}};
        const u32 kinds[5] = {e.typeInt, e.typeInt, e.typeUint, e.typeUint, e.typeFloat};
        const u32 pointers[5] = {frameInt, frameInt, frameUint, frameUint, frameFloat};

        for (int i = 0; i < 5; i++) {
            u32 pointer = s.fresh();

            if (i < 2) {
                Spirv::op(s.body, 65, pointers[i], pointer, frame, indices[members[i][0]]->id, indices[members[i][1]]->id);
            } else {
                Spirv::op(s.body, 65, pointers[i], pointer, frame, indices[members[i][0]]->id);
            }

            Spirv::op(s.body, 61, kinds[i], e.inputs[32 + i], pointer);
        }

        e.phases(k, scope, semantics);

        Node* roots[7] = {k.encoded[0], k.encoded[1], k.encoded[2], k.encoded[3], k.pixel[0], k.pixel[1], k.drawn};
        Vector<Node*> nodes;
        u32 merge = s.fresh();
        u32 store = s.fresh();
        u32 coordinate = s.fresh();
        u32 loaded = s.fresh();
        u32 texel = s.fresh();

        order(roots, 7, nodes);
        e.emit(nodes);
        Spirv::op(s.body, 247, merge, 0);
        Spirv::op(s.body, 250, k.drawn->id, store, merge);
        Spirv::op(s.body, 248, store);
        Spirv::op(s.body, 80, typeIvec2, coordinate, k.pixel[0]->id, k.pixel[1]->id);
        Spirv::op(s.body, 61, typeImage, loaded, image);
        Spirv::op(s.body, 80, e.typeVec4, texel, k.encoded[0]->id, k.encoded[1]->id, k.encoded[2]->id, k.encoded[3]->id);
        Spirv::op(s.body, 99, loaded, coordinate, texel);
        Spirv::op(s.body, 249, merge);
        Spirv::op(s.body, 248, merge);
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
        e.common(module, true);
        Spirv::op(module, 71, image, 34, 0);
        Spirv::op(module, 71, image, 33, 5);
        Spirv::op(module, 71, image, 25);
        Spirv::op(module, 71, typeFrame, 2);
        Spirv::op(module, 72, typeFrame, 0, 35, 0);
        Spirv::op(module, 72, typeFrame, 1, 35, 8);
        Spirv::op(module, 72, typeFrame, 2, 35, 12);
        Spirv::op(module, 72, typeFrame, 3, 35, 16);
        e.types(module);
        Spirv::op(module, 23, typeUvec3, e.typeUint, 3);
        Spirv::op(module, 23, typeIvec2, e.typeInt, 2);
        Spirv::op(module, 32, inputUvec3, 1, typeUvec3);
        Spirv::op(module, 25, typeImage, e.typeFloat, 1, 0, 0, 0, 2, 0);
        Spirv::op(module, 32, imagePointer, 0, typeImage);
        Spirv::op(module, 30, typeFrame, typeIvec2, e.typeUint, e.typeUint, e.typeFloat);
        Spirv::op(module, 32, framePointer, 9, typeFrame);
        Spirv::op(module, 32, frameInt, 9, e.typeInt);
        Spirv::op(module, 32, frameUint, 9, e.typeUint);
        Spirv::op(module, 32, frameFloat, 9, e.typeFloat);

        if (k.loop.count) {
            Spirv::op(module, 32, pointerFloat, 7, e.typeFloat);
            Spirv::op(module, 32, pointerInt, 7, e.typeInt);
        }

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

        for (int i = 0; i < 5; i++) {
            Spirv::op(module, 59, e.storageBytes, e.buffers[i], 12);
        }

        Spirv::op(module, 59, imagePointer, image, 0);
        Spirv::op(module, 59, framePointer, frame, 9);

        for (int i = 0; i < kernelBuffers; i++) {
            if (k.buffer(i)) {
                Spirv::op(module, 59, arrayPointers[i], e.arrays[i], 4);
            }
        }

        return e.finish(pool, module);
    }
}
#endif

StringView compile(ObjPool& pool, const VideoShader& shader, const ShaderOptions& options) {
    Graph g(pool);
    Video video{g, shader, *shader.layout, options};
    Kernel layer;

    if (!shader.tile || shader.tile > 32) {
        fail(StringView(u8"a video layer has an invalid tile"));
    }

    layer.tile = shader.tile;

    if (shader.size[0] == shader.target[0] && shader.size[1] == shader.target[1]) {
        video.native(layer);
    } else {
        video.kernel(layer);
    }

#if defined(__APPLE__)
    return options.tiles == ShaderTiles::Mixed ? mslLayer(pool, layer) : mslKernel(pool, layer);
#else
    return options.tiles == ShaderTiles::Mixed ? spirvLayer(pool, g, layer) : spirvKernel(pool, g, layer);
#endif
}
