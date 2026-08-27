#include "peephole.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define PEEPHOLE_MAX_STACK_TRACK 64

static inline bool inst_dst_is_read(IROpcode op) {
    return op == IR_STORE || op == IR_MEMCPY || op == IR_BR || op == IR_RET;
}

typedef struct TrackedStackSlot {
    int32_t offset;
    size_t  size;
    bool    is_signed;
    X86Reg  reg;
    bool    has_const;
    int64_t const_val;
    bool    valid;
} TrackedStackSlot;

typedef struct AddrState {
    X86Reg  base_reg;
    X86Reg  index_reg;
    uint8_t scale;
    int64_t disp;
    bool    valid;
} AddrState;

typedef struct MachineBlockState {
    X86Reg           reg_alias[REG_COUNT];
    size_t           reg_size[REG_COUNT];
    bool             has_const[REG_COUNT];
    int64_t          const_val[REG_COUNT];
    AddrState        addr[REG_COUNT];
    TrackedStackSlot stack_slots[PEEPHOLE_MAX_STACK_TRACK];
    size_t           stack_slot_count;
} MachineBlockState;

static void state_reset(MachineBlockState* state) {
    for (size_t r = 0; r < REG_COUNT; ++r) {
        state->reg_alias[r] = (X86Reg)r;
        state->reg_size[r]  = 8;
        state->has_const[r] = false;
        state->const_val[r] = 0;

        state->addr[r].base_reg  = REG_NONE;
        state->addr[r].index_reg = REG_NONE;
        state->addr[r].scale     = 0;
        state->addr[r].disp      = 0;
        state->addr[r].valid     = false;
    }

    state->stack_slot_count = 0;
}

static X86Reg get_canonical_reg(const MachineBlockState* state, X86Reg r) {
    size_t depth = 0;

    while (r != REG_NONE && state->reg_alias[r] != r && depth < 16) {
        r = state->reg_alias[r];
        depth++;
    }

    return r;
}

static void invalidate_register(MachineBlockState* state, X86Reg r) {
    if (r == REG_NONE || r >= REG_COUNT) {
        return;
    }

    X86Reg canon = get_canonical_reg(state, r);

    for (size_t i = 0; i < state->stack_slot_count; ++i) {
        if (state->stack_slots[i].valid) {
            X86Reg slot_r     = state->stack_slots[i].reg;
            X86Reg slot_canon = (slot_r != REG_NONE) ? get_canonical_reg(state, slot_r) : REG_NONE;

            if (slot_r == r || slot_r == canon || slot_canon == r || slot_canon == canon) {
                state->stack_slots[i].reg = REG_NONE;

                if (!state->stack_slots[i].has_const) {
                    state->stack_slots[i].valid = false;
                }
            }
        }
    }

    state->reg_alias[r] = r;
    state->has_const[r] = false;
    state->const_val[r] = 0;
    state->reg_size[r]  = 8;

    state->addr[r].base_reg  = REG_NONE;
    state->addr[r].index_reg = REG_NONE;
    state->addr[r].scale     = 0;
    state->addr[r].disp      = 0;
    state->addr[r].valid     = false;

    for (size_t i = 0; i < REG_COUNT; ++i) {
        if (state->reg_alias[i] == r || state->reg_alias[i] == canon) {
            state->reg_alias[i] = (X86Reg)i;
            state->has_const[i] = false;
            state->const_val[i] = 0;
        }

        if (state->addr[i].valid) {
            X86Reg b   = state->addr[i].base_reg;
            X86Reg idx = state->addr[i].index_reg;

            if (b == r || b == canon || idx == r || idx == canon) {
                state->addr[i].base_reg  = REG_NONE;
                state->addr[i].index_reg = REG_NONE;
                state->addr[i].scale     = 0;
                state->addr[i].disp      = 0;
                state->addr[i].valid     = false;
            }
        }
    }
}

