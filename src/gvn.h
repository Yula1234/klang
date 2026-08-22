#ifndef KLANG_GVN_H
#define KLANG_GVN_H

#include "ir.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

void gvn_run_on_function(Arena* arena, IRFunction* func);

void gvn_run_on_module(Arena* arena, IRModule* module);

#ifdef __cplusplus
}
#endif

#endif