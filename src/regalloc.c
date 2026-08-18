#include "regalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static const X86Reg ALLOCATABLE_REGS[] = {
    REG_RBX,
    REG_R12,
    REG_R13,
    REG_R14,
    REG_R15,
    REG_RSI,
    REG_RDI,
    REG_R8,
    REG_R9
};

#define ALLOCATABLE_REG_COUNT (sizeof(ALLOCATABLE_REGS) / sizeof(ALLOCATABLE_REGS[0]))

static const X86Reg CALLEE_SAVED_REGS_LIST[] = {
    REG_RBX,
    REG_R12,
    REG_R13,
    REG_R14,
    REG_R15
};

#define CALLEE_SAVED_COUNT (sizeof(CALLEE_SAVED_REGS_LIST) / sizeof(CALLEE_SAVED_REGS_LIST[0]))

const char* reg_name(X86Reg reg, size_t byte_size) {
    if (byte_size == 0) {
        byte_size = 8;
    }

    switch (reg) {
        case REG_RAX:
            if (byte_size == 1) return "al";
            if (byte_size == 2) return "ax";
            if (byte_size == 4) return "eax";
            return "rax";

        case REG_RCX:
            if (byte_size == 1) return "cl";
            if (byte_size == 2) return "cx";
            if (byte_size == 4) return "ecx";
            return "rcx";

        case REG_RDX:
            if (byte_size == 1) return "dl";
            if (byte_size == 2) return "dx";
            if (byte_size == 4) return "edx";
            return "rdx";

        case REG_RBX:
            if (byte_size == 1) return "bl";
            if (byte_size == 2) return "bx";
            if (byte_size == 4) return "ebx";
            return "rbx";

        case REG_RSI:
            if (byte_size == 1) return "sil";
            if (byte_size == 2) return "si";
            if (byte_size == 4) return "esi";
            return "rsi";

        case REG_RDI:
            if (byte_size == 1) return "dil";
            if (byte_size == 2) return "di";
            if (byte_size == 4) return "edi";
            return "rdi";

        case REG_RSP:
            if (byte_size == 1) return "spl";
            if (byte_size == 2) return "sp";
            if (byte_size == 4) return "esp";
            return "rsp";

        case REG_RBP:
            if (byte_size == 1) return "bpl";
            if (byte_size == 2) return "bp";
            if (byte_size == 4) return "ebp";
            return "rbp";

        case REG_R8:
            if (byte_size == 1) return "r8b";
            if (byte_size == 2) return "r8w";
            if (byte_size == 4) return "r8d";
            return "r8";

        case REG_R9:
            if (byte_size == 1) return "r9b";
            if (byte_size == 2) return "r9w";
            if (byte_size == 4) return "r9d";
            return "r9";

        case REG_R10:
            if (byte_size == 1) return "r10b";
            if (byte_size == 2) return "r10w";
            if (byte_size == 4) return "r10d";
            return "r10";

        case REG_R11:
            if (byte_size == 1) return "r11b";
            if (byte_size == 2) return "r11w";
            if (byte_size == 4) return "r11d";
            return "r11";

        case REG_R12:
            if (byte_size == 1) return "r12b";
            if (byte_size == 2) return "r12w";
            if (byte_size == 4) return "r12d";
            return "r12";

        case REG_R13:
            if (byte_size == 1) return "r13b";
            if (byte_size == 2) return "r13w";
            if (byte_size == 4) return "r13d";
            return "r13";

        case REG_R14:
            if (byte_size == 1) return "r14b";
            if (byte_size == 2) return "r14w";
            if (byte_size == 4) return "r14d";
            return "r14";

        case REG_R15:
            if (byte_size == 1) return "r15b";
            if (byte_size == 2) return "r15w";
            if (byte_size == 4) return "r15d";
            return "r15";

        default:
            return "unknown_reg";
    }
}

bool reg_is_callee_saved(X86Reg reg) {
    return (reg == REG_RBX || reg == REG_R12 || reg == REG_R13 || reg == REG_R14 || reg == REG_R15);
}

IROperand ir_op_reg(X86Reg reg, size_t byte_size, bool is_signed) {
    IROperand op;
    memset(&op, 0, sizeof(op));

    op.kind      = IR_OP_REG;
    op.byte_size = (byte_size == 0) ? 8 : byte_size;
    op.is_signed = is_signed;
    op.reg       = (uint32_t)reg;

    return op;
}

typedef struct FreeSlotNode {
    int32_t              slot_offset;
    struct FreeSlotNode* next;
} FreeSlotNode;

static void track_use(LiveInterval* intervals, const IROperand* op, uint32_t inst_idx) {
    if (!op || op->kind != IR_OP_VREG) {
        return;
    }

    uint32_t vid = op->vreg_id;
    LiveInterval* iv = &intervals[vid];

    if (iv->start_inst == 0) {
        iv->start_inst = inst_idx;
    }

    if (inst_idx > iv->end_inst) {
        iv->end_inst = inst_idx;
    }

    iv->is_active = true;
}

