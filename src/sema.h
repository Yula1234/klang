#ifndef KLANG_SEMA_H
#define KLANG_SEMA_H

#include <stdbool.h>

#include "ast.h"
#include "type.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScopeEntry ScopeEntry;

struct ScopeEntry {
    Symbol*     symbol;
    ScopeEntry* next;
};

typedef struct Scope Scope;

struct Scope {
    Scope*      parent;
    ScopeEntry* entries;
};

typedef struct StructTypeEntry StructTypeEntry;

struct StructTypeEntry {
    StrView          name;
    Type*            type;
    bool             is_resolving;
    bool             is_resolved;
    StructTypeEntry* next;
};

typedef struct UnionTypeEntry UnionTypeEntry;

struct UnionTypeEntry {
    StrView         name;
    Type*           type;
    bool            is_resolving;
    bool            is_resolved;
    UnionTypeEntry* next;
};

typedef struct EnumTypeEntry EnumTypeEntry;

struct EnumTypeEntry {
    StrView        name;
    Type*          type;
    EnumTypeEntry* next;
};

typedef struct TypeAliasEntry TypeAliasEntry;

struct TypeAliasEntry {
    StrView         name;
    Type*           type;
    bool            is_distinct;
    bool            is_resolving;
    TypeAliasEntry* next;
};

typedef struct GenericStructTemplate {
    StrView                       name;
    AstStructDef*                 def;
    struct GenericStructTemplate* next;
} GenericStructTemplate;

typedef struct GenericProcTemplate {
    StrView                     name;
    AstProc*                    def;
    struct GenericProcTemplate* next;
} GenericProcTemplate;

typedef struct StructInstanceCache {
    Type*                       template_type;
    Type**                      args;
    size_t                      arg_count;
    Type*                       instantiated_type;
    struct StructInstanceCache* next;
} StructInstanceCache;

typedef struct ProcInstanceCache {
    AstProc*                  def_template;
    Type**                    args;
    size_t                    arg_count;
    AstProc*                  instantiated_proc;
    struct ProcInstanceCache* next;
} ProcInstanceCache;

typedef struct Sema {
    Arena*           arena;
    Scope*           global_scope;
    Scope*           current_scope;
    StructTypeEntry* struct_registry;
    UnionTypeEntry*  union_registry;
    EnumTypeEntry*         enum_registry;
    TypeAliasEntry*        alias_registry;
    GenericStructTemplate* generic_struct_templates;
    GenericProcTemplate*   generic_proc_templates;
    StructInstanceCache*   struct_instances;
    ProcInstanceCache*     proc_instances;
    const TypeSubstEnv*    current_subst_env;
    AstProgram*            current_program;
    AstProc*               current_proc;
    uint32_t               loop_depth;
    bool                   had_error;
} Sema;

void sema_init(Sema* sema, Arena* arena);

bool sema_analyze_program(Sema* sema, AstProgram* program);

#ifdef __cplusplus
}
#endif

#endif // KLANG_SEMA_H