static void invalidate_caller_saved(MachineBlockState* state) {
    invalidate_register(state, REG_RAX);
    invalidate_register(state, REG_RCX);
    invalidate_register(state, REG_RDX);
    invalidate_register(state, REG_RSI);
    invalidate_register(state, REG_RDI);
    invalidate_register(state, REG_R8);
    invalidate_register(state, REG_R9);
    invalidate_register(state, REG_R10);
    invalidate_register(state, REG_R11);
}

static void invalidate_all_memory(MachineBlockState* state) {
    state->stack_slot_count = 0;
}

static void record_reg_copy(MachineBlockState* state, X86Reg dst, X86Reg src, size_t size) {
    if (dst == REG_NONE || src == REG_NONE || dst == src) {
        return;
    }

    invalidate_register(state, dst);

    X86Reg canon_src = get_canonical_reg(state, src);

    state->reg_alias[dst] = canon_src;
    state->reg_size[dst]  = size;

    if (state->has_const[canon_src]) {
        state->has_const[dst] = true;
        state->const_val[dst] = state->const_val[canon_src];
    }

    if (state->addr[canon_src].valid &&
        state->addr[canon_src].base_reg != dst &&
        state->addr[canon_src].index_reg != dst) {
        state->addr[dst] = state->addr[canon_src];
    }
}

static void record_reg_const(MachineBlockState* state, X86Reg dst, int64_t val, size_t size) {
    if (dst == REG_NONE) {
        return;
    }

    invalidate_register(state, dst);

    state->has_const[dst] = true;
    state->const_val[dst] = val;
    state->reg_size[dst]  = size;
}

static void invalidate_stack_overlap(MachineBlockState* state, int32_t offset, size_t size) {
    int32_t start_a = offset;
    int32_t end_a   = offset + (int32_t)size;

    for (size_t i = 0; i < state->stack_slot_count; ++i) {
        if (!state->stack_slots[i].valid) {
            continue;
        }

        int32_t start_b = state->stack_slots[i].offset;
        int32_t end_b   = start_b + (int32_t)state->stack_slots[i].size;

        bool overlaps = (start_a < end_b && end_a > start_b);

        if (overlaps && (start_a != start_b || size != state->stack_slots[i].size)) {
            state->stack_slots[i].valid = false;
        }
    }
}

static void record_stack_store_reg(MachineBlockState* state, int32_t offset, size_t size, bool is_signed, X86Reg src) {
    if (src == REG_NONE) {
        return;
    }

    invalidate_stack_overlap(state, offset, size);

    X86Reg  canon_src = get_canonical_reg(state, src);
    bool    has_c     = state->has_const[canon_src];
    int64_t c_val     = state->const_val[canon_src];

    for (size_t i = 0; i < state->stack_slot_count; ++i) {
        if (state->stack_slots[i].offset == offset) {
            state->stack_slots[i].size      = size;
            state->stack_slots[i].is_signed = is_signed;
            state->stack_slots[i].reg       = canon_src;
            state->stack_slots[i].has_const = has_c;
            state->stack_slots[i].const_val = c_val;
            state->stack_slots[i].valid     = true;
            return;
        }
    }

    if (state->stack_slot_count < PEEPHOLE_MAX_STACK_TRACK) {
        state->stack_slots[state->stack_slot_count] = (TrackedStackSlot){
            .offset    = offset,
            .size      = size,
            .is_signed = is_signed,
            .reg       = canon_src,
            .has_const = has_c,
            .const_val = c_val,
            .valid     = true
        };

        state->stack_slot_count++;
    }
}

static void record_stack_store_const(MachineBlockState* state, int32_t offset, size_t size, bool is_signed, int64_t val) {
    invalidate_stack_overlap(state, offset, size);

    for (size_t i = 0; i < state->stack_slot_count; ++i) {
        if (state->stack_slots[i].offset == offset) {
            state->stack_slots[i].size      = size;
            state->stack_slots[i].is_signed = is_signed;
            state->stack_slots[i].reg       = REG_NONE;
            state->stack_slots[i].has_const = true;
            state->stack_slots[i].const_val = val;
            state->stack_slots[i].valid     = true;
            return;
        }
    }

    if (state->stack_slot_count < PEEPHOLE_MAX_STACK_TRACK) {
        state->stack_slots[state->stack_slot_count] = (TrackedStackSlot){
            .offset    = offset,
            .size      = size,
            .is_signed = is_signed,
            .reg       = REG_NONE,
            .has_const = true,
            .const_val = val,
            .valid     = true
        };

        state->stack_slot_count++;
    }
}

