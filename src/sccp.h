#ifndef KLANG_SCCP_H
#define KLANG_SCCP_H

#include "ir.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

void sccp_run_on_function(Arena* arena, IRFunction* func);

void sccp_run_on_module(Arena* arena, IRModule* module);

#ifdef __cplusplus
}
#endif

#endif