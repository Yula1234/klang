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
    bool    valid;
} TrackedStackSlot;

typedef struct MachineBlockState {
    X86Reg           reg_alias[REG_COUNT];
    size_t           reg_size[REG_COUNT];
    bool             has_const[REG_COUNT];
    int64_t          const_val[REG_COUNT];
    X86Reg           reg_base[REG_COUNT];
    int64_t          reg_disp[REG_COUNT];
    bool             has_disp[REG_COUNT];
    TrackedStackSlot stack_slots[PEEPHOLE_MAX_STACK_TRACK];
    size_t           stack_slot_count;
} MachineBlockState;

static void state_reset(MachineBlockState* state) {
    for (size_t r = 0; r < REG_COUNT; ++r) {
        state->reg_alias[r] = (X86Reg)r;
        state->reg_size[r]  = 8;
        state->has_const[r] = false;
        state->const_val[r] = 0;
        state->reg_base[r]  = REG_NONE;
        state->reg_disp[r]  = 0;
        state->has_disp[r]  = false;
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

    X86Reg canon_target = get_canonical_reg(state, r);

    state->reg_alias[r] = r;
    state->has_const[r] = false;
    state->const_val[r] = 0;
    state->reg_size[r]  = 8;
    state->has_disp[r]  = false;
    state->reg_base[r]  = REG_NONE;
    state->reg_disp[r]  = 0;

    for (size_t i = 0; i < REG_COUNT; ++i) {
        if (state->reg_alias[i] == r || state->reg_alias[i] == canon_target) {
            state->reg_alias[i] = (X86Reg)i;
            state->has_const[i] = false;
            state->const_val[i] = 0;
        }

        if (state->has_disp[i] && (state->reg_base[i] == r || state->reg_base[i] == canon_target)) {
            state->has_disp[i]  = false;
            state->reg_base[i]  = REG_NONE;
            state->reg_disp[i]  = 0;
        }
    }

    for (size_t i = 0; i < state->stack_slot_count; ++i) {
        if (state->stack_slots[i].valid) {
            X86Reg slot_canon = get_canonical_reg(state, state->stack_slots[i].reg);

            if (state->stack_slots[i].reg == r || state->stack_slots[i].reg == canon_target ||
                slot_canon == r || slot_canon == canon_target) {
                state->stack_slots[i].valid = false;
            }
        }
    }
}

static void invalidate_caller_saved(MachineBlockState* state) {
    invalidate_register(state, REG_RDI);
    invalidate_register(state, REG_RSI);
    invalidate_register(state, REG_R8);
    invalidate_register(state, REG_R9);
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

static void record_stack_store(MachineBlockState* state, int32_t offset, size_t size, bool is_signed, X86Reg src) {
    if (src == REG_NONE) {
        return;
    }

    for (size_t i = 0; i < state->stack_slot_count; ++i) {
        if (state->stack_slots[i].valid && state->stack_slots[i].offset == offset) {
            state->stack_slots[i].size      = size;
            state->stack_slots[i].is_signed = is_signed;
            state->stack_slots[i].reg       = get_canonical_reg(state, src);
            return;
        }
    }

    if (state->stack_slot_count < PEEPHOLE_MAX_STACK_TRACK) {
        state->stack_slots[state->stack_slot_count] = (TrackedStackSlot){
            .offset    = offset,
            .size      = size,
            .is_signed = is_signed,
            .reg       = get_canonical_reg(state, src),
            .valid     = true
        };
        state->stack_slot_count++;
    }
}

static X86Reg find_stack_slot_reg(const MachineBlockState* state, int32_t offset, size_t size, bool is_signed) {
    for (size_t i = 0; i < state->stack_slot_count; ++i) {
        if (state->stack_slots[i].valid &&
            state->stack_slots[i].offset == offset &&
            state->stack_slots[i].size == size &&
            state->stack_slots[i].is_signed == is_signed) {
            return get_canonical_reg(state, state->stack_slots[i].reg);
        }
    }

    return REG_NONE;
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

static bool try_fold_mem_disp(const MachineBlockState* state, IROperand* addr_op, IROperand* disp_op) {
    if (addr_op->kind != IR_OP_REG) {
        return false;
    }

    X86Reg r = get_canonical_reg(state, (X86Reg)addr_op->reg);

    if (!state->has_disp[r]) {
        return false;
    }

    int64_t existing = (disp_op->kind == IR_OP_CONST) ? disp_op->int_val : 0;
    int64_t total    = state->reg_disp[r] + existing;

    if (total < -2147483648LL || total > 2147483647LL) {
        return false;
    }

    *addr_op = ir_op_reg(state->reg_base[r], 8, false);
    *disp_op = ir_op_const(total, 8, true);

    return true;
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
                        X86Reg cached_reg = find_stack_slot_reg(&state, inst->src1.stack_offset, inst->src1.byte_size, inst->src1.is_signed);

                        if (cached_reg != REG_NONE) {
                            X86Reg dst_r = (X86Reg)inst->dst.reg;

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

                        X86Reg dst_r = (X86Reg)inst->dst.reg;
                        invalidate_register(&state, dst_r);
                        record_stack_store(&state, inst->src1.stack_offset, inst->src1.byte_size, inst->src1.is_signed, dst_r);
                        continue;
                    }

                    if (inst->dst.kind == IR_OP_STACK && inst->src1.kind == IR_OP_REG) {
                        X86Reg src_r = (X86Reg)inst->src1.reg;
                        X86Reg existing = find_stack_slot_reg(&state, inst->dst.stack_offset, inst->dst.byte_size, inst->dst.is_signed);

                        if (existing != REG_NONE && existing == get_canonical_reg(&state, src_r)) {
                            inst->opcode = IR_NOP;
                            changed = true;
                            continue;
                        }

                        record_stack_store(&state, inst->dst.stack_offset, inst->dst.byte_size, inst->dst.is_signed, src_r);
                        continue;
                    }
                }

                if (inst->opcode == IR_ADD && inst->dst.kind == IR_OP_REG &&
                    inst->src1.kind == IR_OP_REG && inst->src2.kind == IR_OP_CONST) {

                    X86Reg dst_r  = (X86Reg)inst->dst.reg;
                    X86Reg src1_r = get_canonical_reg(&state, (X86Reg)inst->src1.reg);
                    int64_t delta = inst->src2.int_val;

                    if (dst_r != src1_r && delta >= -2147483648LL && delta <= 2147483647LL) {
                        X86Reg base_r = state.has_disp[src1_r] ? state.reg_base[src1_r] : src1_r;
                        int64_t total = state.has_disp[src1_r] ? (state.reg_disp[src1_r] + delta) : delta;

                        invalidate_register(&state, dst_r);

                        if (total >= -2147483648LL && total <= 2147483647LL) {
                            state.reg_base[dst_r] = base_r;
                            state.reg_disp[dst_r] = total;
                            state.has_disp[dst_r] = true;
                        }
                    } else {
                        invalidate_register(&state, dst_r);
                    }

                    continue;
                }

                if (inst->opcode == IR_LOAD) {
                    if (try_fold_mem_disp(&state, &inst->src1, &inst->src2)) {
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
                    if (try_fold_mem_disp(&state, &inst->dst, &inst->src2)) {
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
                    continue;
                }

                if (inst->opcode == IR_INLINE_ASM) {
                    invalidate_caller_saved(&state);
                    invalidate_all_memory(&state);
                    continue;
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