static const TrackedStackSlot* find_stack_slot(const MachineBlockState* state, int32_t offset, size_t size, bool is_signed) {
    for (size_t i = 0; i < state->stack_slot_count; ++i) {
        if (state->stack_slots[i].valid &&
            state->stack_slots[i].offset == offset &&
            state->stack_slots[i].size == size &&
            state->stack_slots[i].is_signed == is_signed) {

            return &state->stack_slots[i];
        }
    }

    return NULL;
}

static IRBlock* resolve_jump_target(IRBlock* target) {
    size_t depth = 0;

    while (target && depth < 16) {
        IRInst* inst = target->first_inst;

        while (inst && inst->opcode == IR_NOP) {
            inst = inst->next;
        }

        if (inst && inst->opcode == IR_JMP && inst->dst.kind == IR_OP_BLOCK && inst->dst.block && inst->dst.block != target) {
            target = inst->dst.block;
            depth++;
        } else {
            break;
        }
    }

    return target;
}

static bool try_fold_mem_sib(const MachineBlockState* state, IRInst* inst, IROperand* addr_op, IROperand* disp_op) {
    if (!addr_op || addr_op->kind != IR_OP_REG) {
        return false;
    }

    X86Reg r = get_canonical_reg(state, (X86Reg)addr_op->reg);

    if (r == REG_NONE || r >= REG_COUNT || !state->addr[r].valid) {
        return false;
    }

    const AddrState* a = &state->addr[r];

    int64_t existing_disp = (disp_op->kind == IR_OP_CONST) ? disp_op->int_val : 0;
    int64_t total_disp    = a->disp + existing_disp;

    if (total_disp < -2147483648LL || total_disp > 2147483647LL) {
        return false;
    }

    if (a->base_reg != REG_NONE) {
        *addr_op        = ir_op_reg(a->base_reg, 8, false);
        inst->mem_index = a->index_reg;
        inst->mem_scale = a->scale;

        if (total_disp != 0) {
            *disp_op = ir_op_const(total_disp, 8, true);
        } else {
            *disp_op = ir_op_none();
        }

        return true;
    }

    return false;
}

static inline bool is_pure_register_def(const IRInst* inst) {
    if (!inst || inst->dst.kind != IR_OP_REG) {
        return false;
    }

    switch (inst->opcode) {
        case IR_MOV:
            return (inst->src1.kind == IR_OP_REG || inst->src1.kind == IR_OP_CONST);

        case IR_ADDR:
        case IR_GLOBAL_STR:
            return true;

        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_AND:
        case IR_OR:
        case IR_XOR:
        case IR_SHL:
        case IR_SHR:
        case IR_NOT:
        case IR_NEG:
            return true;

        default:
            return false;
    }
}

static bool inst_reads_reg(const IRInst* inst, X86Reg reg) {
    if (!inst || reg == REG_NONE) {
        return false;
    }

    if (inst->src1.kind == IR_OP_REG && (X86Reg)inst->src1.reg == reg) {
        return true;
    }

    if (inst->src2.kind == IR_OP_REG && (X86Reg)inst->src2.reg == reg) {
        return true;
    }

    if (inst_dst_is_read(inst->opcode) && inst->dst.kind == IR_OP_REG && (X86Reg)inst->dst.reg == reg) {
        return true;
    }

    if (inst->mem_index != REG_NONE && inst->mem_index == reg) {
        return true;
    }

    for (size_t i = 0; i < inst->extra_arg_count; ++i) {
        if (inst->extra_args[i].kind == IR_OP_REG && (X86Reg)inst->extra_args[i].reg == reg) {
            return true;
        }
    }

    for (size_t i = 0; i < inst->asm_input_count; ++i) {
        if (inst->asm_inputs[i].val.kind == IR_OP_REG && (X86Reg)inst->asm_inputs[i].val.reg == reg) {
            return true;
        }
    }

    return false;
}

