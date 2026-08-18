#ifndef KLANG_SEMA_H
#define KLANG_SEMA_H

#include <stdbool.h>

#include "ast.h"
#include "type.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScopeEntry {
    Symbol*            symbol;
    struct ScopeEntry* next;
} ScopeEntry;

typedef struct Scope {
    struct Scope* parent;
    ScopeEntry*   entries;
} Scope;

typedef struct Sema {
    Arena*    arena;
    Scope*    global_scope;
    Scope*    current_scope;
    AstProc*  current_proc;
    int32_t   current_stack_offset;
    uint32_t  loop_depth;
    bool      had_error;
} Sema;

void sema_init(Sema* sema, Arena* arena);

bool sema_analyze_program(Sema* sema, AstProgram* program);

#ifdef __cplusplus
}
#endif

#endif // KLANG_SEMA_H