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

    if (a->kind == TYPE_FUNC) {
        if (!type_equals(a->func.return_type, b->func.return_type)) {
            return false;
        }
        if (a->func.param_count != b->func.param_count) {
            return false;
        }
        for (size_t i = 0; i < a->func.param_count; ++i) {
            if (!type_equals(a->func.param_types[i], b->func.param_types[i])) {
                return false;
            }
        }
        return true;
    }

    if (a->kind == TYPE_STRUCT) {
        return a->structure.name.len == b->structure.name.len &&
               memcmp(a->structure.name.data, b->structure.name.data, a->structure.name.len) == 0;
    }

    if (a->kind == TYPE_ARRAY) {
        return a->array.count == b->array.count && type_equals(a->array.elem_type, b->array.elem_type);
    }

    if (a->kind == TYPE_SLICE) {
        return type_equals(a->slice.elem_type, b->slice.elem_type);
    }

    if (a->kind == TYPE_ENUM) {
        return a->enumeration.name.len == b->enumeration.name.len &&
               memcmp(a->enumeration.name.data, b->enumeration.name.data, a->enumeration.name.len) == 0;
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

        case TYPE_STRUCT: {
            if (arena) {
                return arena_sprintf(arena, "%.*s", (int)type->structure.name.len, type->structure.name.data);
            }
            return "struct";
        }

        case TYPE_ENUM: {
            if (arena) {
                return arena_sprintf(arena, "%.*s", (int)type->enumeration.name.len, type->enumeration.name.data);
            }
            return "enum";
        }

        case TYPE_ARRAY: {
            if (arena) {
                return arena_sprintf(arena, "[%zu]%s", type->array.count, type_to_str(type->array.elem_type, arena));
            }
            return "array";
        }

        case TYPE_SLICE: {
            if (arena) {
                return arena_sprintf(arena, "[]%s", type_to_str(type->slice.elem_type, arena));
            }
            return "slice";
        }

        case TYPE_FUNC: {
            if (!arena) {
                return "proc";
            }
            char buf[512];
            size_t offset = snprintf(buf, sizeof(buf), "proc(");
            for (size_t i = 0; i < type->func.param_count; ++i) {
                const char* pt = type_to_str(type->func.param_types[i], arena);
                offset += snprintf(buf + offset, sizeof(buf) - offset, "%s%s", pt, (i + 1 < type->func.param_count) ? ", " : "");
            }
            snprintf(buf + offset, sizeof(buf) - offset, ") -> %s", type_to_str(type->func.return_type, arena));
            return arena_strdup(arena, buf);
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

void type_struct_init(Type* t, StrView name, StructField* fields, size_t count, bool is_packed) {
    assert(t != NULL);

    t->kind                  = TYPE_STRUCT;
    t->structure.name        = name;
    t->structure.fields      = fields;
    t->structure.field_count = count;
    t->structure.is_packed   = is_packed;

    size_t current_offset = 0;
    size_t max_align      = 1;

    for (size_t i = 0; i < count; ++i) {
        StructField* f = &fields[i];
        size_t f_size  = (f->type && f->type->size) ? f->type->size : 8;
        size_t f_align = is_packed ? 1 : ((f->type && f->type->align) ? f->type->align : 8);

        if (f_align > max_align) {
            max_align = f_align;
        }

        if (!is_packed && f_align > 1) {
            current_offset = (current_offset + f_align - 1) & ~(f_align - 1);
        }

        f->offset = current_offset;
        current_offset += f_size;
    }

    t->align = is_packed ? 1 : max_align;

    if (!is_packed && t->align > 1) {
        t->size = (current_offset + t->align - 1) & ~(t->align - 1);
    } else {
        t->size = current_offset;
    }
}

Type* type_struct_create(Arena* arena, StrView name, StructField* fields, size_t count, bool is_packed) {
    Type* t = ARENA_NEW_ZERO(arena, Type);

    type_struct_init(t, name, fields, count, is_packed);

    return t;
}

StructField* type_struct_lookup_field(const Type* struct_type, StrView field_name) {
    if (!struct_type || struct_type->kind != TYPE_STRUCT) {
        return NULL;
    }

    for (size_t i = 0; i < struct_type->structure.field_count; ++i) {
        StructField* f = &struct_type->structure.fields[i];

        if (f->name.len == field_name.len && memcmp(f->name.data, field_name.data, f->name.len) == 0) {
            return f;
        }
    }

    return NULL;
}

Type* type_array_create(Arena* arena, Type* elem_type, size_t count) {
    Type* t = ARENA_NEW_ZERO(arena, Type);

    t->kind            = TYPE_ARRAY;
    t->array.elem_type = elem_type;
    t->array.count     = count;

    size_t e_size  = elem_type->size ? elem_type->size : 8;
    size_t e_align = elem_type->align ? elem_type->align : 8;

    t->size  = e_size * count;
    t->align = e_align;

    return t;
}

Type* type_slice_create(Arena* arena, Type* elem_type) {
    Type* t = ARENA_NEW_ZERO(arena, Type);

    t->kind            = TYPE_SLICE;
    t->size            = 16;
    t->align           = 8;
    t->slice.elem_type = elem_type;

    return t;
}

bool type_is_slice(const Type* type) {
    return type && type->kind == TYPE_SLICE;
}

bool type_is_compound(const Type* type) {
    return type && (type->kind == TYPE_STRUCT || type->kind == TYPE_SLICE);
}

Type* type_func_create(Arena* arena, Type* return_type, Type** param_types, size_t param_count) {
    Type* t = ARENA_NEW_ZERO(arena, Type);
    t->kind              = TYPE_FUNC;
    t->size              = 8;
    t->align             = 8;
    t->func.return_type  = return_type ? return_type : type_primitive(TYPE_VOID);
    t->func.param_types  = param_types;
    t->func.param_count  = param_count;
    return t;
}

Type* type_enum_create(Arena* arena, StrView name, Type* underlying_type, EnumVariant* variants, size_t count) {
    Type* t = ARENA_NEW_ZERO(arena, Type);

    Type* base = underlying_type ? underlying_type : type_primitive(TYPE_U32);

    t->kind                        = TYPE_ENUM;
    t->size                        = base->size;
    t->align                       = base->align;
    t->enumeration.name            = name;
    t->enumeration.underlying_type = base;
    t->enumeration.variants        = variants;
    t->enumeration.variant_count   = count;

    return t;
}

EnumVariant* type_enum_lookup_variant(const Type* enum_type, StrView variant_name) {
    if (!enum_type || enum_type->kind != TYPE_ENUM) {
        return NULL;
    }

    for (size_t i = 0; i < enum_type->enumeration.variant_count; ++i) {
        EnumVariant* v = &enum_type->enumeration.variants[i];

        if (v->name.len == variant_name.len && memcmp(v->name.data, variant_name.data, v->name.len) == 0) {
            return v;
        }
    }

    return NULL;
}