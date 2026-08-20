#ifndef KLANG_IR_OPT_H
#define KLANG_IR_OPT_H

#include "ir.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

void ir_opt_run_on_function(Arena* arena, IRFunction* func);

void ir_opt_run_on_module(Arena* arena, IRModule* module);

#ifdef __cplusplus
}
#endif

#endif