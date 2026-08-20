#include "ir.h"
#include "regalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static const X86Reg CALLER_SAVED_REGS_LIST[] = {
    REG_RDI,
    REG_RSI,
    REG_R8,
    REG_R9
};

#define CALLER_SAVED_COUNT (sizeof(CALLER_SAVED_REGS_LIST) / sizeof(CALLER_SAVED_REGS_LIST[0]))

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

X86Reg parse_reg_name(StrView name, size_t* out_byte_size) {
    if (name.len == 0 || name.data == NULL) {
        return REG_NONE;
    }

    if (name.len == 2) {
        if (memcmp(name.data, "al", 2) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RAX; }
        if (memcmp(name.data, "cl", 2) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RCX; }
        if (memcmp(name.data, "dl", 2) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RDX; }
        if (memcmp(name.data, "bl", 2) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RBX; }

        if (memcmp(name.data, "ax", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RAX; }
        if (memcmp(name.data, "cx", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RCX; }
        if (memcmp(name.data, "dx", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RDX; }
        if (memcmp(name.data, "bx", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RBX; }
        if (memcmp(name.data, "si", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RSI; }
        if (memcmp(name.data, "di", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RDI; }
        if (memcmp(name.data, "sp", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RSP; }
        if (memcmp(name.data, "bp", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RBP; }

        if (memcmp(name.data, "r8", 2) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R8; }
        if (memcmp(name.data, "r9", 2) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R9; }
    }

    if (name.len == 3) {
        if (memcmp(name.data, "eax", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RAX; }
        if (memcmp(name.data, "ecx", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RCX; }
        if (memcmp(name.data, "edx", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RDX; }
        if (memcmp(name.data, "ebx", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RBX; }
        if (memcmp(name.data, "esi", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RSI; }
        if (memcmp(name.data, "edi", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RDI; }
        if (memcmp(name.data, "esp", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RSP; }
        if (memcmp(name.data, "ebp", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RBP; }

        if (memcmp(name.data, "rax", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RAX; }
        if (memcmp(name.data, "rcx", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RCX; }
        if (memcmp(name.data, "rdx", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RDX; }
        if (memcmp(name.data, "rbx", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RBX; }
        if (memcmp(name.data, "rsi", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RSI; }
        if (memcmp(name.data, "rdi", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RDI; }
        if (memcmp(name.data, "rsp", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RSP; }
        if (memcmp(name.data, "rbp", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RBP; }

        if (memcmp(name.data, "sil", 3) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RSI; }
        if (memcmp(name.data, "dil", 3) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RDI; }
        if (memcmp(name.data, "spl", 3) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RSP; }
        if (memcmp(name.data, "bpl", 3) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RBP; }

        if (memcmp(name.data, "r8b", 3) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R8; }
        if (memcmp(name.data, "r9b", 3) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R9; }
        if (memcmp(name.data, "r8w", 3) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R8; }
        if (memcmp(name.data, "r9w", 3) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R9; }
        if (memcmp(name.data, "r8d", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R8; }
        if (memcmp(name.data, "r9d", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R9; }

        if (memcmp(name.data, "r10", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R10; }
        if (memcmp(name.data, "r11", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R11; }
        if (memcmp(name.data, "r12", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R12; }
        if (memcmp(name.data, "r13", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R13; }
        if (memcmp(name.data, "r14", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R14; }
        if (memcmp(name.data, "r15", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R15; }
    }

    if (name.len == 4) {
        if (memcmp(name.data, "r10b", 4) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R10; }
        if (memcmp(name.data, "r11b", 4) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R11; }
        if (memcmp(name.data, "r12b", 4) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R12; }
        if (memcmp(name.data, "r13b", 4) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R13; }
        if (memcmp(name.data, "r14b", 4) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R14; }
        if (memcmp(name.data, "r15b", 4) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R15; }

        if (memcmp(name.data, "r10w", 4) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R10; }
        if (memcmp(name.data, "r11w", 4) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R11; }
        if (memcmp(name.data, "r12w", 4) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R12; }
        if (memcmp(name.data, "r13w", 4) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R13; }
        if (memcmp(name.data, "r14w", 4) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R14; }
        if (memcmp(name.data, "r15w", 4) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R15; }

        if (memcmp(name.data, "r10d", 4) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R10; }
        if (memcmp(name.data, "r11d", 4) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R11; }
        if (memcmp(name.data, "r12d", 4) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R12; }
        if (memcmp(name.data, "r13d", 4) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R13; }
        if (memcmp(name.data, "r14d", 4) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R14; }
        if (memcmp(name.data, "r15d", 4) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R15; }
    }

    return REG_NONE;
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

static void extend_liveness_for_backedge(LiveInterval* intervals, size_t vreg_count, uint32_t target_start, uint32_t loop_end) {
    for (size_t i = 0; i < vreg_count; ++i) {
        LiveInterval* iv = &intervals[i];

        if (!iv->is_active) {
            continue;
        }

        bool is_live_before_loop = (iv->start_inst <= target_start && iv->end_inst >= target_start);
        bool is_defined_in_loop  = (iv->start_inst >= target_start && iv->start_inst <= loop_end);

        if (is_live_before_loop || is_defined_in_loop) {
            if (iv->end_inst < loop_end) {
                iv->end_inst = loop_end;
            }
        }
    }
}

static void handle_potential_backedge(const IRBlock* target, const IRBlock* current_block,
                                      LiveInterval* intervals, size_t vreg_count,
                                      const uint32_t* block_start_idx, const uint32_t* block_end_idx) {
    if (target != NULL && target->id <= current_block->id) {
        uint32_t target_start = block_start_idx[target->id];
        uint32_t loop_end     = block_end_idx[current_block->id];

        extend_liveness_for_backedge(intervals, vreg_count, target_start, loop_end);
    }
}

static bool inst_dst_is_use(IROpcode op) {
    return op == IR_STORE || op == IR_MEMCPY || op == IR_BR || op == IR_RET;
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

            if (inst->opcode == IR_CALL || inst->opcode == IR_CALL_PTR) {
                ARENA_DA_PUSH(arena, call_indices, call_count, call_cap, inst_idx);

                for (size_t i = 0; i < inst->extra_arg_count && i < 6; ++i) {
                    if (inst->extra_args[i].kind == IR_OP_VREG) {
                        uint32_t vid = inst->extra_args[i].vreg_id;
                        if (i == 0 && intervals[vid].hint_reg == REG_NONE) intervals[vid].hint_reg = REG_RDI;
                        if (i == 1 && intervals[vid].hint_reg == REG_NONE) intervals[vid].hint_reg = REG_RSI;
                        if (i == 4 && intervals[vid].hint_reg == REG_NONE) intervals[vid].hint_reg = REG_R8;
                        if (i == 5 && intervals[vid].hint_reg == REG_NONE) intervals[vid].hint_reg = REG_R9;
                    }
                }
            }

            if (inst->opcode == IR_PARAM) {
                size_t param_idx = (size_t)inst->src1.int_val;
                if (inst->dst.kind == IR_OP_VREG) {
                    uint32_t vid = inst->dst.vreg_id;
                    if (param_idx == 0 && intervals[vid].hint_reg == REG_NONE) intervals[vid].hint_reg = REG_RDI;
                    if (param_idx == 1 && intervals[vid].hint_reg == REG_NONE) intervals[vid].hint_reg = REG_RSI;
                    if (param_idx == 4 && intervals[vid].hint_reg == REG_NONE) intervals[vid].hint_reg = REG_R8;
                    if (param_idx == 5 && intervals[vid].hint_reg == REG_NONE) intervals[vid].hint_reg = REG_R9;
                }
            }

            if (inst->opcode == IR_MOV && inst->dst.kind == IR_OP_VREG && inst->src1.kind == IR_OP_VREG) {
                uint32_t dst_vid = inst->dst.vreg_id;
                uint32_t src_vid = inst->src1.vreg_id;

                if (intervals[src_vid].hint_reg != REG_NONE && intervals[dst_vid].hint_reg == REG_NONE) {
                    intervals[dst_vid].hint_reg = intervals[src_vid].hint_reg;
                } else if (intervals[dst_vid].hint_reg != REG_NONE && intervals[src_vid].hint_reg == REG_NONE) {
                    intervals[src_vid].hint_reg = intervals[dst_vid].hint_reg;
                }
            }

            track_use(intervals, &inst->src1, inst_idx);
            track_use(intervals, &inst->src2, inst_idx);

            for (size_t i = 0; i < inst->extra_arg_count; ++i) {
                track_use(intervals, &inst->extra_args[i], inst_idx);
            }

            if (inst_dst_is_use(inst->opcode)) {
                track_use(intervals, &inst->dst, inst_idx);
            } else {
                track_def(intervals, &inst->dst, inst_idx);
            }
        }

        block_end_idx[b->id] = inst_idx;
    }

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_JMP) {
                handle_potential_backedge(inst->dst.block, b, intervals, func->next_vreg_id, block_start_idx, block_end_idx);
            } else if (inst->opcode == IR_BR) {
                handle_potential_backedge(inst->src1.block, b, intervals, func->next_vreg_id, block_start_idx, block_end_idx);
                handle_potential_backedge(inst->src2.block, b, intervals, func->next_vreg_id, block_start_idx, block_end_idx);
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

static X86Reg get_free_register(const bool* reg_in_use, bool crosses_call, X86Reg hint_reg) {
    if (hint_reg != REG_NONE && !reg_in_use[hint_reg]) {
        if (crosses_call) {
            if (reg_is_callee_saved(hint_reg)) {
                return hint_reg;
            }
        } else {
            return hint_reg;
        }
    }

    if (crosses_call) {
        for (size_t i = 0; i < CALLEE_SAVED_COUNT; ++i) {
            X86Reg r = CALLEE_SAVED_REGS_LIST[i];

            if (!reg_in_use[r]) {
                return r;
            }
        }

        return REG_NONE;
    }

    for (size_t i = 0; i < CALLER_SAVED_COUNT; ++i) {
        X86Reg r = CALLER_SAVED_REGS_LIST[i];

        if (!reg_in_use[r]) {
            return r;
        }
    }

    for (size_t i = 0; i < CALLEE_SAVED_COUNT; ++i) {
        X86Reg r = CALLEE_SAVED_REGS_LIST[i];

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
        intervals[i].hint_reg      = REG_NONE;
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

    LiveInterval** active = ARENA_NEW_ARRAY(arena, LiveInterval*, vreg_count + 1);
    size_t active_count = 0;

    bool reg_in_use[REG_COUNT];
    memset(reg_in_use, 0, sizeof(reg_in_use));

    FreeSlotNode* free_slots = NULL;

    for (size_t i = 0; i < active_intervals_count; ++i) {
        LiveInterval* current = sorted_intervals[i];
        bool crosses_call = (current->assigned_slot == 1);
        current->assigned_slot = 0;

        expire_old_intervals(arena, current, active, &active_count, reg_in_use, &free_slots);

        X86Reg free_reg = get_free_register(reg_in_use, crosses_call, current->hint_reg);

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

            insert_active_sorted_by_end(active, &active_count, current);
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