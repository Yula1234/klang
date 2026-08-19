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
    StructTypeEntry* next;
};

typedef struct EnumTypeEntry EnumTypeEntry;

struct EnumTypeEntry {
    StrView        name;
    Type*          type;
    EnumTypeEntry* next;
};

typedef struct Sema {
    Arena*           arena;
    Scope*           global_scope;
    Scope*           current_scope;
    StructTypeEntry* struct_registry;
    EnumTypeEntry*   enum_registry;
    AstProc*         current_proc;
    uint32_t         loop_depth;
    bool             had_error;
} Sema;

void sema_init(Sema* sema, Arena* arena);

bool sema_analyze_program(Sema* sema, AstProgram* program);

#ifdef __cplusplus
}
#endif

#endif // KLANG_SEMA_H