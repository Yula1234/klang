#ifndef KLANG_REGALLOC_H
#define KLANG_REGALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "arena.h"
#include "lexer.h"
#include "x86.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IRFunction IRFunction;
typedef struct IRModule   IRModule;

typedef struct LiveInterval {
    uint32_t vreg_id;
    uint32_t start_inst;
    uint32_t end_inst;
    X86Reg   assigned_reg;
    X86Reg   hint_reg;
    int32_t  assigned_slot;
    bool     is_spilled;
    bool     is_active;
} LiveInterval;

typedef struct RegAllocResult {
    uint32_t callee_saved_mask;
    size_t   spill_slot_count;
} RegAllocResult;

RegAllocResult regalloc_run_on_function(Arena* arena, IRFunction* func);

void           regalloc_run_on_module(Arena* arena, IRModule* module);

#ifdef __cplusplus
}
#endif

#endif