static bool inst_overwrites_reg(const IRInst* inst, X86Reg reg) {
    if (!inst || reg == REG_NONE || inst_dst_is_read(inst->opcode)) {
        return false;
    }

    if (inst->dst.kind == IR_OP_REG && (X86Reg)inst->dst.reg == reg) {
        return true;
    }

    return false;
}

static bool is_reg_def_dead(const IRInst* from_inst, X86Reg reg) {
    if (!from_inst || reg == REG_NONE) {
        return false;
    }

    for (const IRInst* curr = from_inst->next; curr != NULL; curr = curr->next) {
        if (curr->opcode == IR_NOP) {
            continue;
        }

        if (inst_reads_reg(curr, reg)) {
            return false;
        }

        if (inst_overwrites_reg(curr, reg)) {
            return true;
        }

        if (curr->opcode == IR_CALL || curr->opcode == IR_CALL_PTR) {
            if (!reg_is_callee_saved(reg)) {
                return true;
            }
            continue;
        }

        if (curr->opcode == IR_RET) {
            if (reg == REG_RAX) {
                return false;
            }
            return true;
        }

        if (curr->opcode == IR_TAIL_CALL || curr->opcode == IR_TAIL_CALL_PTR ||
            curr->opcode == IR_INLINE_ASM || curr->opcode == IR_JMP ||
            curr->opcode == IR_BR) {
            return false;
        }
    }

    return false;
}

