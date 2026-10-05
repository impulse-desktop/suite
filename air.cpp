#include "air.h"

#include "error.h"

#include <std/str/view.h>
#include <std/mem/obj_pool.h>

#include <string.h>

using namespace stl;

namespace {
    const u32 roundConstants[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    static u32 rotated(u32 x, int n) {
        return (x >> n) | (x << (32 - n));
    }

    static void compress(u32 (&h)[8], const u8* block) {
        u32 w[64];

        for (int i = 0; i < 16; i++) {
            w[i] = (u32)block[i * 4] << 24 | (u32)block[i * 4 + 1] << 16 | (u32)block[i * 4 + 2] << 8 | block[i * 4 + 3];
        }

        for (int i = 16; i < 64; i++) {
            u32 s0 = rotated(w[i - 15], 7) ^ rotated(w[i - 15], 18) ^ (w[i - 15] >> 3);
            u32 s1 = rotated(w[i - 2], 17) ^ rotated(w[i - 2], 19) ^ (w[i - 2] >> 10);

            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        u32 v[8];

        memcpy(v, h, sizeof(v));

        for (int i = 0; i < 64; i++) {
            u32 s1 = rotated(v[4], 6) ^ rotated(v[4], 11) ^ rotated(v[4], 25);
            u32 choice = (v[4] & v[5]) ^ (~v[4] & v[6]);
            u32 t1 = v[7] + s1 + choice + roundConstants[i] + w[i];
            u32 s0 = rotated(v[0], 2) ^ rotated(v[0], 13) ^ rotated(v[0], 22);
            u32 majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);

            memmove(v + 1, v, 7 * sizeof(u32));
            v[4] += t1;
            v[0] = t1 + s0 + majority;
        }

        for (int i = 0; i < 8; i++) {
            h[i] += v[i];
        }
    }
}

void sha256(const void* data, size_t size, u8 (&digest)[32]) {
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const u8* bytes = (const u8*)data;
    size_t full = size / 64;
    size_t rest = size % 64;
    size_t blocks = rest + 9 > 64 ? 2 : 1;
    u64 bits = (u64)size * 8;
    u8 tail[128] = {};

    for (size_t i = 0; i < full; i++) {
        compress(h, bytes + i * 64);
    }

    memcpy(tail, bytes + full * 64, rest);
    tail[rest] = 0x80;

    for (int i = 0; i < 8; i++) {
        tail[blocks * 64 - 1 - i] = (u8)(bits >> (8 * i));
    }

    for (size_t i = 0; i < blocks; i++) {
        compress(h, tail + i * 64);
    }

    for (int i = 0; i < 32; i++) {
        digest[i] = (u8)(h[i / 4] >> (24 - 8 * (i % 4)));
    }
}

namespace {
    enum : u8 {
        KindGlobal,
        KindFunction,
        KindConstant,
        KindArgument,
        KindInstruction,
    };

    enum : u8 {
        TagRaw,
        TagValue,
        TagBlock,
    };

    struct Bits {
        Vector<u32> words;
        Vector<u64> open;
        Vector<u64> ops;
        u64 pending = 0;
        u32 used = 0;
        u32 width = 2;

        void emit(u64 value, u32 bits) {
            pending |= value << used;
            used += bits;

            while (used >= 32) {
                words.pushBack((u32)pending);
                pending >>= 32;
                used -= 32;
            }
        }

        void vbr(u64 value, u32 bits) {
            u64 high = 1ull << (bits - 1);

            while (value >= high) {
                emit((value & (high - 1)) | high, bits);
                value >>= bits - 1;
            }

            emit(value, bits);
        }

        void align() {
            if (used) {
                emit(0, 32 - used);
            }
        }

        void enter(u32 id, u32 inner) {
            emit(1, width);
            vbr(id, 8);
            vbr(inner, 4);
            align();
            open.pushBack((u64)words.length() << 8 | width);
            words.pushBack(0);
            width = inner;
        }

