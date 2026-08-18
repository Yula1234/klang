#ifndef KLANG_REGALLOC_H
#define KLANG_REGALLOC_H

#include "ir.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

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

IROperand      ir_op_reg(X86Reg reg, size_t byte_size, bool is_signed);

RegAllocResult regalloc_run_on_function(Arena* arena, IRFunction* func);
void           regalloc_run_on_module(Arena* arena, IRModule* module);

#ifdef __cplusplus
}
#endif

#endif // KLANG_REGALLOC_H