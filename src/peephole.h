#ifndef KLANG_PEEPHOLE_H
#define KLANG_PEEPHOLE_H

#include "ir.h"
#include "arena.h"
#include "regalloc.h"

#ifdef __cplusplus
extern "C" {
#endif

void peephole_run_on_function(Arena* arena, IRFunction* func);

void peephole_run_on_module(Arena* arena, IRModule* module);

#ifdef __cplusplus
}
#endif

#endif