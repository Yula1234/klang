#include "type.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static Type s_type_void   = { .kind = TYPE_VOID,   .size = 0,  .align = 1, .loc = {0} };
static Type s_type_bool   = { .kind = TYPE_BOOL,   .size = 1,  .align = 1, .loc = {0} };
static Type s_type_char   = { .kind = TYPE_CHAR,   .size = 1,  .align = 1, .loc = {0} };
static Type s_type_valist = { .kind = TYPE_VALIST, .size = 24, .align = 8, .loc = {0} };
static Type s_type_null   = { .kind = TYPE_NULL,   .size = 8,  .align = 8, .loc = {0} };

static Type s_type_i8   = { .kind = TYPE_I8,   .size = 1, .align = 1, .loc = {0} };
static Type s_type_i16  = { .kind = TYPE_I16,  .size = 2, .align = 2, .loc = {0} };
static Type s_type_i32  = { .kind = TYPE_I32,  .size = 4, .align = 4, .loc = {0} };
static Type s_type_i64  = { .kind = TYPE_I64,  .size = 8, .align = 8, .loc = {0} };

static Type s_type_u8   = { .kind = TYPE_U8,   .size = 1, .align = 1, .loc = {0} };
static Type s_type_u16  = { .kind = TYPE_U16,  .size = 2, .align = 2, .loc = {0} };
static Type s_type_u32  = { .kind = TYPE_U32,  .size = 4, .align = 4, .loc = {0} };
static Type s_type_u64  = { .kind = TYPE_U64,  .size = 8, .align = 8, .loc = {0} };

