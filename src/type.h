#ifndef KLANG_TYPE_H
#define KLANG_TYPE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "lexer.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum TypeKind {
    TYPE_VOID = 0,
    TYPE_BOOL,
    TYPE_CHAR,

    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,

    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,

    TYPE_PTR,

    TYPE_STRUCT,
    TYPE_ARRAY,
    TYPE_FUNC,
    TYPE_ENUM
} TypeKind;

typedef struct Type Type;

typedef struct EnumVariant {
    StrView name;
    int64_t value;
} EnumVariant;

typedef struct StructField {
    StrView name;
    Type*   type;
    size_t  offset;
} StructField;

struct Type {
    TypeKind kind;
    size_t   size;
    size_t   align;

    union {
        struct {
            Type* base;
        } ptr;

        struct {
            StrView      name;
            StructField* fields;
            size_t       field_count;
            bool         is_packed;
        } structure;

        struct {
            Type*  elem_type;
            size_t count;
        } array;

        struct {
            Type*  return_type;
            Type** param_types;
            size_t param_count;
        } func;

        struct {
            StrView      name;
            Type*        underlying_type;
            EnumVariant* variants;
            size_t       variant_count;
        } enumeration;
    };
};

Type*       type_primitive(TypeKind kind);

Type*       type_ptr(Arena* arena, Type* base_type);

bool        type_equals(const Type* a, const Type* b);

bool        type_is_integer(const Type* type);

bool        type_is_signed(const Type* type);

bool        type_is_pointer(const Type* type);

size_t      type_pointer_depth(const Type* type);

const char* type_to_str(const Type* type, Arena* arena);

Type*       type_integer_promote(const Type* type);

Type*       type_common_arithmetic(const Type* a, const Type* b);

void        type_struct_init(Type* struct_type, StrView name, StructField* fields, size_t count, bool is_packed);
Type*       type_struct_create(Arena* arena, StrView name, StructField* fields, size_t count, bool is_packed);

StructField* type_struct_lookup_field(const Type* struct_type, StrView field_name);

Type*       type_array_create(Arena* arena, Type* elem_type, size_t count);

Type*       type_func_create(Arena* arena, Type* return_type, Type** param_types, size_t param_count);

Type*        type_enum_create(Arena* arena, StrView name, Type* underlying_type, EnumVariant* variants, size_t count);
EnumVariant* type_enum_lookup_variant(const Type* enum_type, StrView variant_name);

#ifdef __cplusplus
}
#endif

#endif // KLANG_TYPE_H