#include "type.h"

#include <assert.h>
#include <stdio.h>

static Type s_type_void = { .kind = TYPE_VOID, .size = 0, .align = 1 };
static Type s_type_bool = { .kind = TYPE_BOOL, .size = 1, .align = 1 };
static Type s_type_char = { .kind = TYPE_CHAR, .size = 1, .align = 1 };

static Type s_type_i8   = { .kind = TYPE_I8,   .size = 1, .align = 1 };
static Type s_type_i16  = { .kind = TYPE_I16,  .size = 2, .align = 2 };
static Type s_type_i32  = { .kind = TYPE_I32,  .size = 4, .align = 4 };
static Type s_type_i64  = { .kind = TYPE_I64,  .size = 8, .align = 8 };

static Type s_type_u8   = { .kind = TYPE_U8,   .size = 1, .align = 1 };
static Type s_type_u16  = { .kind = TYPE_U16,  .size = 2, .align = 2 };
static Type s_type_u32  = { .kind = TYPE_U32,  .size = 4, .align = 4 };
static Type s_type_u64  = { .kind = TYPE_U64,  .size = 8, .align = 8 };

Type* type_primitive(TypeKind kind) {
    switch (kind) {
        case TYPE_VOID: return &s_type_void;
        case TYPE_BOOL: return &s_type_bool;
        case TYPE_CHAR: return &s_type_char;

        case TYPE_I8:   return &s_type_i8;
        case TYPE_I16:  return &s_type_i16;
        case TYPE_I32:  return &s_type_i32;
        case TYPE_I64:  return &s_type_i64;

        case TYPE_U8:   return &s_type_u8;
        case TYPE_U16:  return &s_type_u16;
        case TYPE_U32:  return &s_type_u32;
        case TYPE_U64:  return &s_type_u64;

        default:
            assert(false && "Invalid primitive type kind");
            return NULL;
    }
}

Type* type_ptr(Arena* arena, Type* base_type) {
    assert(arena != NULL);
    assert(base_type != NULL);

    Type* ptr_type = ARENA_NEW_ZERO(arena, Type);

    ptr_type->kind     = TYPE_PTR;
    ptr_type->size     = 8;
    ptr_type->align    = 8;
    ptr_type->ptr.base = base_type;

    return ptr_type;
}

bool type_equals(const Type* a, const Type* b) {
    if (a == b) {
        return true;
    }

    if (!a || !b || a->kind != b->kind) {
        return false;
    }

    if (a->kind == TYPE_PTR) {
        return type_equals(a->ptr.base, b->ptr.base);
    }

    return true;
}

bool type_is_integer(const Type* type) {
    if (!type) {
        return false;
    }

    return (type->kind >= TYPE_CHAR && type->kind <= TYPE_U64);
}

bool type_is_signed(const Type* type) {
    if (!type) {
        return false;
    }

    return (type->kind >= TYPE_I8 && type->kind <= TYPE_I64);
}

bool type_is_pointer(const Type* type) {
    return type && type->kind == TYPE_PTR;
}

size_t type_pointer_depth(const Type* type) {
    size_t depth = 0;

    while (type && type->kind == TYPE_PTR) {
        depth++;
        type = type->ptr.base;
    }

    return depth;
}

const char* type_to_str(const Type* type, Arena* arena) {
    if (!type) {
        return "<null_type>";
    }

    switch (type->kind) {
        case TYPE_VOID: return "void";
        case TYPE_BOOL: return "bool";
        case TYPE_CHAR: return "char";

        case TYPE_I8:   return "i8";
        case TYPE_I16:  return "i16";
        case TYPE_I32:  return "i32";
        case TYPE_I64:  return "i64";

        case TYPE_U8:   return "u8";
        case TYPE_U16:  return "u16";
        case TYPE_U32:  return "u32";
        case TYPE_U64:  return "u64";

        case TYPE_PTR: {
            const char* base_str = type_to_str(type->ptr.base, arena);

            if (arena) {
                return arena_sprintf(arena, "%s*", base_str);
            }
            
            return "ptr";
        }

        default:
            return "<unknown_type>";
    }
}

Type* type_integer_promote(const Type* type) {
    if (!type || !type_is_integer(type)) {
        return (Type*)type;
    }

    if (type->size < 4) {
        return type_primitive(TYPE_I32);
    }

    return (Type*)type;
}

static int get_type_rank(TypeKind kind) {
    switch (kind) {
        case TYPE_I32: return 1;
        case TYPE_U32: return 2;
        case TYPE_I64: return 3;
        case TYPE_U64: return 4;
        default:       return 0;
    }
}

Type* type_common_arithmetic(const Type* a, const Type* b) {
    if (!a || !b) {
        return NULL;
    }

    Type* prom_a = type_integer_promote(a);
    Type* prom_b = type_integer_promote(b);

    if (type_equals(prom_a, prom_b)) {
        return prom_a;
    }

    if (type_is_integer(prom_a) && type_is_integer(prom_b)) {
        int rank_a = get_type_rank(prom_a->kind);
        int rank_b = get_type_rank(prom_b->kind);

        return (rank_a >= rank_b) ? prom_a : prom_b;
    }

    return prom_a;
}