#ifndef KLANG_OUT_OF_SSA_H
#define KLANG_OUT_OF_SSA_H

#include "ir.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

void out_of_ssa_run_on_function(Arena* arena, IRFunction* func);

void out_of_ssa_run_on_module(Arena* arena, IRModule* module);

#ifdef __cplusplus
}
#endif

#endif