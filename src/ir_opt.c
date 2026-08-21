#include "ir_opt.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static inline bool is_power_of_two(uint64_t val) {
    return val > 0 && (val & (val - 1)) == 0;
}

static inline uint32_t get_power_of_two_exp(uint64_t val) {
    uint32_t exp = 0;

    while ((val & 1) == 0) {
        val >>= 1;
        exp++;
    }

    return exp;
}

static int64_t truncate_int(int64_t val, size_t byte_size, bool is_signed) {
    switch (byte_size) {
        case 1:
            return is_signed ? (int64_t)(int8_t)val : (int64_t)(uint8_t)val;

        case 2:
            return is_signed ? (int64_t)(int16_t)val : (int64_t)(uint16_t)val;

        case 4:
            return is_signed ? (int64_t)(int32_t)val : (int64_t)(uint32_t)val;

        case 8:
        default:
            return is_signed ? val : (int64_t)(uint64_t)val;
    }
}

static bool inst_dst_is_read(IROpcode op) {
    return op == IR_STORE || op == IR_MEMCPY || op == IR_BR || op == IR_RET;
}

static bool inst_has_side_effects(const IRInst* inst) {
    IROpcode op = inst->opcode;
    if (op == IR_STORE || op == IR_MEMCPY || op == IR_JMP || op == IR_BR ||
        op == IR_RET || op == IR_CALL || op == IR_CALL_PTR ||
        op == IR_PARAM || op == IR_INLINE_ASM) {
        return true;
    }

    if (op == IR_MOV && (inst->dst.kind == IR_OP_STACK || inst->dst.kind == IR_OP_GLOBAL)) {
        return true;
    }

    return false;
}

static IROperand resolve_operand_from_table(const IROperand* table, size_t cap, IROperand op) {
    size_t depth          = 0;
    size_t orig_byte_size = op.byte_size;
    bool   orig_is_signed = op.is_signed;

    while (op.kind == IR_OP_VREG && op.vreg_id < cap && depth < 32) {
        IROperand mapped = table[op.vreg_id];

        if (mapped.kind == IR_OP_NONE) {
            break;
        }

        op = mapped;
        depth++;
    }

    if (orig_byte_size > 0) {
        op.byte_size = orig_byte_size;
        op.is_signed = orig_is_signed;
    }

    if (op.kind == IR_OP_CONST) {
        op.int_val = truncate_int(op.int_val, op.byte_size, op.is_signed);
    }

    return op;
}

static bool try_fold_constant_binary(IROpcode op, int64_t a, int64_t b, size_t size, bool is_signed, int64_t* out_val) {
    switch (op) {
        case IR_ADD:
            *out_val = a + b;
            return true;

        case IR_SUB:
            *out_val = a - b;
            return true;

        case IR_MUL:
            *out_val = a * b;
            return true;

        case IR_DIV:
            if (b == 0) return false;
            if (is_signed) {
                if (a == (-9223372036854775807LL - 1) && b == -1) return false;
                *out_val = a / b;
            } else {
                *out_val = (int64_t)((uint64_t)a / (uint64_t)b);
            }
            return true;

        case IR_MOD:
            if (b == 0) return false;
            if (is_signed) {
                if (a == (-9223372036854775807LL - 1) && b == -1) return false;
                *out_val = a % b;
            } else {
                *out_val = (int64_t)((uint64_t)a % (uint64_t)b);
            }
            return true;

        case IR_AND:
            *out_val = a & b;
            return true;

        case IR_OR:
            *out_val = a | b;
            return true;

        case IR_XOR:
            *out_val = a ^ b;
            return true;

        case IR_SHL: {
            uint32_t shift = (uint32_t)(b & (size == 8 ? 63 : 31));
            *out_val = a << shift;
            return true;
        }

        case IR_SHR: {
            uint32_t shift = (uint32_t)(b & (size == 8 ? 63 : 31));
            if (is_signed) {
                *out_val = a >> shift;
            } else {
                *out_val = (int64_t)(((uint64_t)a) >> shift);
            }
            return true;
        }

        case IR_CMP_EQ:
            *out_val = (a == b) ? 1 : 0;
            return true;

        case IR_CMP_NE:
            *out_val = (a != b) ? 1 : 0;
            return true;

        case IR_CMP_LT:
            *out_val = is_signed ? (a < b ? 1 : 0) : ((uint64_t)a < (uint64_t)b ? 1 : 0);
            return true;

        case IR_CMP_LE:
            *out_val = is_signed ? (a <= b ? 1 : 0) : ((uint64_t)a <= (uint64_t)b ? 1 : 0);
            return true;

        case IR_CMP_GT:
            *out_val = is_signed ? (a > b ? 1 : 0) : ((uint64_t)a > (uint64_t)b ? 1 : 0);
            return true;

        case IR_CMP_GE:
            *out_val = is_signed ? (a >= b ? 1 : 0) : ((uint64_t)a >= (uint64_t)b ? 1 : 0);
            return true;

        default:
            return false;
    }
}

