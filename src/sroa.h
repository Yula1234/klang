#ifndef KLANG_SROA_H
#define KLANG_SROA_H

#include "ir.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

void sroa_run_on_function(Arena* arena, IRFunction* func);

void sroa_run_on_module(Arena* arena, IRModule* module);

#ifdef __cplusplus
}
#endif

#endif