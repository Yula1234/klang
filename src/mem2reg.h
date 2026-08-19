#ifndef KLANG_MEM2REG_H
#define KLANG_MEM2REG_H

#include "ir.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

void mem2reg_run_on_function(Arena* arena, IRFunction* func);

void mem2reg_run_on_module(Arena* arena, IRModule* module);

#ifdef __cplusplus
}
#endif

#endif