static bool optimize_instruction(IRInst* inst) {
    if (inst->opcode == IR_NOP) {
        return false;
    }

    if (inst->opcode == IR_NEG && inst->src1.kind == IR_OP_CONST) {
        inst->opcode = IR_MOV;
        inst->src1   = ir_op_const(truncate_int(-inst->src1.int_val, inst->dst.byte_size, inst->dst.is_signed), inst->dst.byte_size, inst->dst.is_signed);
        inst->src2   = ir_op_none();
        return true;
    }

    if (inst->opcode == IR_NOT && inst->src1.kind == IR_OP_CONST) {
        inst->opcode = IR_MOV;
        inst->src1   = ir_op_const(truncate_int(~inst->src1.int_val, inst->dst.byte_size, inst->dst.is_signed), inst->dst.byte_size, inst->dst.is_signed);
        inst->src2   = ir_op_none();
        return true;
    }

    if (inst->src1.kind == IR_OP_CONST && inst->src2.kind == IR_OP_CONST) {
        int64_t res = 0;
        if (try_fold_constant_binary(inst->opcode, inst->src1.int_val, inst->src2.int_val, inst->dst.byte_size, inst->src1.is_signed, &res)) {
            inst->opcode = IR_MOV;
            inst->src1   = ir_op_const(truncate_int(res, inst->dst.byte_size, inst->dst.is_signed), inst->dst.byte_size, inst->dst.is_signed);
            inst->src2   = ir_op_none();
            return true;
        }
    }

    if (inst->src1.kind == IR_OP_VREG && inst->src2.kind == IR_OP_VREG && inst->src1.vreg_id == inst->src2.vreg_id) {
        switch (inst->opcode) {
            case IR_SUB:
            case IR_XOR:
                inst->opcode = IR_MOV;
                inst->src1   = ir_op_const(0, inst->dst.byte_size, inst->dst.is_signed);
                inst->src2   = ir_op_none();
                return true;

            case IR_AND:
            case IR_OR:
                inst->opcode = IR_MOV;
                inst->src2   = ir_op_none();
                return true;

            case IR_CMP_EQ:
            case IR_CMP_LE:
            case IR_CMP_GE:
                inst->opcode = IR_MOV;
                inst->src1   = ir_op_const(1, 1, false);
                inst->src2   = ir_op_none();
                return true;

            case IR_CMP_NE:
            case IR_CMP_LT:
            case IR_CMP_GT:
                inst->opcode = IR_MOV;
                inst->src1   = ir_op_const(0, 1, false);
                inst->src2   = ir_op_none();
                return true;

            default:
                break;
        }
    }

    if (inst->src2.kind == IR_OP_CONST) {
        int64_t c = inst->src2.int_val;

        if (inst->opcode == IR_ADD && c == 0) {
            inst->opcode = IR_MOV;
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_SUB && c == 0) {
            inst->opcode = IR_MOV;
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_MUL && c == 1) {
            inst->opcode = IR_MOV;
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_MUL && c == 0) {
            inst->opcode = IR_MOV;
            inst->src1   = ir_op_const(0, inst->dst.byte_size, inst->dst.is_signed);
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_DIV && c == 1) {
            inst->opcode = IR_MOV;
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_AND && c == 0) {
            inst->opcode = IR_MOV;
            inst->src1   = ir_op_const(0, inst->dst.byte_size, inst->dst.is_signed);
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_OR && c == 0) {
            inst->opcode = IR_MOV;
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_XOR && c == 0) {
            inst->opcode = IR_MOV;
            inst->src2   = ir_op_none();
            return true;
        }

        if ((inst->opcode == IR_SHL || inst->opcode == IR_SHR) && c == 0) {
            inst->opcode = IR_MOV;
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_MUL && c > 0 && is_power_of_two((uint64_t)c)) {
            uint32_t shift = get_power_of_two_exp((uint64_t)c);
            inst->opcode   = IR_SHL;
            inst->src2     = ir_op_const((int64_t)shift, 8, false);
            return true;
        }

        if (inst->opcode == IR_DIV && !inst->src1.is_signed && c > 0 && is_power_of_two((uint64_t)c)) {
            uint32_t shift = get_power_of_two_exp((uint64_t)c);
            inst->opcode   = IR_SHR;
            inst->src2     = ir_op_const((int64_t)shift, 8, false);
            return true;
        }

        if (inst->opcode == IR_MOD && !inst->src1.is_signed && c > 0 && is_power_of_two((uint64_t)c)) {
            int64_t mask = c - 1;
            inst->opcode = IR_AND;
            inst->src2   = ir_op_const(mask, inst->dst.byte_size, false);
            return true;
        }
    }

    if (inst->src1.kind == IR_OP_CONST) {
        int64_t c = inst->src1.int_val;

        if (inst->opcode == IR_ADD && c == 0) {
            inst->opcode = IR_MOV;
            inst->src1   = inst->src2;
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_MUL && c == 1) {
            inst->opcode = IR_MOV;
            inst->src1   = inst->src2;
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_MUL && c == 0) {
            inst->opcode = IR_MOV;
            inst->src1   = ir_op_const(0, inst->dst.byte_size, inst->dst.is_signed);
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_AND && c == 0) {
            inst->opcode = IR_MOV;
            inst->src1   = ir_op_const(0, inst->dst.byte_size, inst->dst.is_signed);
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_OR && c == 0) {
            inst->opcode = IR_MOV;
            inst->src1   = inst->src2;
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->opcode == IR_XOR && c == 0) {
            inst->opcode = IR_MOV;
            inst->src1   = inst->src2;
            inst->src2   = ir_op_none();
            return true;
        }
    }

    if (inst->opcode == IR_BR) {
        if (inst->dst.kind == IR_OP_CONST) {
            IRBlock* target = (inst->dst.int_val != 0) ? inst->src1.block : inst->src2.block;

            inst->opcode = IR_JMP;
            inst->dst    = ir_op_block(target);
            inst->src1   = ir_op_none();
            inst->src2   = ir_op_none();
            return true;
        }

        if (inst->src1.kind == IR_OP_BLOCK && inst->src2.kind == IR_OP_BLOCK && inst->src1.block == inst->src2.block) {
            inst->opcode = IR_JMP;
            inst->dst    = inst->src1;
            inst->src1   = ir_op_none();
            inst->src2   = ir_op_none();
            return true;
        }
    }

    return false;
}