void peephole_run_on_function(Arena* arena, IRFunction* func) {
    (void)arena;

    if (!func || !func->first_block) {
        return;
    }

    MachineBlockState state;
    bool changed = true;
    size_t pass = 0;

    while (changed && pass < 8) {
        changed = false;
        pass++;

        for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
            state_reset(&state);

            for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
                if (inst->opcode == IR_NOP) {
                    continue;
                }

                if (is_pure_register_def(inst) && is_reg_def_dead(inst, (X86Reg)inst->dst.reg)) {
                    inst->opcode = IR_NOP;
                    changed = true;
                    continue;
                }

                if (inst->opcode == IR_MOV) {
                    if (inst->dst.kind == IR_OP_REG && inst->src1.kind == IR_OP_REG) {
                        X86Reg dst_r = (X86Reg)inst->dst.reg;
                        X86Reg src_r = (X86Reg)inst->src1.reg;

                        if (dst_r == src_r) {
                            if (inst->dst.byte_size == inst->src1.byte_size) {
                                inst->opcode = IR_NOP;
                                changed = true;
                                continue;
                            }

                            if (!inst->src1.is_signed && state.reg_size[src_r] >= inst->dst.byte_size) {
                                inst->opcode = IR_NOP;
                                changed = true;
                                continue;
                            }
                        }

                        X86Reg canon_dst = get_canonical_reg(&state, dst_r);
                        X86Reg canon_src = get_canonical_reg(&state, src_r);

                        if (canon_dst == canon_src &&
                            state.reg_size[dst_r] >= inst->dst.byte_size &&
                            state.reg_size[src_r] >= inst->dst.byte_size &&
                            inst->dst.byte_size == inst->src1.byte_size) {

                            inst->opcode = IR_NOP;
                            changed = true;
                            continue;
                        }

                        record_reg_copy(&state, dst_r, src_r, inst->dst.byte_size);
                        continue;
                    }

                    if (inst->dst.kind == IR_OP_REG && inst->src1.kind == IR_OP_CONST) {
                        X86Reg dst_r = (X86Reg)inst->dst.reg;

                        if (state.has_const[dst_r] &&
                            state.const_val[dst_r] == inst->src1.int_val &&
                            state.reg_size[dst_r] == inst->dst.byte_size) {

                            inst->opcode = IR_NOP;
                            changed = true;
                            continue;
                        }

                        record_reg_const(&state, dst_r, inst->src1.int_val, inst->dst.byte_size);
                        continue;
                    }

                    if (inst->dst.kind == IR_OP_REG && inst->src1.kind == IR_OP_STACK) {
                        X86Reg dst_r = (X86Reg)inst->dst.reg;
                        const TrackedStackSlot* slot = find_stack_slot(&state, inst->src1.stack_offset, inst->src1.byte_size, inst->src1.is_signed);

                        if (slot != NULL) {
                            if (slot->reg != REG_NONE) {
                                X86Reg cached_reg = get_canonical_reg(&state, slot->reg);

                                if (dst_r == cached_reg) {
                                    inst->opcode = IR_NOP;
                                    changed = true;
                                    continue;
                                }

                                inst->src1 = ir_op_reg(cached_reg, inst->src1.byte_size, inst->src1.is_signed);
                                record_reg_copy(&state, dst_r, cached_reg, inst->dst.byte_size);
                                changed = true;
                                continue;
                            }

                            if (slot->has_const) {
                                inst->src1 = ir_op_const(slot->const_val, inst->src1.byte_size, inst->src1.is_signed);
                                record_reg_const(&state, dst_r, slot->const_val, inst->dst.byte_size);
                                changed = true;
                                continue;
                            }
                        }

                        invalidate_register(&state, dst_r);
                        record_stack_store_reg(&state, inst->src1.stack_offset, inst->src1.byte_size, inst->src1.is_signed, dst_r);
                        continue;
                    }

                    if (inst->dst.kind == IR_OP_STACK && inst->src1.kind == IR_OP_REG) {
                        X86Reg src_r = (X86Reg)inst->src1.reg;
                        const TrackedStackSlot* slot = find_stack_slot(&state, inst->dst.stack_offset, inst->dst.byte_size, inst->dst.is_signed);

                        if (slot != NULL && slot->reg != REG_NONE && get_canonical_reg(&state, slot->reg) == get_canonical_reg(&state, src_r)) {
                            inst->opcode = IR_NOP;
                            changed = true;
                            continue;
                        }

                        record_stack_store_reg(&state, inst->dst.stack_offset, inst->dst.byte_size, inst->dst.is_signed, src_r);
                        continue;
                    }

                    if (inst->dst.kind == IR_OP_STACK && inst->src1.kind == IR_OP_CONST) {
                        const TrackedStackSlot* slot = find_stack_slot(&state, inst->dst.stack_offset, inst->dst.byte_size, inst->dst.is_signed);

                        if (slot != NULL && slot->has_const && slot->const_val == inst->src1.int_val) {
                            inst->opcode = IR_NOP;
                            changed = true;
                            continue;
                        }

                        record_stack_store_const(&state, inst->dst.stack_offset, inst->dst.byte_size, inst->dst.is_signed, inst->src1.int_val);
                        continue;
                    }
                }

                if (inst->opcode == IR_SHL && inst->dst.kind == IR_OP_REG &&
                    inst->src1.kind == IR_OP_REG && inst->src2.kind == IR_OP_CONST) {

                    X86Reg  dst_r  = (X86Reg)inst->dst.reg;
                    X86Reg  src1_r = get_canonical_reg(&state, (X86Reg)inst->src1.reg);
                    int64_t shift  = inst->src2.int_val;

                    invalidate_register(&state, dst_r);

                    if (dst_r != src1_r && src1_r != REG_RSP && (shift == 1 || shift == 2 || shift == 3)) {
                        state.addr[dst_r] = (AddrState){
                            .base_reg  = REG_NONE,
                            .index_reg = src1_r,
                            .scale     = (uint8_t)(1 << shift),
                            .disp      = 0,
                            .valid     = true
                        };
                    }

                    continue;
                }

                if (inst->opcode == IR_MUL && inst->dst.kind == IR_OP_REG) {
                    X86Reg  dst_r     = (X86Reg)inst->dst.reg;
                    X86Reg  index_r   = REG_NONE;
                    uint8_t scale_val = 0;

                    if (inst->src1.kind == IR_OP_REG && inst->src2.kind == IR_OP_CONST) {
                        index_r = get_canonical_reg(&state, (X86Reg)inst->src1.reg);
                        if (inst->src2.int_val == 1 || inst->src2.int_val == 2 ||
                            inst->src2.int_val == 4 || inst->src2.int_val == 8) {
                            scale_val = (uint8_t)inst->src2.int_val;
                        }
                    } else if (inst->src1.kind == IR_OP_CONST && inst->src2.kind == IR_OP_REG) {
                        index_r = get_canonical_reg(&state, (X86Reg)inst->src2.reg);
                        if (inst->src1.int_val == 1 || inst->src1.int_val == 2 ||
                            inst->src1.int_val == 4 || inst->src1.int_val == 8) {
                            scale_val = (uint8_t)inst->src1.int_val;
                        }
                    }

                    invalidate_register(&state, dst_r);

                    if (dst_r != index_r && index_r != REG_NONE && index_r != REG_RSP && scale_val > 0) {
                        state.addr[dst_r] = (AddrState){
                            .base_reg  = REG_NONE,
                            .index_reg = index_r,
                            .scale     = scale_val,
                            .disp      = 0,
                            .valid     = true
                        };
                    }

                    continue;
                }

                if (inst->opcode == IR_ADD && inst->dst.kind == IR_OP_REG) {
                    X86Reg dst_r = (X86Reg)inst->dst.reg;

                    if (inst->src1.kind == IR_OP_REG && inst->src2.kind == IR_OP_CONST) {
                        X86Reg  src1_r = get_canonical_reg(&state, (X86Reg)inst->src1.reg);
                        int64_t delta  = inst->src2.int_val;

                        invalidate_register(&state, dst_r);

                        if (dst_r != src1_r) {
                            if (state.addr[src1_r].valid) {
                                int64_t total = state.addr[src1_r].disp + delta;

                                if (total >= -2147483648LL && total <= 2147483647LL) {
                                    state.addr[dst_r]      = state.addr[src1_r];
                                    state.addr[dst_r].disp = total;
                                }
                            } else if (delta >= -2147483648LL && delta <= 2147483647LL && src1_r != REG_RSP) {
                                state.addr[dst_r] = (AddrState){
                                    .base_reg  = src1_r,
                                    .index_reg = REG_NONE,
                                    .scale     = 0,
                                    .disp      = delta,
                                    .valid     = true
                                };
                            }
                        }

                        continue;
                    }

                    if (inst->src1.kind == IR_OP_REG && inst->src2.kind == IR_OP_REG) {
                        X86Reg s1 = get_canonical_reg(&state, (X86Reg)inst->src1.reg);
                        X86Reg s2 = get_canonical_reg(&state, (X86Reg)inst->src2.reg);

                        invalidate_register(&state, dst_r);

                        if (dst_r != s1 && dst_r != s2) {
                            if (state.addr[s1].valid && state.addr[s1].index_reg != REG_NONE &&
                                state.addr[s1].base_reg == REG_NONE && !state.addr[s2].valid && s2 != REG_RSP) {

                                state.addr[dst_r] = (AddrState){
                                    .base_reg  = s2,
                                    .index_reg = state.addr[s1].index_reg,
                                    .scale     = state.addr[s1].scale,
                                    .disp      = state.addr[s1].disp,
                                    .valid     = true
                                };
                            } else if (state.addr[s2].valid && state.addr[s2].index_reg != REG_NONE &&
                                       state.addr[s2].base_reg == REG_NONE && !state.addr[s1].valid && s1 != REG_RSP) {

                                state.addr[dst_r] = (AddrState){
                                    .base_reg  = s1,
                                    .index_reg = state.addr[s2].index_reg,
                                    .scale     = state.addr[s2].scale,
                                    .disp      = state.addr[s2].disp,
                                    .valid     = true
                                };
                            } else if (!state.addr[s1].valid && !state.addr[s2].valid && s1 != REG_RSP && s2 != REG_RSP) {
                                state.addr[dst_r] = (AddrState){
                                    .base_reg  = s1,
                                    .index_reg = s2,
                                    .scale     = 1,
                                    .disp      = 0,
                                    .valid     = true
                                };
                            }
                        }

                        continue;
                    }
                }

                if (inst->opcode == IR_LOAD) {
                    if (try_fold_mem_sib(&state, inst, &inst->src1, &inst->src2)) {
                        changed = true;
                    }

                    if (inst->dst.kind == IR_OP_REG) {
                        X86Reg dst_r = (X86Reg)inst->dst.reg;
                        invalidate_register(&state, dst_r);

                        if (!inst->dst.is_signed) {
                            state.reg_size[dst_r] = 8;
                        }
                    }

                    continue;
                }

                if (inst->opcode == IR_STORE) {
                    if (try_fold_mem_sib(&state, inst, &inst->dst, &inst->src2)) {
                        changed = true;
                    }

                    invalidate_all_memory(&state);
                    continue;
                }

                if (inst->opcode >= IR_ADD && inst->opcode <= IR_SHR) {
                    if (inst->dst.kind == IR_OP_REG && inst->src1.kind == IR_OP_REG && inst->next != NULL) {
                        IRInst* next_inst = inst->next;

                        while (next_inst && next_inst->opcode == IR_NOP) {
                            next_inst = next_inst->next;
                        }

                        if (next_inst && next_inst->opcode == IR_MOV &&
                            next_inst->dst.kind == IR_OP_REG && next_inst->src1.kind == IR_OP_REG) {

                            if (next_inst->src1.reg == inst->dst.reg &&
                                next_inst->dst.reg == inst->src1.reg &&
                                next_inst->dst.byte_size == inst->dst.byte_size) {

                                inst->dst          = next_inst->dst;
                                next_inst->opcode  = IR_NOP;
                                changed            = true;
                            }
                        }
                    }
                }

                if (inst->opcode == IR_CALL || inst->opcode == IR_CALL_PTR ||
                    inst->opcode == IR_TAIL_CALL || inst->opcode == IR_TAIL_CALL_PTR) {
                    invalidate_caller_saved(&state);
                    invalidate_all_memory(&state);
                }

                if (inst->opcode == IR_INLINE_ASM) {
                    invalidate_caller_saved(&state);
                    invalidate_all_memory(&state);
                }

                if (inst->opcode == IR_MEMCPY) {
                    invalidate_all_memory(&state);
                    continue;
                }

                if (inst->opcode == IR_STORE) {
                    invalidate_all_memory(&state);
                }

                if (inst->opcode == IR_JMP && inst->dst.kind == IR_OP_BLOCK) {
                    IRBlock* resolved = resolve_jump_target(inst->dst.block);

                    if (resolved != inst->dst.block) {
                        inst->dst.block = resolved;
                        changed = true;
                    }

                    if (inst->dst.block == b->next_block) {
                        inst->opcode = IR_NOP;
                        changed = true;
                        continue;
                    }
                }

                if (inst->opcode == IR_BR) {
                    if (inst->src1.kind == IR_OP_BLOCK) {
                        IRBlock* res_then = resolve_jump_target(inst->src1.block);

                        if (res_then != inst->src1.block) {
                            inst->src1.block = res_then;
                            changed = true;
                        }
                    }

                    if (inst->src2.kind == IR_OP_BLOCK) {
                        IRBlock* res_else = resolve_jump_target(inst->src2.block);

                        if (res_else != inst->src2.block) {
                            inst->src2.block = res_else;
                            changed = true;
                        }
                    }

                    if (inst->src1.kind == IR_OP_BLOCK && inst->src2.kind == IR_OP_BLOCK && inst->src1.block == inst->src2.block) {
                        inst->opcode = IR_JMP;
                        inst->dst    = inst->src1;
                        inst->src1   = ir_op_none();
                        inst->src2   = ir_op_none();
                        changed      = true;

                        if (inst->dst.block == b->next_block) {
                            inst->opcode = IR_NOP;
                        }

                        continue;
                    }
                }

                if (inst->dst.kind == IR_OP_REG && !inst_dst_is_read(inst->opcode)) {
                    invalidate_register(&state, (X86Reg)inst->dst.reg);
                }
            }
        }
    }

    ir_eliminate_nops(func);
}

void peephole_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        peephole_run_on_function(arena, f);
    }
}