static void track_def(LiveInterval* intervals, const IROperand* op, uint32_t inst_idx) {
    if (!op || op->kind != IR_OP_VREG) {
        return;
    }

    uint32_t vid = op->vreg_id;
    LiveInterval* iv = &intervals[vid];

    if (iv->start_inst == 0) {
        iv->start_inst = inst_idx;
    }

    if (inst_idx > iv->end_inst) {
        iv->end_inst = inst_idx;
    }

    iv->is_active = true;
}

static void compute_liveness(Arena* arena, IRFunction* func, LiveInterval* intervals, uint32_t* block_start_idx, uint32_t* block_end_idx) {
    uint32_t inst_idx = 0;
    size_t call_cap = 0;
    size_t call_count = 0;
    uint32_t* call_indices = NULL;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        block_start_idx[b->id] = inst_idx + 2;

        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            inst_idx += 2;

            if (inst->opcode == IR_CALL) {
                ARENA_DA_PUSH(arena, call_indices, call_count, call_cap, inst_idx);
            }

            track_use(intervals, &inst->src1, inst_idx);
            track_use(intervals, &inst->src2, inst_idx);

            for (size_t i = 0; i < inst->extra_arg_count; ++i) {
                track_use(intervals, &inst->extra_args[i], inst_idx);
            }

            track_def(intervals, &inst->dst, inst_idx);
        }

        block_end_idx[b->id] = inst_idx;
    }

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_JMP && inst->dst.block && inst->dst.block->id <= b->id) {
                uint32_t target_start = block_start_idx[inst->dst.block->id];
                uint32_t loop_end     = block_end_idx[b->id];

                for (size_t i = 0; i < func->next_vreg_id; ++i) {
                    LiveInterval* iv = &intervals[i];

                    if (iv->is_active && iv->start_inst <= target_start && iv->end_inst >= target_start) {
                        if (iv->end_inst < loop_end) {
                            iv->end_inst = loop_end;
                        }
                    }
                }
            } else if (inst->opcode == IR_BR) {
                if (inst->src1.block && inst->src1.block->id <= b->id) {
                    uint32_t target_start = block_start_idx[inst->src1.block->id];
                    uint32_t loop_end     = block_end_idx[b->id];

                    for (size_t i = 0; i < func->next_vreg_id; ++i) {
                        LiveInterval* iv = &intervals[i];

                        if (iv->is_active && iv->start_inst <= target_start && iv->end_inst >= target_start) {
                            if (iv->end_inst < loop_end) {
                                iv->end_inst = loop_end;
                            }
                        }
                    }
                }

                if (inst->src2.block && inst->src2.block->id <= b->id) {
                    uint32_t target_start = block_start_idx[inst->src2.block->id];
                    uint32_t loop_end     = block_end_idx[b->id];

                    for (size_t i = 0; i < func->next_vreg_id; ++i) {
                        LiveInterval* iv = &intervals[i];

                        if (iv->is_active && iv->start_inst <= target_start && iv->end_inst >= target_start) {
                            if (iv->end_inst < loop_end) {
                                iv->end_inst = loop_end;
                            }
                        }
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < func->next_vreg_id; ++i) {
        LiveInterval* iv = &intervals[i];

        if (!iv->is_active) {
            continue;
        }

        for (size_t c = 0; c < call_count; ++c) {
            uint32_t c_idx = call_indices[c];

            if (iv->start_inst < c_idx && iv->end_inst > c_idx) {
                iv->is_spilled = false;
                iv->assigned_slot = 1;
                break;
            }
        }
    }
}

static int compare_intervals_by_start(const void* a, const void* b) {
    const LiveInterval* ia = *(const LiveInterval**)a;
    const LiveInterval* ib = *(const LiveInterval**)b;

    if (ia->start_inst != ib->start_inst) {
        return (ia->start_inst < ib->start_inst) ? -1 : 1;
    }

    return (ia->end_inst < ib->end_inst) ? -1 : 1;
}

static void expire_old_intervals(Arena* arena, LiveInterval* current, LiveInterval** active, size_t* active_count, bool* reg_in_use, FreeSlotNode** free_slots) {
    size_t new_active_count = 0;

    for (size_t i = 0; i < *active_count; ++i) {
        LiveInterval* iv = active[i];

        if (iv->end_inst < current->start_inst) {
            if (!iv->is_spilled && iv->assigned_reg != REG_NONE) {
                reg_in_use[iv->assigned_reg] = false;
            } else if (iv->is_spilled) {
                FreeSlotNode* node = ARENA_NEW(arena, FreeSlotNode);
                node->slot_offset  = iv->assigned_slot;
                node->next         = *free_slots;
                *free_slots        = node;
            }
        } else {
            active[new_active_count++] = iv;
        }
    }

    *active_count = new_active_count;
}

static void insert_active_sorted_by_end(LiveInterval** active, size_t* active_count, LiveInterval* current) {
    size_t i = *active_count;

    while (i > 0 && active[i - 1]->end_inst > current->end_inst) {
        active[i] = active[i - 1];
        i--;
    }

    active[i] = current;
    (*active_count)++;
}

static X86Reg get_free_register(const bool* reg_in_use, bool crosses_call) {
    if (crosses_call) {
        for (size_t i = 0; i < CALLEE_SAVED_COUNT; ++i) {
            X86Reg r = CALLEE_SAVED_REGS_LIST[i];

            if (!reg_in_use[r]) {
                return r;
            }
        }

        return REG_NONE;
    }

    for (size_t i = 0; i < ALLOCATABLE_REG_COUNT; ++i) {
        X86Reg r = ALLOCATABLE_REGS[i];

        if (!reg_in_use[r]) {
            return r;
        }
    }

    return REG_NONE;
}

static int32_t allocate_or_reuse_slot(IRFunction* func, FreeSlotNode** free_slots, size_t* spill_count) {
    if (*free_slots != NULL) {
        FreeSlotNode* node = *free_slots;
        int32_t slot       = node->slot_offset;
        *free_slots        = node->next;
        return slot;
    }

    (*spill_count)++;
    return ir_func_alloc_stack_slot(func, 8, 8);
}

static void rewrite_operand(IROperand* op, const LiveInterval* intervals) {
    if (!op || op->kind != IR_OP_VREG) {
        return;
    }

    uint32_t vid = op->vreg_id;
    const LiveInterval* iv = &intervals[vid];

    if (iv->is_spilled) {
        op->kind         = IR_OP_STACK;
        op->stack_offset = iv->assigned_slot;
    } else {
        *op = ir_op_reg(iv->assigned_reg, op->byte_size, op->is_signed);
    }
}

RegAllocResult regalloc_run_on_function(Arena* arena, IRFunction* func) {
    RegAllocResult result;
    result.callee_saved_mask = 0;
    result.spill_slot_count  = 0;

    if (!func || func->next_vreg_id == 0) {
        return result;
    }

    size_t vreg_count = func->next_vreg_id;
    LiveInterval* intervals = ARENA_NEW_ARRAY_ZERO(arena, LiveInterval, vreg_count);

    for (size_t i = 0; i < vreg_count; ++i) {
        intervals[i].vreg_id       = (uint32_t)i;
        intervals[i].start_inst    = 0;
        intervals[i].end_inst      = 0;
        intervals[i].assigned_reg  = REG_NONE;
        intervals[i].assigned_slot = 0;
        intervals[i].is_spilled    = false;
        intervals[i].is_active     = false;
    }

    size_t block_count = func->next_block_id + 1;
    uint32_t* block_start_idx = ARENA_NEW_ARRAY_ZERO(arena, uint32_t, block_count);
    uint32_t* block_end_idx   = ARENA_NEW_ARRAY_ZERO(arena, uint32_t, block_count);

    compute_liveness(arena, func, intervals, block_start_idx, block_end_idx);

    size_t active_intervals_count = 0;

    for (size_t i = 0; i < vreg_count; ++i) {
        if (intervals[i].is_active) {
            active_intervals_count++;
        }
    }

    if (active_intervals_count == 0) {
        return result;
    }

    LiveInterval** sorted_intervals = ARENA_NEW_ARRAY(arena, LiveInterval*, active_intervals_count);
    size_t sort_idx = 0;

    for (size_t i = 0; i < vreg_count; ++i) {
        if (intervals[i].is_active) {
            sorted_intervals[sort_idx++] = &intervals[i];
        }
    }

    qsort(sorted_intervals, active_intervals_count, sizeof(LiveInterval*), compare_intervals_by_start);

    LiveInterval** active = ARENA_NEW_ARRAY(arena, LiveInterval*, ALLOCATABLE_REG_COUNT + 1);
    size_t active_count = 0;

    bool reg_in_use[REG_COUNT];
    memset(reg_in_use, 0, sizeof(reg_in_use));

    FreeSlotNode* free_slots = NULL;

    for (size_t i = 0; i < active_intervals_count; ++i) {
        LiveInterval* current = sorted_intervals[i];
        bool crosses_call = (current->assigned_slot == 1);
        current->assigned_slot = 0;

        expire_old_intervals(arena, current, active, &active_count, reg_in_use, &free_slots);

        X86Reg free_reg = get_free_register(reg_in_use, crosses_call);

        if (free_reg != REG_NONE) {
            current->assigned_reg = free_reg;
            reg_in_use[free_reg]  = true;

            if (reg_is_callee_saved(free_reg)) {
                result.callee_saved_mask |= (1 << free_reg);
            }

            insert_active_sorted_by_end(active, &active_count, current);
        } else {
            current->is_spilled    = true;
            current->assigned_reg  = REG_NONE;
            current->assigned_slot = allocate_or_reuse_slot(func, &free_slots, &result.spill_slot_count);
        }
    }

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            rewrite_operand(&inst->src1, intervals);
            rewrite_operand(&inst->src2, intervals);

            for (size_t k = 0; k < inst->extra_arg_count; ++k) {
                rewrite_operand(&inst->extra_args[k], intervals);
            }

            rewrite_operand(&inst->dst, intervals);
        }
    }

    func->callee_saved_mask = result.callee_saved_mask;

    return result;
}

void regalloc_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        regalloc_run_on_function(arena, f);
    }
}