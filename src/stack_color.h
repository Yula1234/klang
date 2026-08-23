#ifndef KLANG_STACK_COLOR_H
#define KLANG_STACK_COLOR_H

#include "ir.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

void stack_color_run_on_function(Arena* arena, IRFunction* func);

void stack_color_run_on_module(Arena* arena, IRModule* module);

#ifdef __cplusplus
}
#endif

#endif