Type* type_primitive(TypeKind kind) {
    switch (kind) {
        case TYPE_VOID:   return &s_type_void;
        case TYPE_BOOL:   return &s_type_bool;
        case TYPE_CHAR:   return &s_type_char;
        case TYPE_VALIST: return &s_type_valist;
        case TYPE_NULL:   return &s_type_null;

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
    ptr_type->loc      = (SourceLoc){0};
    ptr_type->ptr.base = base_type;

    return ptr_type;
}

Type* type_param_create(Arena* arena, uint32_t depth, uint32_t index, StrView name, Symbol* symbol) {
    Type* t = ARENA_NEW_ZERO(arena, Type);

    t->kind         = TYPE_PARAM;
    t->size         = 0;
    t->align        = 1;
    t->loc          = (SourceLoc){0};
    t->param.depth  = depth;
    t->param.index  = index;
    t->param.name   = name;
    t->param.symbol = symbol;

    return t;
}

Type* type_subst(Arena* arena, Type* type, const TypeSubstEnv* env) {
    if (!type || !env) {
        return type;
    }

    switch (type->kind) {
        case TYPE_PARAM: {
            for (const TypeSubstEnv* e = env; e != NULL; e = e->parent) {
                if (e->depth == type->param.depth && type->param.index < e->count) {
                    return e->concrete_types[type->param.index];
                }
            }
            return type;
        }

        case TYPE_PTR: {
            Type* subst_base = type_subst(arena, type->ptr.base, env);

            if (subst_base == type->ptr.base) {
                return type;
            }

            return type_ptr(arena, subst_base);
        }

        case TYPE_ARRAY: {
            Type* subst_elem = type_subst(arena, type->array.elem_type, env);

            if (subst_elem == type->array.elem_type) {
                return type;
            }

            return type_array_create(arena, subst_elem, type->array.count);
        }

        case TYPE_SLICE: {
            Type* subst_elem = type_subst(arena, type->slice.elem_type, env);

            if (subst_elem == type->slice.elem_type) {
                return type;
            }

            return type_slice_create(arena, subst_elem);
        }

        case TYPE_DISTINCT: {
            Type* subst_base = type_subst(arena, type->distinct_type.base, env);

            if (subst_base == type->distinct_type.base) {
                return type;
            }

            return type_distinct_create(arena, type->distinct_type.name, subst_base);
        }

        case TYPE_TUPLE: {
            bool changed = false;
            Type** new_elems = ARENA_NEW_ARRAY(arena, Type*, type->tuple.count);

            for (size_t i = 0; i < type->tuple.count; ++i) {
                new_elems[i] = type_subst(arena, type->tuple.elements[i], env);

                if (new_elems[i] != type->tuple.elements[i]) {
                    changed = true;
                }
            }

            if (!changed) {
                return type;
            }

            return type_tuple_create(arena, new_elems, type->tuple.count);
        }

        case TYPE_FUNC: {
            Type* new_ret = type_subst(arena, type->func.return_type, env);
            bool changed = (new_ret != type->func.return_type);

            Type** new_params = ARENA_NEW_ARRAY(arena, Type*, type->func.param_count);

            for (size_t i = 0; i < type->func.param_count; ++i) {
                new_params[i] = type_subst(arena, type->func.param_types[i], env);

                if (new_params[i] != type->func.param_types[i]) {
                    changed = true;
                }
            }

            if (!changed) {
                return type;
            }

            return type_func_create(arena, new_ret, new_params, type->func.param_count, type->func.is_variadic);
        }

        case TYPE_STRUCT: {
            if (type->structure.generic_arg_count > 0) {
                bool changed = false;
                Type** new_args = ARENA_NEW_ARRAY(arena, Type*, type->structure.generic_arg_count);

                for (size_t i = 0; i < type->structure.generic_arg_count; ++i) {
                    new_args[i] = type_subst(arena, type->structure.generic_args[i], env);

                    if (new_args[i] != type->structure.generic_args[i]) {
                        changed = true;
                    }
                }

                if (changed) {
                    Type* copy = ARENA_NEW_ZERO(arena, Type);
                    *copy = *type;
                    copy->structure.generic_args = new_args;
                    return copy;
                }
            }

            return type;
        }

        default:
            return type;
    }
}

Type* type_distinct_create(Arena* arena, StrView name, Type* base_type) {
    assert(arena != NULL);
    assert(base_type != NULL);

    Type* t = ARENA_NEW_ZERO(arena, Type);

    t->kind               = TYPE_DISTINCT;
    t->size               = base_type->size;
    t->align              = base_type->align;
    t->loc                = (SourceLoc){0};
    t->distinct_type.name = name;
    t->distinct_type.base = base_type;

    return t;
}

void type_union_init(Type* t, StrView name, StructField* fields, size_t count) {
    assert(t != NULL);

    t->kind                  = TYPE_UNION;
    t->structure.name        = name;
    t->structure.fields      = fields;
    t->structure.field_count = count;
    t->structure.is_packed   = false;
    t->loc                   = (SourceLoc){0};

    size_t max_size  = 0;
    size_t max_align = 1;

    for (size_t i = 0; i < count; ++i) {
        StructField* f = &fields[i];
        size_t f_size  = (f->type && f->type->size) ? f->type->size : 8;
        size_t f_align = (f->type && f->type->align) ? f->type->align : 8;

        if (f_size > max_size) {
            max_size = f_size;
        }

        if (f_align > max_align) {
            max_align = f_align;
        }

        f->offset = 0;
    }

    t->align = max_align;
    t->size  = (max_align > 1) ? ((max_size + max_align - 1) & ~(max_align - 1)) : max_size;
}

Type* type_union_create(Arena* arena, StrView name, StructField* fields, size_t count) {
    Type* t = ARENA_NEW_ZERO(arena, Type);

    type_union_init(t, name, fields, count);

    return t;
}

bool type_equals(const Type* a, const Type* b) {
    if (a == b) {
        return true;
    }

    if (!a || !b || a->kind != b->kind) {
        return false;
    }

    if (a->kind == TYPE_PARAM) {
        return a->param.depth == b->param.depth && a->param.index == b->param.index;
    }

    if (a->kind == TYPE_DISTINCT) {
        return a->distinct_type.name.len == b->distinct_type.name.len &&
               memcmp(a->distinct_type.name.data, b->distinct_type.name.data, a->distinct_type.name.len) == 0;
    }

    if (a->kind == TYPE_PTR) {
        return type_equals(a->ptr.base, b->ptr.base);
    }

    if (a->kind == TYPE_FUNC) {
        if (a->func.is_variadic != b->func.is_variadic) {
            return false;
        }

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

    if (a->kind == TYPE_STRUCT || a->kind == TYPE_UNION) {
        if (a->structure.generic_arg_count > 0 || b->structure.generic_arg_count > 0) {
            if (a->structure.generic_arg_count != b->structure.generic_arg_count) {
                return false;
            }

            StrView a_name = a->structure.generic_template ? a->structure.generic_template->structure.name : a->structure.name;
            StrView b_name = b->structure.generic_template ? b->structure.generic_template->structure.name : b->structure.name;

            if (a_name.len != b_name.len || memcmp(a_name.data, b_name.data, a_name.len) != 0) {
                return false;
            }

            for (size_t i = 0; i < a->structure.generic_arg_count; ++i) {
                if (!type_equals(a->structure.generic_args[i], b->structure.generic_args[i])) {
                    return false;
                }
            }

            return true;
        }

        return a->structure.name.len == b->structure.name.len &&
               memcmp(a->structure.name.data, b->structure.name.data, a->structure.name.len) == 0;
    }

    if (a->kind == TYPE_ARRAY) {
        return a->array.count == b->array.count && type_equals(a->array.elem_type, b->array.elem_type);
    }

    if (a->kind == TYPE_SLICE) {
        return type_equals(a->slice.elem_type, b->slice.elem_type);
    }

    if (a->kind == TYPE_TUPLE) {
        if (a->tuple.count != b->tuple.count) {
            return false;
        }

        for (size_t i = 0; i < a->tuple.count; ++i) {
            if (!type_equals(a->tuple.elements[i], b->tuple.elements[i])) {
                return false;
            }
        }

        return true;
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

static const char* format_type_list(Arena* arena, Type** types, size_t count, const char* prefix, const char* suffix, const char* separator) {
    if (!arena) {
        return prefix;
    }

    size_t prefix_len    = strlen(prefix);
    size_t suffix_len    = strlen(suffix);
    size_t separator_len = strlen(separator);

    const char** rendered = ARENA_NEW_ARRAY(arena, const char*, count);
    size_t total_len      = prefix_len + suffix_len + 1;

    for (size_t i = 0; i < count; ++i) {
        rendered[i] = type_to_str(types[i], arena);
        total_len  += strlen(rendered[i]);

        if (i + 1 < count) {
            total_len += separator_len;
        }
    }

    char* buffer  = (char*)arena_alloc(arena, total_len);
    size_t cursor = 0;

    memcpy(buffer + cursor, prefix, prefix_len);
    cursor += prefix_len;

    for (size_t i = 0; i < count; ++i) {
        size_t len = strlen(rendered[i]);
        memcpy(buffer + cursor, rendered[i], len);
        cursor += len;

        if (i + 1 < count) {
            memcpy(buffer + cursor, separator, separator_len);
            cursor += separator_len;
        }
    }

    memcpy(buffer + cursor, suffix, suffix_len);
    cursor += suffix_len;

    buffer[cursor] = '\0';

    return buffer;
}

const char* type_to_str(const Type* type, Arena* arena) {
    if (!type) {
        return "<null_type>";
    }

    switch (type->kind) {
        case TYPE_VOID: return "void";
        case TYPE_BOOL: return "bool";
        case TYPE_CHAR: return "char";
        case TYPE_NULL: return "null";

        case TYPE_I8:   return "i8";
        case TYPE_I16:  return "i16";
        case TYPE_I32:  return "i32";
        case TYPE_I64:  return "i64";

        case TYPE_U8:   return "u8";
        case TYPE_U16:  return "u16";
        case TYPE_U32:  return "u32";
        case TYPE_U64:  return "u64";

        case TYPE_PARAM: {
            if (arena) {
                return arena_sprintf(arena, "%.*s", (int)type->param.name.len, type->param.name.data);
            }
            return "type_param";
        }

        case TYPE_PTR: {
            const char* base_str = type_to_str(type->ptr.base, arena);

            if (arena) {
                return arena_sprintf(arena, "%s*", base_str);
            }

            return "ptr";
        }

        case TYPE_STRUCT: {
            if (!arena) {
                return "struct";
            }

            if (type->structure.generic_arg_count > 0) {
                StrView name = type->structure.generic_template
                                   ? type->structure.generic_template->structure.name
                                   : type->structure.name;

                const char* prefix = arena_sprintf(arena, "%.*s[", (int)name.len, name.data);

                return format_type_list(arena, type->structure.generic_args, type->structure.generic_arg_count, prefix, "]", ", ");
            }

            return arena_sprintf(arena, "%.*s", (int)type->structure.name.len, type->structure.name.data);
        }

        case TYPE_UNION: {
            if (!arena) {
                return "union";
            }

            if (type->structure.generic_arg_count > 0) {
                StrView name = type->structure.generic_template
                                   ? type->structure.generic_template->structure.name
                                   : type->structure.name;

                const char* prefix = arena_sprintf(arena, "union %.*s[", (int)name.len, name.data);

                return format_type_list(arena, type->structure.generic_args, type->structure.generic_arg_count, prefix, "]", ", ");
            }

            return arena_sprintf(arena, "union %.*s", (int)type->structure.name.len, type->structure.name.data);
        }

        case TYPE_DISTINCT: {
            if (arena) {
                return arena_sprintf(arena, "distinct %.*s", (int)type->distinct_type.name.len, type->distinct_type.name.data);
            }
            return "distinct";
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

        case TYPE_TUPLE: {
            if (!arena) {
                return "tuple";
            }

            return format_type_list(arena, type->tuple.elements, type->tuple.count, "(", ")", ", ");
        }

        case TYPE_VALIST: return "VaList";

        case TYPE_FUNC: {
            if (!arena) {
                return "proc";
            }

            const char* params_part = format_type_list(arena, type->func.param_types, type->func.param_count, "proc(", ")", ", ");
            const char* return_part = type_to_str(type->func.return_type, arena);

            if (type->func.is_variadic) {
                return arena_sprintf(arena, "proc(..., %s) -> %s", params_part, return_part);
            }

            return arena_sprintf(arena, "%s -> %s", params_part, return_part);
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
    t->loc                   = (SourceLoc){0};

    size_t current_offset = 0;
    size_t max_align      = 1;

    for (size_t i = 0; i < count; ++i) {
        StructField* f = &fields[i];
        size_t f_size  = (f->type && f->type->size) ? f->type->size : 8;
        size_t f_align = is_packed ? 1 : ((f->type && f->type->align) ? f->type->align : 8);

        if (f_align > max_align) {
            max_align = f_align;
        }

        if (f->has_explicit_offset) {
            current_offset = f->explicit_offset;
        } else if (!is_packed && f_align > 1) {
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
    if (!struct_type || (struct_type->kind != TYPE_STRUCT && struct_type->kind != TYPE_UNION)) {
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
    t->loc             = (SourceLoc){0};
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
    t->loc             = (SourceLoc){0};
    t->size            = 16;
    t->align           = 8;
    t->slice.elem_type = elem_type;

    return t;
}

bool type_is_slice(const Type* type) {
    return type && type->kind == TYPE_SLICE;
}

Type* type_tuple_create(Arena* arena, Type** elements, size_t count) {
    Type* t = ARENA_NEW_ZERO(arena, Type);

    t->kind           = TYPE_TUPLE;
    t->loc            = (SourceLoc){0};
    t->tuple.elements = elements;
    t->tuple.count    = count;
    t->tuple.offsets  = ARENA_NEW_ARRAY_ZERO(arena, size_t, count);

    size_t current_offset = 0;
    size_t max_align      = 1;

    for (size_t i = 0; i < count; ++i) {
        Type* elem_type = elements[i];
        size_t e_size   = (elem_type && elem_type->size) ? elem_type->size : 8;
        size_t e_align  = (elem_type && elem_type->align) ? elem_type->align : 8;

        if (e_align > max_align) {
            max_align = e_align;
        }

        if (e_align > 1) {
            current_offset = (current_offset + e_align - 1) & ~(e_align - 1);
        }

        t->tuple.offsets[i] = current_offset;
        current_offset += e_size;
    }

    t->align = max_align;
    t->size  = (t->align > 1) ? ((current_offset + t->align - 1) & ~(t->align - 1)) : current_offset;

    return t;
}

bool type_is_tuple(const Type* type) {
    return type && type->kind == TYPE_TUPLE;
}

bool type_is_compound(const Type* type) {
    return type && (type->kind == TYPE_STRUCT ||
                    type->kind == TYPE_UNION  ||
                    type->kind == TYPE_SLICE  ||
                    type->kind == TYPE_TUPLE  ||
                    type->kind == TYPE_ARRAY);
}

bool type_requires_sret(const Type* type) {
    return type_is_compound(type);
}

Type* type_func_create(Arena* arena, Type* return_type, Type** param_types, size_t param_count, bool is_variadic) {
    Type* t = ARENA_NEW_ZERO(arena, Type);

    t->kind              = TYPE_FUNC;
    t->size              = 8;
    t->align             = 8;
    t->loc               = (SourceLoc){0};
    t->func.return_type  = return_type ? return_type : type_primitive(TYPE_VOID);
    t->func.param_types  = param_types;
    t->func.param_count  = param_count;
    t->func.is_variadic  = is_variadic;

    return t;
}

Type* type_enum_create(Arena* arena, StrView name, Type* underlying_type, EnumVariant* variants, size_t count) {
    Type* t = ARENA_NEW_ZERO(arena, Type);

    Type* base = underlying_type ? underlying_type : type_primitive(TYPE_U32);

    t->kind                        = TYPE_ENUM;
    t->size                        = base->size;
    t->align                       = base->align;
    t->loc                         = (SourceLoc){0};
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

int64_t int_truncate_to_width(int64_t val, size_t byte_size, bool is_signed) {
    switch (byte_size) {
        case 1:
            return is_signed ? (int64_t)(int8_t)val : (int64_t)(uint8_t)val;

        case 2:
            return is_signed ? (int64_t)(int16_t)val : (int64_t)(uint16_t)val;

        case 4:
            return is_signed ? (int64_t)(int32_t)val : (int64_t)(uint32_t)val;

        default:
            return val;
    }
}