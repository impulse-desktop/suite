#pragma once

#include <std/sys/types.h>
#include <std/lib/vector.h>

namespace stl {
    class ObjPool;
    class StringView;
}

void sha256(const void* data, size_t size, u8 (&digest)[32]);

enum class AirFunction : u8 {
    Kernel = 2,
    Visible = 4,
};

enum class AirAttributes : u8 {
    None,
    Pure,
    Convergent,
};

struct AirModule {
    struct Type {
        u32 code;
        u32 start;
        u32 count;
        const char* name;
    };

    struct Value {
        u8 kind;
        u32 type;
        u32 index;
    };

    struct Constant {
        u32 type;
        u32 code;
        u32 start;
        u32 count;
    };

    struct Global {
        const char* name;
        u32 type;
        u32 space;
        u32 initial;
    };

    struct Function {
        const char* name;
        u32 type;
        u32 value;
        AirAttributes attributes;
        bool body;
    };

    struct Operand {
        u8 tag;
        u64 value;
    };

    struct Instruction {
        u32 code;
        u32 type;
        u32 start;
        u32 count;
    };

    struct Incoming {
        u32 phi;
        u32 value;
        u32 block;
    };

    struct Metadata {
        u8 code;
        u32 start;
        u32 count;
        const char* text;
    };

    struct Named {
        const char* name;
        u32 start;
        u32 count;
    };

    stl::Vector<Type> types;
    stl::Vector<u32> typeOperands;
    stl::Vector<Value> values;
    stl::Vector<Constant> constants;
    stl::Vector<u64> constantOperands;
    stl::Vector<Global> globals;
    stl::Vector<Function> functions;
    stl::Vector<Instruction> code;
    stl::Vector<Operand> operands;
    stl::Vector<Incoming> incoming;
    stl::Vector<u32> blocks;
    stl::Vector<Metadata> metadata;
    stl::Vector<u32> metadataOperands;
    stl::Vector<Named> named;
    u32 entered = 0;
    u32 at = 0;
    u32 none;

    AirModule();

    u32 voidType();
    u32 integer(u32 bits);
    u32 real();
    u32 half();
    u32 vector(u32 count, u32 element);
    u32 array(u32 count, u32 element);
    u32 pointer(u32 element, u32 space);
    u32 function(u32 result, const u32* params, u32 count);
    u32 structure(const char* name, const u32* members, u32 count);
    u32 opaque(const char* name);
    u32 typeOf(u32 value) const;

    u32 integerConstant(u32 type, i64 value);
    u32 realConstant(u32 type, u64 bits);
    u32 undef(u32 type);
    u32 aggregate(u32 type, const u32* elements, u32 count);
    u32 global(const char* name, u32 type, u32 space);
    u32 declare(const char* name, u32 type, AirAttributes attributes);
    u32 define(const char* name, u32 type, AirAttributes attributes);
    u32 argument(u32 index);

    u32 text(const char* text);
    u32 value(u32 value);
    u32 node(const u32* items, u32 count);
    void name(const char* name, const u32* nodes, u32 count);

    u32 block();
    void enter(u32 block);
    u32 current() const;
    u32 binary(u32 opcode, u32 a, u32 b);
    u32 cast(u32 opcode, u32 value, u32 type);
    u32 compare(u32 predicate, u32 a, u32 b);
    u32 select(u32 condition, u32 yes, u32 no);
    u32 extract(u32 vector, u32 index);
    u32 insert(u32 vector, u32 element, u32 index);
    u32 element(u32 source, u32 pointer, const u32* indices, u32 count, u32 result);
    u32 load(u32 type, u32 pointer, u32 align);
    void store(u32 value, u32 pointer, u32 align);
    u32 call(u32 function, const u32* args, u32 count);
    u32 phi(u32 type);
    void arrive(u32 phi, u32 value, u32 block);
    void branch(u32 target);
    void branch(u32 condition, u32 yes, u32 no);
    void ret(u32 value);
    void ret();

    stl::StringView library(stl::ObjPool& pool, const char* name, AirFunction kind);

private:
    u32 type(u32 code, const u32* ops, u32 count, const char* name);
    u32 make(u8 kind, u32 type, u32 index);
    u32 constant(u32 type, u32 code, const u64* ops, u32 count);
    u32 instruction(u32 code, u32 type, const Operand* ops, u32 count);
};