        void leave() {
            emit(0, width);
            align();

            u64 top = open.popBack();
            size_t start = (size_t)(top >> 8);

            words.mut(start) = (u32)(words.length() - start - 1);
            width = (u32)(top & 0xff);
        }

        void record(u32 code) {
            emit(3, width);
            vbr(code, 6);
            vbr(ops.length(), 6);

            for (u64 op : ops) {
                vbr(op, 6);
            }

            ops.clear();
        }

        void chars(const char* text) {
            for (const char* c = text; *c; c++) {
                ops.pushBack((u8)*c);
            }
        }
    };

    static u64 encodedAlign(u32 bytes) {
        u64 log = 0;

        while ((1u << log) < bytes) {
            log++;
        }

        return log + 1;
    }

    static u64 rotatedSign(i64 value) {
        return value >= 0 ? (u64)value << 1 : (u64)(-value) << 1 | 1;
    }

    static void put(u8*& at, const void* data, size_t size) {
        memcpy(at, data, size);
        at += size;
    }

    static void put16(u8*& at, u16 value) {
        put(at, &value, sizeof(value));
    }

    static void put32(u8*& at, u32 value) {
        put(at, &value, sizeof(value));
    }

    static void put64(u8*& at, u64 value) {
        put(at, &value, sizeof(value));
    }

    static void tag(u8*& at, const char* name, u16 size) {
        put(at, name, 4);
        put16(at, size);
    }