void ir_opt_run_on_function(Arena* arena, IRFunction* func) {
    if (!func || !func->first_block) {
        return;
    }

    size_t cap = func->next_vreg_id + 1024;
    IROperand* subst_table = ARENA_NEW_ARRAY_ZERO(arena, IROperand, cap);
    uint32_t* use_counts   = ARENA_NEW_ARRAY_ZERO(arena, uint32_t, cap);
    uint32_t* def_counts   = ARENA_NEW_ARRAY_ZERO(arena, uint32_t, cap);

    bool changed = true;
    size_t iteration = 0;

    while (changed && iteration < 16) {
        changed = false;
        iteration++;

        if (func->next_vreg_id >= cap) {
            cap = func->next_vreg_id + 1024;
            subst_table = ARENA_NEW_ARRAY_ZERO(arena, IROperand, cap);
            use_counts  = ARENA_NEW_ARRAY_ZERO(arena, uint32_t, cap);
            def_counts  = ARENA_NEW_ARRAY_ZERO(arena, uint32_t, cap);
        } else {
            memset(subst_table, 0, sizeof(IROperand) * cap);
            memset(use_counts, 0, sizeof(uint32_t) * cap);
            memset(def_counts, 0, sizeof(uint32_t) * cap);
        }

        for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
            for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
                if (inst->opcode == IR_NOP) {
                    continue;
                }

                if (!inst_dst_is_read(inst->opcode) && inst->dst.kind == IR_OP_VREG && inst->dst.vreg_id < cap) {
                    def_counts[inst->dst.vreg_id]++;
                }
            }
        }

        for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
            for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
                if (inst->opcode == IR_NOP) {
                    continue;
                }

                if (inst_dst_is_read(inst->opcode)) {
                    IROperand resolved = resolve_operand_from_table(subst_table, cap, inst->dst);
                    if (memcmp(&resolved, &inst->dst, sizeof(IROperand)) != 0) {
                        inst->dst = resolved;
                        changed = true;
                    }
                }

                IROperand resolved_src1 = resolve_operand_from_table(subst_table, cap, inst->src1);
                if (memcmp(&resolved_src1, &inst->src1, sizeof(IROperand)) != 0) {
                    inst->src1 = resolved_src1;
                    changed = true;
                }

                IROperand resolved_src2 = resolve_operand_from_table(subst_table, cap, inst->src2);
                if (memcmp(&resolved_src2, &inst->src2, sizeof(IROperand)) != 0) {
                    inst->src2 = resolved_src2;
                    changed = true;
                }

                for (size_t i = 0; i < inst->extra_arg_count; ++i) {
                    IROperand resolved_extra = resolve_operand_from_table(subst_table, cap, inst->extra_args[i]);
                    if (memcmp(&resolved_extra, &inst->extra_args[i], sizeof(IROperand)) != 0) {
                        inst->extra_args[i] = resolved_extra;
                        changed = true;
                    }
                }

                for (size_t i = 0; i < inst->asm_input_count; ++i) {
                    IROperand resolved_asm = resolve_operand_from_table(subst_table, cap, inst->asm_inputs[i].val);
                    if (memcmp(&resolved_asm, &inst->asm_inputs[i].val, sizeof(IROperand)) != 0) {
                        inst->asm_inputs[i].val = resolved_asm;
                        changed = true;
                    }
                }

                if (optimize_instruction(inst)) {
                    changed = true;
                }

                if (inst->opcode == IR_MOV && inst->dst.kind == IR_OP_VREG && inst->dst.vreg_id < cap) {
                    if (def_counts[inst->dst.vreg_id] == 1) {
                        if (inst->src1.kind == IR_OP_CONST) {
                            subst_table[inst->dst.vreg_id] = inst->src1;
                        } else if (inst->src1.kind == IR_OP_VREG && inst->src1.vreg_id < cap && def_counts[inst->src1.vreg_id] == 1) {
                            if (inst->dst.byte_size == inst->src1.byte_size && inst->dst.is_signed == inst->src1.is_signed) {
                                subst_table[inst->dst.vreg_id] = inst->src1;
                            }
                        }
                    }
                }
            }
        }

        for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
            for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
                if (inst->opcode == IR_NOP) {
                    continue;
                }

                if (inst_dst_is_read(inst->opcode) && inst->dst.kind == IR_OP_VREG && inst->dst.vreg_id < cap) {
                    use_counts[inst->dst.vreg_id]++;
                }

                if (inst->opcode == IR_MOV && (inst->dst.kind == IR_OP_STACK || inst->dst.kind == IR_OP_GLOBAL)) {
                    if (inst->src1.kind == IR_OP_VREG && inst->src1.vreg_id < cap) {
                        use_counts[inst->src1.vreg_id]++;
                    }
                }

                if (inst->src1.kind == IR_OP_VREG && inst->src1.vreg_id < cap) {
                    use_counts[inst->src1.vreg_id]++;
                }

                if (inst->src2.kind == IR_OP_VREG && inst->src2.vreg_id < cap) {
                    use_counts[inst->src2.vreg_id]++;
                }

                for (size_t i = 0; i < inst->extra_arg_count; ++i) {
                    if (inst->extra_args[i].kind == IR_OP_VREG && inst->extra_args[i].vreg_id < cap) {
                        use_counts[inst->extra_args[i].vreg_id]++;
                    }
                }

                for (size_t i = 0; i < inst->asm_input_count; ++i) {
                    if (inst->asm_inputs[i].val.kind == IR_OP_VREG && inst->asm_inputs[i].val.vreg_id < cap) {
                        use_counts[inst->asm_inputs[i].val.vreg_id]++;
                    }
                }
            }
        }

        for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
            for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
                if (inst->opcode == IR_NOP || inst_has_side_effects(inst)) {
                    continue;
                }

                if (inst->dst.kind == IR_OP_VREG && inst->dst.vreg_id < cap) {
                    if (use_counts[inst->dst.vreg_id] == 0 && def_counts[inst->dst.vreg_id] >= 1) {
                        inst->opcode = IR_NOP;
                        inst->dst    = ir_op_none();
                        inst->src1   = ir_op_none();
                        inst->src2   = ir_op_none();
                        changed = true;
                    }
                }
            }
        }
    }

    ir_eliminate_nops(func);
}

void ir_opt_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        ir_opt_run_on_function(arena, f);
    }
}