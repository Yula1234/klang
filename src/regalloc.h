#ifndef KLANG_REGALLOC_H
#define KLANG_REGALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "arena.h"
#include "lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IRFunction IRFunction;
typedef struct IRModule   IRModule;

typedef enum X86Reg {
    REG_NONE = 0,
    REG_RAX,
    REG_RCX,
    REG_RDX,
    REG_RBX,
    REG_RSI,
    REG_RDI,
    REG_RSP,
    REG_RBP,
    REG_R8,
    REG_R9,
    REG_R10,
    REG_R11,
    REG_R12,
    REG_R13,
    REG_R14,
    REG_R15,
    REG_COUNT
} X86Reg;

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

const char*    reg_name(X86Reg reg, size_t byte_size);

bool           reg_is_callee_saved(X86Reg reg);

X86Reg         parse_reg_name(StrView name, size_t* out_byte_size);

RegAllocResult regalloc_run_on_function(Arena* arena, IRFunction* func);

void           regalloc_run_on_module(Arena* arena, IRModule* module);

#ifdef __cplusplus
}
#endif

#endif // KLANG_REGALLOC_H