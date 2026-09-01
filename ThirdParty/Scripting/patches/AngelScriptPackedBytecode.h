#pragma once

BEGIN_AS_NAMESPACE
// Pointer operands follow the opcode DWORD and are consequently only 4-byte
// aligned in packed bytecode. A compiler-supported packed wrapper preserves
// the assignable macro API without forming a naturally aligned pointer there.
#if defined(_MSC_VER)
#pragma pack(push, 1)
struct asPWORD_UNALIGNED
{
    asPWORD value;
};
#pragma pack(pop)
#else
struct __attribute__((packed, may_alias)) asPWORD_UNALIGNED
{
    asPWORD value;
};
#endif
END_AS_NAMESPACE

#define SPARK_ANGELSCRIPT_PACKED_POINTER_OPERAND 1
#undef asBC_PTRARG
#define asBC_PTRARG(x)                                                                                                 \
    (((AS_NAMESPACE_QUALIFIER asPWORD_UNALIGNED*)(((AS_NAMESPACE_QUALIFIER asDWORD*)(x)) + 1))->value)