    constexpr u32 airMajor = 2;
    constexpr u32 airMinor = 6;
    constexpr u32 languageMajor = 3;
    constexpr u32 languageMinor = 0;
    constexpr u32 macosMajor = 14;
    constexpr const char* triple = "air64-apple-macosx14.0.0";
    constexpr const char* layout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32";
}

AirModule::AirModule()
    : none(voidType())
{
}

u32 AirModule::type(u32 code, const u32* ops, u32 count, const char* name) {
    for (size_t i = 0; i < types.length(); i++) {
        const Type& t = types[i];

        if (t.code == code && t.count == count && t.name == name && (!count || !memcmp(typeOperands.data() + t.start, ops, count * sizeof(u32)))) {
            return (u32)i;
        }
    }

    types.pushBack(Type{code, (u32)typeOperands.length(), count, name});

    if (count) {
        typeOperands.append(ops, count);
    }

    return (u32)types.length() - 1;
}

u32 AirModule::voidType() {
    return type(2, nullptr, 0, nullptr);
}

u32 AirModule::integer(u32 bits) {
    return type(7, &bits, 1, nullptr);
}

u32 AirModule::real() {
    return type(3, nullptr, 0, nullptr);
}

u32 AirModule::half() {
    return type(10, nullptr, 0, nullptr);
}

u32 AirModule::vector(u32 count, u32 element) {
    const u32 ops[2] = {count, element};

    return type(12, ops, 2, nullptr);
}

u32 AirModule::array(u32 count, u32 element) {
    const u32 ops[2] = {count, element};

    return type(11, ops, 2, nullptr);
}

u32 AirModule::pointer(u32 element, u32 space) {
    const u32 ops[2] = {element, space};

    return type(8, ops, 2, nullptr);
}

u32 AirModule::function(u32 result, const u32* params, u32 count) {
    u32 ops[16] = {0, result};

    if (count > 14) {
        fail(StringView(u8"an AIR function takes too many parameters"));
    }

    memcpy(ops + 2, params, count * sizeof(u32));

    return type(21, ops, count + 2, nullptr);
}

u32 AirModule::structure(const char* name, const u32* members, u32 count) {
    u32 ops[16] = {0};

    if (count > 15) {
        fail(StringView(u8"an AIR structure has too many members"));
    }

    memcpy(ops + 1, members, count * sizeof(u32));

    return type(20, ops, count + 1, name);
}

u32 AirModule::opaque(const char* name) {
    const u32 packed = 0;

    return type(6, &packed, 1, name);
}

u32 AirModule::typeOf(u32 value) const {
    return values[value].type;
}

u32 AirModule::make(u8 kind, u32 type, u32 index) {
    values.pushBack(Value{kind, type, index});

    return (u32)values.length() - 1;
}

u32 AirModule::constant(u32 type, u32 code, const u64* ops, u32 count) {
    constants.pushBack(Constant{type, code, (u32)constantOperands.length(), count});

    if (count) {
        constantOperands.append(ops, count);
    }

    return make(KindConstant, type, (u32)constants.length() - 1);
}

u32 AirModule::integerConstant(u32 type, i64 value) {
    u64 encoded = rotatedSign(value);

    return constant(type, 4, &encoded, 1);
}

u32 AirModule::realConstant(u32 type, u64 bits) {
    return constant(type, 6, &bits, 1);
}

u32 AirModule::undef(u32 type) {
    return constant(type, 3, nullptr, 0);
}

u32 AirModule::aggregate(u32 type, const u32* elements, u32 count) {
    u64 ops[16];

    if (count > 16) {
        fail(StringView(u8"an AIR aggregate has too many elements"));
    }

    for (u32 i = 0; i < count; i++) {
        ops[i] = elements[i];
    }

    return constant(type, 7, ops, count);
}

u32 AirModule::global(const char* name, u32 type, u32 space) {
    globals.pushBack(Global{name, type, space, undef(type)});

    return make(KindGlobal, pointer(type, space), (u32)globals.length() - 1);
}

u32 AirModule::declare(const char* name, u32 type, AirAttributes attributes) {
    for (const Function& f : functions) {
        if (!strcmp(f.name, name)) {
            return f.value;
        }
    }

    u32 value = make(KindFunction, pointer(type, 0), (u32)functions.length());

    functions.pushBack(Function{name, type, value, attributes, false});

    return value;
}

u32 AirModule::define(const char* name, u32 type, AirAttributes attributes) {
    for (const Function& f : functions) {
        if (f.body) {
            fail(StringView(u8"an AIR module defines one function"));
        }
    }

    u32 value = make(KindFunction, pointer(type, 0), (u32)functions.length());

    functions.pushBack(Function{name, type, value, attributes, true});

    return value;
}

u32 AirModule::argument(u32 index) {
    for (const Function& f : functions) {
        if (f.body) {
            const Type& t = types[f.type];

            if (index + 2 >= t.count) {
                fail(StringView(u8"an AIR function has no such argument"));
            }

            return make(KindArgument, typeOperands[t.start + 2 + index], index);
        }
    }

    fail(StringView(u8"an AIR module defines no function"));
}

u32 AirModule::text(const char* text) {
    metadata.pushBack(Metadata{1, 0, 0, text});

    return (u32)metadata.length() - 1;
}

u32 AirModule::value(u32 value) {
    metadata.pushBack(Metadata{2, (u32)metadataOperands.length(), 1, nullptr});
    metadataOperands.pushBack(value);

    return (u32)metadata.length() - 1;
}

u32 AirModule::node(const u32* items, u32 count) {
    metadata.pushBack(Metadata{3, (u32)metadataOperands.length(), count, nullptr});

    if (count) {
        metadataOperands.append(items, count);
    }

    return (u32)metadata.length() - 1;
}

void AirModule::name(const char* name, const u32* nodes, u32 count) {
    named.pushBack(Named{name, (u32)metadataOperands.length(), count});
    metadataOperands.append(nodes, count);
}

u32 AirModule::block() {
    blocks.pushBack(0xffffffffu);

    return (u32)blocks.length() - 1;
}

void AirModule::enter(u32 block) {
    if (blocks[block] != 0xffffffffu) {
        fail(StringView(u8"an AIR block is entered twice"));
    }

    blocks.mut(block) = entered++;
    at = block;
}

u32 AirModule::current() const {
    return at;
}

u32 AirModule::instruction(u32 opcode, u32 type, const Operand* ops, u32 count) {
    code.pushBack(Instruction{opcode, type, (u32)operands.length(), count});

    if (count) {
        operands.append(ops, count);
    }

    return type == none ? 0xffffffffu : make(KindInstruction, type, (u32)code.length() - 1);
}

u32 AirModule::binary(u32 opcode, u32 a, u32 b) {
    const Operand ops[3] = {{TagValue, a}, {TagValue, b}, {TagRaw, opcode}};

    return instruction(2, typeOf(a), ops, 3);
}

u32 AirModule::cast(u32 opcode, u32 value, u32 type) {
    const Operand ops[3] = {{TagValue, value}, {TagRaw, type}, {TagRaw, opcode}};

    return instruction(3, type, ops, 3);
}

u32 AirModule::compare(u32 predicate, u32 a, u32 b) {
    const Operand ops[3] = {{TagValue, a}, {TagValue, b}, {TagRaw, predicate}};

    return instruction(28, integer(1), ops, 3);
}

u32 AirModule::select(u32 condition, u32 yes, u32 no) {
    const Operand ops[3] = {{TagValue, yes}, {TagValue, no}, {TagValue, condition}};

    return instruction(29, typeOf(yes), ops, 3);
}

u32 AirModule::extract(u32 vector, u32 index) {
    const Operand ops[2] = {{TagValue, vector}, {TagValue, index}};
    const Type& t = types[typeOf(vector)];

    return instruction(6, typeOperands[t.start + 1], ops, 2);
}

u32 AirModule::insert(u32 vector, u32 element, u32 index) {
    const Operand ops[3] = {{TagValue, vector}, {TagValue, element}, {TagValue, index}};

    return instruction(7, typeOf(vector), ops, 3);
}

u32 AirModule::element(u32 source, u32 pointer, const u32* indices, u32 count, u32 result) {
    Operand ops[8] = {{TagRaw, 1}, {TagRaw, source}, {TagValue, pointer}};

    if (count > 5) {
        fail(StringView(u8"an AIR address has too many indices"));
    }

    for (u32 i = 0; i < count; i++) {
        ops[3 + i] = Operand{TagValue, indices[i]};
    }

    return instruction(43, result, ops, count + 3);
}

u32 AirModule::load(u32 type, u32 pointer, u32 align) {
    const Operand ops[4] = {{TagValue, pointer}, {TagRaw, type}, {TagRaw, encodedAlign(align)}, {TagRaw, 0}};

    return instruction(20, type, ops, 4);
}

void AirModule::store(u32 value, u32 pointer, u32 align) {
    const Operand ops[4] = {{TagValue, pointer}, {TagValue, value}, {TagRaw, encodedAlign(align)}, {TagRaw, 0}};

    instruction(44, none, ops, 4);
}

u32 AirModule::call(u32 function, const u32* args, u32 count) {
    Operand ops[20] = {{TagRaw, 0}, {TagRaw, 1u << 15}, {TagRaw, 0}, {TagValue, function}};
    u32 type = 0;

    if (count > 16) {
        fail(StringView(u8"an AIR call passes too many arguments"));
    }

    for (const Function& f : functions) {
        if (f.value == function) {
            type = f.type;
        }
    }

    ops[2].value = type;

    for (u32 i = 0; i < count; i++) {
        ops[4 + i] = Operand{TagValue, args[i]};
    }

    return instruction(34, typeOperands[types[type].start + 1], ops, count + 4);
}

u32 AirModule::phi(u32 type) {
    const Operand ops[1] = {{TagRaw, type}};

    return instruction(16, type, ops, 1);
}

void AirModule::arrive(u32 phi, u32 value, u32 block) {
    incoming.pushBack(Incoming{values[phi].index, value, block});
}

void AirModule::branch(u32 target) {
    const Operand ops[1] = {{TagBlock, target}};

    instruction(11, none, ops, 1);
}

void AirModule::branch(u32 condition, u32 yes, u32 no) {
    const Operand ops[3] = {{TagBlock, yes}, {TagBlock, no}, {TagValue, condition}};

    instruction(11, none, ops, 3);
}

void AirModule::ret(u32 value) {
    const Operand ops[1] = {{TagValue, value}};

    instruction(10, none, ops, 1);
}

void AirModule::ret() {
    instruction(10, none, nullptr, 0);
}

StringView AirModule::library(ObjPool& pool, const char* entry, AirFunction kind) {
    u32 base[5] = {0, (u32)globals.length(), (u32)(globals.length() + functions.length()), (u32)(globals.length() + functions.length() + constants.length()), 0};
    u32 arguments = 0;
    Vector<u32> ids;
    const Function* defined = nullptr;

    for (const Function& f : functions) {
        if (f.body) {
            defined = &f;
            arguments = types[f.type].count - 2;
        }
    }

    if (!defined) {
        fail(StringView(u8"an AIR module defines no function"));
    }

    base[4] = base[3] + arguments;

    for (const Instruction& i : code) {
        ids.pushBack(i.type == none ? 0xffffffffu : base[4]++);
    }

    auto absolute = [&](u32 handle) -> u64 {
        const Value& v = values[handle];

        return v.kind == KindInstruction ? ids[v.index] : base[v.kind] + v.index;
    };

    Bits b;

    b.emit('B', 8);
    b.emit('C', 8);
    b.emit(0x0, 4);
    b.emit(0xC, 4);
    b.emit(0xE, 4);
    b.emit(0xD, 4);
    b.enter(8, 3);
    b.ops.pushBack(1);
    b.record(1);

    b.enter(17, 4);
    b.ops.pushBack(types.length());
    b.record(1);

    for (const Type& t : types) {
        if (t.name) {
            b.chars(t.name);
            b.record(19);
        }

        for (u32 i = 0; i < t.count; i++) {
            b.ops.pushBack(typeOperands[t.start + i]);
        }

        b.record(t.code);
    }

    b.leave();

    b.enter(10, 3);

    const u64 groups[2][6] = {{1, 0xffffffffu, 0, 18, 0, 20}, {2, 0xffffffffu, 0, 43, 0, 18}};

    for (const auto& group : groups) {
        b.ops.append(group, 6);
        b.record(3);
    }

    b.leave();
    b.enter(9, 3);

    for (u64 group = 1; group <= 2; group++) {
        b.ops.pushBack(group);
        b.record(2);
    }

    b.leave();
    b.chars(triple);
    b.record(2);
    b.chars(layout);
    b.record(3);

    for (const Global& g : globals) {
        const u64 ops[9] = {g.type, (u64)g.space << 2 | 2, absolute(g.initial) + 1, 3, encodedAlign(4), 0, 0, 0, 1};

        b.ops.append(ops, 9);
        b.record(7);
    }

    for (const Function& f : functions) {
        const u64 ops[8] = {f.type, 0, f.body ? 0u : 1u, 0, (u64)f.attributes, 0, 0, 0};

        b.ops.append(ops, 8);
        b.record(8);
    }

    b.enter(11, 4);

    u32 setType = 0xffffffffu;

    for (const Constant& c : constants) {
        if (c.type != setType) {
            setType = c.type;
            b.ops.pushBack(c.type);
            b.record(1);
        }

        for (u32 i = 0; i < c.count; i++) {
            u64 op = constantOperands[c.start + i];

            b.ops.pushBack(c.code == 7 ? absolute((u32)op) : op);
        }

        b.record(c.code);
    }

    b.leave();
    b.enter(15, 3);

    for (const Metadata& m : metadata) {
        if (m.code == 1) {
            b.chars(m.text);
        } else if (m.code == 2) {
            u32 handle = metadataOperands[m.start];

            b.ops.pushBack(typeOf(handle));
            b.ops.pushBack(absolute(handle));
        } else {
            for (u32 i = 0; i < m.count; i++) {
                b.ops.pushBack(metadataOperands[m.start + i] + 1);
            }
        }

        b.record(m.code);
    }

    for (const Named& n : named) {
        b.chars(n.name);
        b.record(4);

        for (u32 i = 0; i < n.count; i++) {
            b.ops.pushBack(metadataOperands[n.start + i]);
        }

        b.record(10);
    }

    b.leave();
    b.enter(14, 3);

    for (size_t i = 0; i < globals.length(); i++) {
        b.ops.pushBack(base[KindGlobal] + i);
        b.chars(globals[i].name);
        b.record(1);
    }

    for (size_t i = 0; i < functions.length(); i++) {
        b.ops.pushBack(base[KindFunction] + i);
        b.chars(functions[i].name);
        b.record(1);
    }

    b.leave();
    b.enter(12, 4);
    b.ops.pushBack(entered);
    b.record(1);

    u32 next = base[3] + arguments;

    for (size_t n = 0; n < code.length(); n++) {
        const Instruction& i = code[n];

        for (u32 k = 0; k < i.count; k++) {
            const Operand& op = operands[i.start + k];

            if (op.tag == TagRaw) {
                b.ops.pushBack(op.value);
            } else if (op.tag == TagBlock) {
                b.ops.pushBack(blocks[(u32)op.value]);
            } else {
                u64 id = absolute((u32)op.value);

                if (id >= next) {
                    fail(StringView(u8"an AIR instruction uses a value defined after it"));
                }

                b.ops.pushBack(next - id);
            }
        }

        if (i.code == 16) {
            for (const Incoming& in : incoming) {
                if (in.phi == n) {
                    b.ops.pushBack(rotatedSign((i64)next - (i64)absolute(in.value)));
                    b.ops.pushBack(blocks[in.block]);
                }
            }
        }

        b.record(i.code);

        if (i.type != none) {
            next++;
        }
    }

    b.leave();
    b.leave();

    size_t moduleSize = 20 + b.words.length() * sizeof(u32);
    size_t nameSize = strlen(entry) + 1;
    size_t entrySize = 4 + 6 + nameSize + 7 + 38 + 14 + 30 + 14 + 4;
    size_t publicOffset = 88 + 4 + entrySize + 4;
    size_t bitcodeOffset = publicOffset + 16;
    size_t total = bitcodeOffset + moduleSize;
    u8* bytes = (u8*)pool.allocate(total);
    u8* at = bytes;
    u8* module = bytes + bitcodeOffset;
    u8 digest[32];

    put(at, "MTLB", 4);
    put16(at, 0x8001);
    put16(at, 2);
    put16(at, 7);
    put16(at, 0x8100);
    put16(at, macosMajor);
    put16(at, 0);
    put64(at, total);
    put64(at, 88);
    put64(at, entrySize);
    put64(at, publicOffset);
    put64(at, 8);
    put64(at, publicOffset + 8);
    put64(at, 8);
    put64(at, bitcodeOffset);
    put64(at, moduleSize);
    put32(at, 1);

    u8* wrapped = module;

    put32(wrapped, 0x0b17c0de);
    put32(wrapped, 0);
    put32(wrapped, 20);
    put32(wrapped, (u32)(moduleSize - 20));
    put32(wrapped, 0xffffffffu);
    put(wrapped, b.words.data(), b.words.length() * sizeof(u32));
    sha256(module, moduleSize, digest);

    put32(at, (u32)entrySize);
    tag(at, "NAME", (u16)nameSize);
    put(at, entry, nameSize);
    tag(at, "TYPE", 1);
    *at++ = (u8)kind;
    tag(at, "HASH", 32);
    put(at, digest, 32);
    tag(at, "MDSZ", 8);
    put64(at, moduleSize);
    tag(at, "OFFT", 24);
    put64(at, 0);
    put64(at, 0);
    put64(at, 0);
    tag(at, "VERS", 8);
    put16(at, airMajor);
    put16(at, airMinor);
    put16(at, languageMajor);
    put16(at, languageMinor);
    put(at, "ENDT", 4);
    put(at, "ENDT", 4);

    for (int i = 0; i < 2; i++) {
        put32(at, 4);
        put(at, "ENDT", 4);
    }

    return StringView(bytes, total);
}
