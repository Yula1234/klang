#ifndef KLANG_LICM_H
#define KLANG_LICM_H

#include "ir.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

void licm_run_on_function(Arena* arena, IRFunction* func);

void licm_run_on_module(Arena* arena, IRModule* module);

#ifdef __cplusplus
}
#endif

#endif