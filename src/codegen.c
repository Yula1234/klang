#include "codegen.h"
#include "regalloc.h"
#include "lexer.h"
#include "abi.h"
#include "x86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static const X86Reg CALLEE_SAVED_REGS[] = {
    REG_RBX,
    REG_R12,
    REG_R13,
    REG_R14,
    REG_R15
};

typedef struct ArgMove {
    X86Reg    dst_reg;
    IROperand src_op;
    bool      done;
} ArgMove;

static const char* x86_size_prefix(size_t bytes) {
    switch (bytes) {
        case 1:  return "byte";
        case 2:  return "word";
        case 4:  return "dword";
        default: return "qword";
    }
}

static const char* x86_reg_name(const char* reg64, size_t bytes) {
    if (bytes == 0 || bytes >= 8) return reg64;

    if (strcmp(reg64, "rax") == 0) return (bytes == 1) ? "al"   : (bytes == 2) ? "ax"   : "eax";
    if (strcmp(reg64, "rcx") == 0) return (bytes == 1) ? "cl"   : (bytes == 2) ? "cx"   : "ecx";
    if (strcmp(reg64, "rdx") == 0) return (bytes == 1) ? "dl"   : (bytes == 2) ? "dx"   : "edx";
    if (strcmp(reg64, "rbx") == 0) return (bytes == 1) ? "bl"   : (bytes == 2) ? "bx"   : "ebx";
    if (strcmp(reg64, "rsi") == 0) return (bytes == 1) ? "sil"  : (bytes == 2) ? "si"   : "esi";
    if (strcmp(reg64, "rdi") == 0) return (bytes == 1) ? "dil"  : (bytes == 2) ? "di"   : "edi";
    if (strcmp(reg64, "rsp") == 0) return (bytes == 1) ? "spl"  : (bytes == 2) ? "sp"   : "esp";
    if (strcmp(reg64, "rbp") == 0) return (bytes == 1) ? "bpl"  : (bytes == 2) ? "bp"   : "ebp";
    if (strcmp(reg64, "r8")  == 0) return (bytes == 1) ? "r8b"  : (bytes == 2) ? "r8w"  : "r8d";
    if (strcmp(reg64, "r9")  == 0) return (bytes == 1) ? "r9b"  : (bytes == 2) ? "r9w"  : "r9d";
    if (strcmp(reg64, "r10") == 0) return (bytes == 1) ? "r10b" : (bytes == 2) ? "r10w" : "r10d";
    if (strcmp(reg64, "r11") == 0) return (bytes == 1) ? "r11b" : (bytes == 2) ? "r11w" : "r11d";
    if (strcmp(reg64, "r12") == 0) return (bytes == 1) ? "r12b" : (bytes == 2) ? "r12w" : "r12d";
    if (strcmp(reg64, "r13") == 0) return (bytes == 1) ? "r13b" : (bytes == 2) ? "r13w" : "r13d";
    if (strcmp(reg64, "r14") == 0) return (bytes == 1) ? "r14b" : (bytes == 2) ? "r14w" : "r14d";
    if (strcmp(reg64, "r15") == 0) return (bytes == 1) ? "r15b" : (bytes == 2) ? "r15w" : "r15d";

    return reg64;
}

static void format_stack_offset(char* buf, size_t buf_size, int32_t off) {
    if (off >= 0) {
        snprintf(buf, buf_size, "[rbp + %d]", off);
    } else {
        snprintf(buf, buf_size, "[rbp - %d]", -off);
    }
}

static size_t get_callee_saved_count(const IRFunction* func) {
    size_t count = 0;

    for (size_t i = 0; i < 5; ++i) {
        if (func->callee_saved_mask & (1 << CALLEE_SAVED_REGS[i])) {
            count++;
        }
    }

    return count;
}

static inline int32_t get_effective_stack_offset(const IRFunction* func, int32_t offset) {
    if (offset > 0) {
        return offset + (int32_t)(get_callee_saved_count(func) * 8);
    }
    return offset;
}

static void emit_callee_saved_push(FILE* out, const IRFunction* func) {
    for (size_t i = 0; i < 5; ++i) {
        if (func->callee_saved_mask & (1 << CALLEE_SAVED_REGS[i])) {
            fprintf(out, "    push %s\n", reg_name(CALLEE_SAVED_REGS[i], 8));
        }
    }
}

static void emit_callee_saved_pop(FILE* out, const IRFunction* func) {
    for (int i = 4; i >= 0; --i) {
        if (func->callee_saved_mask & (1 << CALLEE_SAVED_REGS[i])) {
            fprintf(out, "    pop %s\n", reg_name(CALLEE_SAVED_REGS[i], 8));
        }
    }
}

static inline int32_t get_vreg_stack_offset(const IRFunction* func, uint32_t vreg_id) {
    size_t base_offset = func->stack_frame_size;
    return -(int32_t)(base_offset + (vreg_id + 1) * 8);
}

static inline size_t get_total_function_stack_size(const IRFunction* func) {
    size_t saved_count = get_callee_saved_count(func);

    if (saved_count % 2 == 1) {
        return ((func->stack_frame_size + 8 + 15) & ~15) - 8;
    }

    return (func->stack_frame_size + 15) & ~15;
}

static inline bool is_signed_imm32(int64_t val) {
    return val >= -2147483648LL && val <= 2147483647LL;
}

static bool func_needs_frame_pointer(const IRFunction* func) {
    if (func->stack_frame_size > 0) {
        return true;
    }

    for (const IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (const IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->dst.kind == IR_OP_STACK ||
                inst->src1.kind == IR_OP_STACK ||
                inst->src2.kind == IR_OP_STACK) {
                return true;
            }

            for (size_t i = 0; i < inst->extra_arg_count; ++i) {
                if (inst->extra_args[i].kind == IR_OP_STACK) {
                    return true;
                }
            }

            for (size_t i = 0; i < inst->asm_input_count; ++i) {
                if (inst->asm_inputs[i].val.kind == IR_OP_STACK) {
                    return true;
                }
            }
        }
    }

    return false;
}

static void format_memory_address(char* buf, size_t buf_size, const IRFunction* func,
                                  const IROperand* base_op, X86Reg index_reg, uint8_t scale, int64_t disp) {

    if (!base_op || base_op->kind == IR_OP_NONE) {
        if (index_reg != REG_NONE) {
            const char* idx_r = reg_name(index_reg, 8);

            if (scale > 1) {
                if (disp == 0) {
                    snprintf(buf, buf_size, "[%s*%u]", idx_r, scale);
                } else if (disp > 0) {
                    snprintf(buf, buf_size, "[%s*%u + %lld]", idx_r, scale, (long long)disp);
                } else {
                    snprintf(buf, buf_size, "[%s*%u - %lld]", idx_r, scale, (long long)(-disp));
                }
            } else {
                if (disp == 0) {
                    snprintf(buf, buf_size, "[%s]", idx_r);
                } else if (disp > 0) {
                    snprintf(buf, buf_size, "[%s + %lld]", idx_r, (long long)disp);
                } else {
                    snprintf(buf, buf_size, "[%s - %lld]", idx_r, (long long)(-disp));
                }
            }
            return;
        }

        snprintf(buf, buf_size, "[%lld]", (long long)disp);
        return;
    }

    if (base_op->kind == IR_OP_REG) {
        const char* base_r = reg_name((X86Reg)base_op->reg, 8);

        if (index_reg != REG_NONE) {
            const char* idx_r = reg_name(index_reg, 8);

            if (scale > 1) {
                if (disp == 0) {
                    snprintf(buf, buf_size, "[%s + %s*%u]", base_r, idx_r, scale);
                } else if (disp > 0) {
                    snprintf(buf, buf_size, "[%s + %s*%u + %lld]", base_r, idx_r, scale, (long long)disp);
                } else {
                    snprintf(buf, buf_size, "[%s + %s*%u - %lld]", base_r, idx_r, scale, (long long)(-disp));
                }
            } else {
                if (disp == 0) {
                    snprintf(buf, buf_size, "[%s + %s]", base_r, idx_r);
                } else if (disp > 0) {
                    snprintf(buf, buf_size, "[%s + %s + %lld]", base_r, idx_r, (long long)disp);
                } else {
                    snprintf(buf, buf_size, "[%s + %s - %lld]", base_r, idx_r, (long long)(-disp));
                }
            }
            return;
        }

        if (disp == 0) {
            snprintf(buf, buf_size, "[%s]", base_r);
        } else if (disp > 0) {
            snprintf(buf, buf_size, "[%s + %lld]", base_r, (long long)disp);
        } else {
            snprintf(buf, buf_size, "[%s - %lld]", base_r, (long long)(-disp));
        }
        return;
    }

    if (base_op->kind == IR_OP_GLOBAL) {
        StrView gname = base_op->global_name;

        if (index_reg != REG_NONE) {
            const char* idx_r = reg_name(index_reg, 8);

            if (scale > 1) {
                if (disp == 0) {
                    snprintf(buf, buf_size, "[%.*s + %s*%u]", (int)gname.len, gname.data, idx_r, scale);
                } else if (disp > 0) {
                    snprintf(buf, buf_size, "[%.*s + %s*%u + %lld]", (int)gname.len, gname.data, idx_r, scale, (long long)disp);
                } else {
                    snprintf(buf, buf_size, "[%.*s + %s*%u - %lld]", (int)gname.len, gname.data, idx_r, scale, (long long)(-disp));
                }
            } else {
                if (disp == 0) {
                    snprintf(buf, buf_size, "[%.*s + %s]", (int)gname.len, gname.data, idx_r);
                } else if (disp > 0) {
                    snprintf(buf, buf_size, "[%.*s + %s + %lld]", (int)gname.len, gname.data, idx_r, (long long)disp);
                } else {
                    snprintf(buf, buf_size, "[%.*s + %s - %lld]", (int)gname.len, gname.data, idx_r, (long long)(-disp));
                }
            }
            return;
        }

        if (disp == 0) {
            snprintf(buf, buf_size, "[%.*s]", (int)gname.len, gname.data);
        } else if (disp > 0) {
            snprintf(buf, buf_size, "[%.*s + %lld]", (int)gname.len, gname.data, (long long)disp);
        } else {
            snprintf(buf, buf_size, "[%.*s - %lld]", (int)gname.len, gname.data, (long long)(-disp));
        }
        return;
    }

    if (base_op->kind == IR_OP_STACK) {
        int32_t off = get_effective_stack_offset(func, base_op->stack_offset) + (int32_t)disp;

        if (off >= 0) {
            snprintf(buf, buf_size, "[rbp + %d]", off);
        } else {
            snprintf(buf, buf_size, "[rbp - %d]", -off);
        }
        return;
    }

    snprintf(buf, buf_size, "[0]");
}

static void emit_load_operand(FILE* out, const IRFunction* func, const IROperand* op, const char* target_reg) {
    switch (op->kind) {
        case IR_OP_CONST: {
            if (op->int_val == 0) {
                const char* r32 = x86_reg_name(target_reg, 4);
                fprintf(out, "    xor %s, %s\n", r32, r32);
            } else {
                fprintf(out, "    mov %s, %lld\n", target_reg, (long long)op->int_val);
            }
            break;
        }

        case IR_OP_REG: {
            size_t size = op->byte_size ? op->byte_size : 8;
            const char* src_r = reg_name((X86Reg)op->reg, size);
            const char* dst_r = x86_reg_name(target_reg, size);

            if (strcmp(src_r, dst_r) != 0) {
                if (size == 1) {
                    const char* inst = op->is_signed ? "movsx" : "movzx";
                    fprintf(out, "    %s %s, %s\n", inst, x86_reg_name(target_reg, 4), src_r);
                } else if (size == 2) {
                    const char* inst = op->is_signed ? "movsx" : "movzx";
                    fprintf(out, "    %s %s, %s\n", inst, x86_reg_name(target_reg, 4), src_r);
                } else if (size == 4) {
                    if (op->is_signed) {
                        fprintf(out, "    movsxd %s, %s\n", target_reg, src_r);
                    } else {
                        fprintf(out, "    mov %s, %s\n", dst_r, src_r);
                    }
                } else {
                    fprintf(out, "    mov %s, %s\n", target_reg, src_r);
                }
            }
            break;
        }

        case IR_OP_VREG: {
            int32_t off = get_vreg_stack_offset(func, op->vreg_id);
            fprintf(out, "    mov %s, [rbp %d]\n", target_reg, off);
            break;
        }

        case IR_OP_STACK: {
            int32_t off = get_effective_stack_offset(func, op->stack_offset);
            char mem_op[64];
            format_stack_offset(mem_op, sizeof(mem_op), off);
            size_t size = op->byte_size ? op->byte_size : 8;

            if (size == 1) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, byte %s\n", inst, x86_reg_name(target_reg, 4), mem_op);
            } else if (size == 2) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, word %s\n", inst, x86_reg_name(target_reg, 4), mem_op);
            } else if (size == 4) {
                if (op->is_signed) {
                    fprintf(out, "    movsxd %s, dword %s\n", target_reg, mem_op);
                } else {
                    const char* reg32 = x86_reg_name(target_reg, 4);
                    fprintf(out, "    mov %s, dword %s\n", reg32, mem_op);
                }
            } else {
                fprintf(out, "    mov %s, qword %s\n", target_reg, mem_op);
            }
            break;
        }

        case IR_OP_GLOBAL: {
            StrView gname = op->global_name;
            size_t size = op->byte_size ? op->byte_size : 8;

            if (size == 1) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, byte [%.*s]\n", inst, x86_reg_name(target_reg, 4), (int)gname.len, gname.data);
            } else if (size == 2) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, word [%.*s]\n", inst, x86_reg_name(target_reg, 4), (int)gname.len, gname.data);
            } else if (size == 4) {
                if (op->is_signed) {
                    fprintf(out, "    movsxd %s, dword [%.*s]\n", target_reg, (int)gname.len, gname.data);
                } else {
                    const char* reg32 = x86_reg_name(target_reg, 4);
                    fprintf(out, "    mov %s, dword [%.*s]\n", reg32, (int)gname.len, gname.data);
                }
            } else {
                fprintf(out, "    mov %s, qword [%.*s]\n", target_reg, (int)gname.len, gname.data);
            }
            break;
        }

        case IR_OP_STR: {
            fprintf(out, "    lea %s, [LC_STR_%u]\n", target_reg, op->str_id);
            break;
        }

        case IR_OP_NONE:
        case IR_OP_BLOCK:
            break;
    }
}

static void emit_store_from_rax(FILE* out, const IRFunction* func, const IROperand* dst) {
    if (dst->kind == IR_OP_REG) {
        size_t size = dst->byte_size ? dst->byte_size : 8;

        if (size == 1) {
            const char* dst_r32 = reg_name((X86Reg)dst->reg, 4);
            const char* ext = dst->is_signed ? "movsx" : "movzx";
            fprintf(out, "    %s %s, al\n", ext, dst_r32);
        } else if (size == 2) {
            const char* dst_r32 = reg_name((X86Reg)dst->reg, 4);
            const char* ext = dst->is_signed ? "movsx" : "movzx";
            fprintf(out, "    %s %s, ax\n", ext, dst_r32);
        } else if (size == 4) {
            const char* dst_r32 = reg_name((X86Reg)dst->reg, 4);
            fprintf(out, "    mov %s, eax\n", dst_r32);
        } else {
            const char* dst_r64 = reg_name((X86Reg)dst->reg, 8);
            if (strcmp(dst_r64, "rax") != 0) {
                fprintf(out, "    mov %s, rax\n", dst_r64);
            }
        }
    } else if (dst->kind == IR_OP_VREG) {
        int32_t off = get_vreg_stack_offset(func, dst->vreg_id);
        fprintf(out, "    mov [rbp %d], rax\n", off);
    } else if (dst->kind == IR_OP_STACK) {
        int32_t off = get_effective_stack_offset(func, dst->stack_offset);
        char mem_op[64];
        format_stack_offset(mem_op, sizeof(mem_op), off);
        size_t size = dst->byte_size ? dst->byte_size : 8;
        const char* prefix = x86_size_prefix(size);
        const char* reg = x86_reg_name("rax", size);

        fprintf(out, "    mov %s %s, %s\n", prefix, mem_op, reg);
    } else if (dst->kind == IR_OP_GLOBAL) {
        size_t size = dst->byte_size ? dst->byte_size : 8;
        const char* prefix = x86_size_prefix(size);
        const char* reg = x86_reg_name("rax", size);

        fprintf(out, "    mov %s [%.*s], %s\n", prefix, (int)dst->global_name.len, dst->global_name.data, reg);
    }
}

static void emit_div_mod(FILE* out, const IRFunction* func, const IRInst* inst) {
    size_t size    = inst->dst.byte_size ? inst->dst.byte_size : 8;
    bool is_signed = inst->src1.is_signed;

    emit_load_operand(out, func, &inst->src2, "r10");
    emit_load_operand(out, func, &inst->src1, "rax");

    const char* divisor_reg = x86_reg_name("r10", size);

    if (size == 8) {
        if (is_signed) {
            fprintf(out, "    cqo\n");
            fprintf(out, "    idiv %s\n", divisor_reg);
        } else {
            fprintf(out, "    xor edx, edx\n");
            fprintf(out, "    div %s\n", divisor_reg);
        }
    } else if (size == 4) {
        if (is_signed) {
            fprintf(out, "    cdq\n");
            fprintf(out, "    idiv %s\n", divisor_reg);
        } else {
            fprintf(out, "    xor edx, edx\n");
            fprintf(out, "    div %s\n", divisor_reg);
        }
    } else if (size == 2) {
        if (is_signed) {
            fprintf(out, "    cwd\n");
            fprintf(out, "    idiv %s\n", divisor_reg);
        } else {
            fprintf(out, "    xor dx, dx\n");
            fprintf(out, "    div %s\n", divisor_reg);
        }
    } else {
        if (is_signed) {
            fprintf(out, "    cbw\n");
            fprintf(out, "    idiv %s\n", divisor_reg);
        } else {
            fprintf(out, "    movzx ax, al\n");
            fprintf(out, "    div %s\n", divisor_reg);
        }
    }

    if (inst->opcode == IR_MOD) {
        if (size == 1) {
            fprintf(out, "    mov al, ah\n");
        } else {
            fprintf(out, "    mov %s, %s\n", x86_reg_name("rax", size), x86_reg_name("rdx", size));
        }
    }

    emit_store_from_rax(out, func, &inst->dst);
}

static void emit_binary_op(FILE* out, const IRFunction* func, const IRInst* inst, const char* op_asm) {
    if (inst->dst.kind == IR_OP_REG) {
        size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
        const char* dst_r = reg_name((X86Reg)inst->dst.reg, size);

        if (inst->opcode == IR_ADD && size == 8 && inst->src1.kind == IR_OP_REG) {
            const char* s1_r = reg_name((X86Reg)inst->src1.reg, 8);

            if (inst->src2.kind == IR_OP_REG && inst->dst.reg != inst->src1.reg && inst->dst.reg != inst->src2.reg) {
                const char* s2_r = reg_name((X86Reg)inst->src2.reg, 8);
                fprintf(out, "    lea %s, [%s + %s]\n", dst_r, s1_r, s2_r);
                return;
            }

            if (inst->src2.kind == IR_OP_CONST && is_signed_imm32(inst->src2.int_val) && inst->dst.reg != inst->src1.reg) {
                fprintf(out, "    lea %s, [%s + %lld]\n", dst_r, s1_r, (long long)inst->src2.int_val);
                return;
            }
        }

        bool src2_is_direct_imm = (inst->src2.kind == IR_OP_CONST && is_signed_imm32(inst->src2.int_val));
        bool src2_in_temp = false;

        if (src2_is_direct_imm) {
        } else if (inst->src2.kind == IR_OP_REG && inst->src2.byte_size >= size) {
            if (inst->src2.reg == inst->dst.reg && (inst->src1.kind != IR_OP_REG || inst->src1.reg != inst->dst.reg)) {
                const char* src2_r = reg_name((X86Reg)inst->src2.reg, size);
                fprintf(out, "    mov %s, %s\n", x86_reg_name("r10", size), src2_r);
                src2_in_temp = true;
            }
        } else {
            emit_load_operand(out, func, &inst->src2, "r10");
            src2_in_temp = true;
        }

        if (inst->src1.kind == IR_OP_REG && inst->src1.byte_size >= size) {
            const char* src1_r = reg_name((X86Reg)inst->src1.reg, size);
            if (strcmp(dst_r, src1_r) != 0) {
                fprintf(out, "    mov %s, %s\n", dst_r, src1_r);
            }
        } else {
            emit_load_operand(out, func, &inst->src1, "rax");
            fprintf(out, "    mov %s, %s\n", dst_r, x86_reg_name("rax", size));
        }

        if (src2_is_direct_imm) {
            fprintf(out, "    %s %s, %lld\n", op_asm, dst_r, (long long)inst->src2.int_val);
        } else if (src2_in_temp) {
            fprintf(out, "    %s %s, %s\n", op_asm, dst_r, x86_reg_name("r10", size));
        } else {
            const char* src2_r = reg_name((X86Reg)inst->src2.reg, size);
            fprintf(out, "    %s %s, %s\n", op_asm, dst_r, src2_r);
        }
    } else {
        emit_load_operand(out, func, &inst->src2, "r10");
        emit_load_operand(out, func, &inst->src1, "rax");
        fprintf(out, "    %s rax, r10\n", op_asm);
        emit_store_from_rax(out, func, &inst->dst);
    }
}

static void emit_load_address(FILE* out, const IRFunction* func, const IROperand* op, const char* target_reg) {
    switch (op->kind) {
        case IR_OP_REG: {
            const char* src_r = reg_name((X86Reg)op->reg, 8);
            const char* dst_r = x86_reg_name(target_reg, 8);

            if (strcmp(src_r, dst_r) != 0) {
                fprintf(out, "    mov %s, %s\n", dst_r, src_r);
            }
            break;
        }

        case IR_OP_VREG: {
            int32_t off = get_vreg_stack_offset(func, op->vreg_id);
            fprintf(out, "    mov %s, [rbp %d]\n", target_reg, off);
            break;
        }

        case IR_OP_STACK: {
            int32_t off = get_effective_stack_offset(func, op->stack_offset);
            char mem_op[64];
            format_stack_offset(mem_op, sizeof(mem_op), off);
            fprintf(out, "    lea %s, %s\n", target_reg, mem_op);
            break;
        }

        case IR_OP_GLOBAL: {
            StrView gname = op->global_name;
            fprintf(out, "    lea %s, [%.*s]\n", target_reg, (int)gname.len, gname.data);
            break;
        }

        case IR_OP_CONST: {
            if (op->int_val == 0) {
                const char* r32 = x86_reg_name(target_reg, 4);
                fprintf(out, "    xor %s, %s\n", r32, r32);
            } else {
                fprintf(out, "    mov %s, %lld\n", target_reg, (long long)op->int_val);
            }
            break;
        }

        default:
            break;
    }
}

static void emit_parallel_register_moves(FILE* out, const IRFunction* func, ArgMove* moves, size_t count) {
    size_t pending = 0;

    for (size_t i = 0; i < count; ++i) {
        if (moves[i].src_op.kind == IR_OP_REG && (X86Reg)moves[i].src_op.reg == moves[i].dst_reg) {
            moves[i].done = true;
        } else {
            moves[i].done = false;
            pending++;
        }
    }

    while (pending > 0) {
        bool progress = false;

        for (size_t i = 0; i < count; ++i) {
            if (moves[i].done) {
                continue;
            }

            bool dst_used_as_src = false;

            for (size_t j = 0; j < count; ++j) {
                if (!moves[j].done && i != j &&
                    moves[j].src_op.kind == IR_OP_REG &&
                    (X86Reg)moves[j].src_op.reg == moves[i].dst_reg) {

                    dst_used_as_src = true;
                    break;
                }
            }

            if (!dst_used_as_src) {
                const char* target_r = reg_name(moves[i].dst_reg, 8);
                emit_load_operand(out, func, &moves[i].src_op, target_r);

                moves[i].done = true;
                pending--;
                progress = true;
                break;
            }
        }

        if (!progress && pending > 0) {
            size_t cycle_idx = (size_t)-1;

            for (size_t i = 0; i < count; ++i) {
                if (!moves[i].done) {
                    cycle_idx = i;
                    break;
                }
            }

            assert(cycle_idx != (size_t)-1);

            X86Reg clobbered_reg = moves[cycle_idx].dst_reg;
            const char* clobbered_r = reg_name(clobbered_reg, 8);

            fprintf(out, "    mov r10, %s\n", clobbered_r);

            for (size_t j = 0; j < count; ++j) {
                if (!moves[j].done &&
                    moves[j].src_op.kind == IR_OP_REG &&
                    (X86Reg)moves[j].src_op.reg == clobbered_reg) {

                    moves[j].src_op = ir_op_reg(REG_R10, moves[j].src_op.byte_size, moves[j].src_op.is_signed);
                }
            }

            const char* target_r = reg_name(moves[cycle_idx].dst_reg, 8);
            emit_load_operand(out, func, &moves[cycle_idx].src_op, target_r);

            moves[cycle_idx].done = true;
            pending--;
        }
    }
}

static void emit_call_arguments(
    FILE* out,
    const IRFunction* func,
    const IRInst* inst
) {
    size_t argc = inst->extra_arg_count;

    size_t reg_args = (argc < KLANG_ABI_GP_ARG_COUNT) ? argc : KLANG_ABI_GP_ARG_COUNT;
    size_t stack_args = argc - reg_args;

    bool needs_padding = (stack_args % 2) != 0;

    if (needs_padding) {
        fprintf(out, "    sub rsp, %d\n", KLANG_ABI_GP_SLOT_SIZE);
    }

    for (size_t i = argc; i > KLANG_ABI_GP_ARG_COUNT; --i) {
        emit_load_operand(
            out,
            func,
            &inst->extra_args[i - 1],
            "rax"
        );

        fprintf(
            out,
            "    push rax\n"
        );
    }

    ArgMove moves[KLANG_ABI_GP_ARG_COUNT];

    for (size_t i = 0; i < reg_args; ++i) {
        moves[i].dst_reg = abi_gp_arg_reg(i);
        moves[i].src_op  = inst->extra_args[i];
        moves[i].done    = false;
    }

    emit_parallel_register_moves(
        out,
        func,
        moves,
        reg_args
    );
}

static void get_condition_mnemonics(IROpcode op, bool is_signed, const char** out_setcc, const char** out_jcc) {
    switch (op) {
        case IR_CMP_EQ:
            if (out_setcc) *out_setcc = "sete";
            if (out_jcc)   *out_jcc   = "je";
            break;

        case IR_CMP_NE:
            if (out_setcc) *out_setcc = "setne";
            if (out_jcc)   *out_jcc   = "jne";
            break;

        case IR_CMP_LT:
            if (out_setcc) *out_setcc = is_signed ? "setl"  : "setb";
            if (out_jcc)   *out_jcc   = is_signed ? "jl"    : "jb";
            break;

        case IR_CMP_LE:
            if (out_setcc) *out_setcc = is_signed ? "setle" : "setbe";
            if (out_jcc)   *out_jcc   = is_signed ? "jle"   : "jbe";
            break;

        case IR_CMP_GT:
            if (out_setcc) *out_setcc = is_signed ? "setg"  : "seta";
            if (out_jcc)   *out_jcc   = is_signed ? "jg"    : "ja";
            break;

        case IR_CMP_GE:
            if (out_setcc) *out_setcc = is_signed ? "setge" : "setae";
            if (out_jcc)   *out_jcc   = is_signed ? "jge"   : "jae";
            break;

        default:
            if (out_setcc) *out_setcc = "sete";
            if (out_jcc)   *out_jcc   = "je";
            break;
    }
}

static void emit_cmp_operands(FILE* out, const IRFunction* func, const IRInst* inst) {
    size_t size = inst->src1.byte_size ? inst->src1.byte_size : 8;

    if (inst->src2.kind == IR_OP_CONST && inst->src2.int_val == 0 &&
        (inst->opcode == IR_CMP_EQ || inst->opcode == IR_CMP_NE)) {

        if (inst->src1.kind == IR_OP_REG) {
            const char* s1_r = reg_name((X86Reg)inst->src1.reg, size);
            fprintf(out, "    test %s, %s\n", s1_r, s1_r);
            return;
        }

        if (inst->src1.kind == IR_OP_STACK) {
            int32_t off = get_effective_stack_offset(func, inst->src1.stack_offset);
            char mem_op[64];
            format_stack_offset(mem_op, sizeof(mem_op), off);
            const char* prefix = x86_size_prefix(size);
            fprintf(out, "    cmp %s %s, 0\n", prefix, mem_op);
            return;
        }

        emit_load_operand(out, func, &inst->src1, "rax");
        const char* r = x86_reg_name("rax", size);
        fprintf(out, "    test %s, %s\n", r, r);
        return;
    }

    if (inst->src2.kind == IR_OP_CONST && is_signed_imm32(inst->src2.int_val)) {
        if (inst->src1.kind == IR_OP_REG) {
            const char* s1_r = reg_name((X86Reg)inst->src1.reg, size);
            fprintf(out, "    cmp %s, %lld\n", s1_r, (long long)inst->src2.int_val);
            return;
        }

        if (inst->src1.kind == IR_OP_STACK) {
            int32_t off = get_effective_stack_offset(func, inst->src1.stack_offset);
            char mem_op[64];
            format_stack_offset(mem_op, sizeof(mem_op), off);
            const char* prefix = x86_size_prefix(size);
            fprintf(out, "    cmp %s %s, %lld\n", prefix, mem_op, (long long)inst->src2.int_val);
            return;
        }

        emit_load_operand(out, func, &inst->src1, "rax");
        const char* r = x86_reg_name("rax", size);
        fprintf(out, "    cmp %s, %lld\n", r, (long long)inst->src2.int_val);
        return;
    }

    if (inst->src1.kind == IR_OP_REG && inst->src2.kind == IR_OP_REG) {
        const char* s1_r = reg_name((X86Reg)inst->src1.reg, size);
        const char* s2_r = reg_name((X86Reg)inst->src2.reg, size);
        fprintf(out, "    cmp %s, %s\n", s1_r, s2_r);
        return;
    }

    if (inst->src1.kind == IR_OP_REG && inst->src2.kind == IR_OP_STACK) {
        const char* s1_r = reg_name((X86Reg)inst->src1.reg, size);
        int32_t off = get_effective_stack_offset(func, inst->src2.stack_offset);
        char mem_op[64];
        format_stack_offset(mem_op, sizeof(mem_op), off);
        const char* prefix = x86_size_prefix(size);
        fprintf(out, "    cmp %s, %s %s\n", s1_r, prefix, mem_op);
        return;
    }

    emit_load_operand(out, func, &inst->src2, "r10");
    emit_load_operand(out, func, &inst->src1, "rax");

    const char* r1 = x86_reg_name("rax", size);
    const char* r2 = x86_reg_name("r10", size);

    fprintf(out, "    cmp %s, %s\n", r1, r2);
}

static inline bool operand_equals(IROperand a, IROperand b) {
    if (a.kind != b.kind) {
        return false;
    }

    switch (a.kind) {
        case IR_OP_NONE:
            return true;

        case IR_OP_CONST:
            return a.int_val == b.int_val && a.byte_size == b.byte_size;

        case IR_OP_VREG:
            return a.vreg_id == b.vreg_id && a.byte_size == b.byte_size;

        case IR_OP_REG:
            return a.reg == b.reg && a.byte_size == b.byte_size;

        case IR_OP_STACK:
            return a.stack_offset == b.stack_offset && a.byte_size == b.byte_size;

        case IR_OP_GLOBAL:
            return strview_equals(a.global_name, b.global_name);

        case IR_OP_STR:
            return a.str_id == b.str_id;

        case IR_OP_BLOCK:
            return a.block == b.block;
    }

    return false;
}

static bool is_same_memory_location(const IRInst* load_inst, const IRInst* store_inst) {
    if (!load_inst || !store_inst) {
        return false;
    }

    if (load_inst->dst.byte_size != store_inst->src1.byte_size) {
        return false;
    }

    if (!operand_equals(load_inst->src1, store_inst->dst)) {
        return false;
    }

    if (!operand_equals(load_inst->src2, store_inst->src2)) {
        return false;
    }

    if (load_inst->mem_index != store_inst->mem_index ||
        load_inst->mem_scale != store_inst->mem_scale) {
        return false;
    }

    return true;
}

static inline bool inst_dst_is_read(IROpcode op) {
    return op == IR_STORE || op == IR_MEMCPY || op == IR_BR || op == IR_RET;
}

static bool is_reg_read_later_in_block(const IRInst* from_inst, X86Reg reg) {
    if (reg == REG_NONE) {
        return false;
    }

    for (const IRInst* inst = from_inst; inst != NULL; inst = inst->next) {
        if (inst->opcode == IR_NOP) {
            continue;
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

        if (inst->mem_index == reg) {
            return true;
        }

        if (!inst_dst_is_read(inst->opcode) && inst->dst.kind == IR_OP_REG && (X86Reg)inst->dst.reg == reg) {
            return false;
        }
    }

    return false;
}

static bool try_emit_fused_mem_op(FILE* out, const IRFunction* func, const IRInst* inst, const IRInst** out_next) {
    if (!inst || inst->opcode != IR_LOAD || inst->dst.kind != IR_OP_REG) {
        return false;
    }

    const IRInst* inst2 = inst->next;

    while (inst2 && inst2->opcode == IR_NOP) {
        inst2 = inst2->next;
    }

    if (!inst2 || inst2->dst.kind != IR_OP_REG) {
        return false;
    }

    const IRInst* inst3 = inst2->next;

    while (inst3 && inst3->opcode == IR_NOP) {
        inst3 = inst3->next;
    }

    if (!inst3 || inst3->opcode != IR_STORE || inst3->src1.kind != IR_OP_REG) {
        return false;
    }

    if (!is_same_memory_location(inst, inst3)) {
        return false;
    }

    X86Reg load_reg  = (X86Reg)inst->dst.reg;
    X86Reg store_reg = (X86Reg)inst3->src1.reg;
    X86Reg op_dst    = (X86Reg)inst2->dst.reg;

    if (op_dst != store_reg) {
        return false;
    }

    if (inst->src1.kind == IR_OP_REG && (X86Reg)inst->src1.reg == op_dst) {
        return false;
    }

    if (inst->mem_index != REG_NONE && inst->mem_index == op_dst) {
        return false;
    }

    if (is_reg_read_later_in_block(inst3->next, load_reg)) {
        return false;
    }

    if (load_reg != op_dst && is_reg_read_later_in_block(inst3->next, op_dst)) {
        return false;
    }

    size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
    const char* prefix = x86_size_prefix(size);
    int64_t disp = (inst->src2.kind == IR_OP_CONST) ? inst->src2.int_val : 0;

    char mem_spec[128];
    format_memory_address(mem_spec, sizeof(mem_spec), func, &inst->src1, inst->mem_index, inst->mem_scale, disp);

    if (inst2->opcode == IR_NOT && inst2->src1.kind == IR_OP_REG && (X86Reg)inst2->src1.reg == load_reg) {
        fprintf(out, "    not %s %s\n", prefix, mem_spec);
        *out_next = inst3;
        return true;
    }

    if (inst2->opcode == IR_NEG && inst2->src1.kind == IR_OP_REG && (X86Reg)inst2->src1.reg == load_reg) {
        fprintf(out, "    neg %s %s\n", prefix, mem_spec);
        *out_next = inst3;
        return true;
    }

    IROperand other_op = ir_op_none();
    const char* op_asm = NULL;

    if (inst2->opcode >= IR_ADD && inst2->opcode <= IR_XOR) {
        bool is_s1 = (inst2->src1.kind == IR_OP_REG && (X86Reg)inst2->src1.reg == load_reg);
        bool is_s2 = (inst2->src2.kind == IR_OP_REG && (X86Reg)inst2->src2.reg == load_reg);

        if (is_s1) {
            other_op = inst2->src2;
        } else if (is_s2 && (inst2->opcode == IR_ADD || inst2->opcode == IR_AND ||
                             inst2->opcode == IR_OR  || inst2->opcode == IR_XOR)) {
            other_op = inst2->src1;
        } else {
            return false;
        }

        switch (inst2->opcode) {
            case IR_ADD: op_asm = "add"; break;
            case IR_SUB: op_asm = "sub"; break;
            case IR_AND: op_asm = "and"; break;
            case IR_OR:  op_asm = "or";  break;
            case IR_XOR: op_asm = "xor"; break;
            default: return false;
        }

        if (inst2->opcode == IR_ADD && other_op.kind == IR_OP_CONST && other_op.int_val == 1) {
            fprintf(out, "    inc %s %s\n", prefix, mem_spec);
            *out_next = inst3;
            return true;
        }

        if (inst2->opcode == IR_SUB && other_op.kind == IR_OP_CONST && other_op.int_val == 1) {
            fprintf(out, "    dec %s %s\n", prefix, mem_spec);
            *out_next = inst3;
            return true;
        }

        if (other_op.kind == IR_OP_CONST && is_signed_imm32(other_op.int_val)) {
            fprintf(out, "    %s %s %s, %lld\n", op_asm, prefix, mem_spec, (long long)other_op.int_val);
            *out_next = inst3;
            return true;
        }

        if (other_op.kind == IR_OP_REG) {
            const char* r_name = reg_name((X86Reg)other_op.reg, size);
            fprintf(out, "    %s %s %s, %s\n", op_asm, prefix, mem_spec, r_name);
            *out_next = inst3;
            return true;
        }

        emit_load_operand(out, func, &other_op, "r10");
        fprintf(out, "    %s %s %s, %s\n", op_asm, prefix, mem_spec, x86_reg_name("r10", size));
        *out_next = inst3;
        return true;
    }

    if (inst2->opcode == IR_SHL || inst2->opcode == IR_SHR) {
        if (inst2->src1.kind != IR_OP_REG || (X86Reg)inst2->src1.reg != load_reg) {
            return false;
        }

        op_asm   = (inst2->opcode == IR_SHL) ? "shl" : (inst2->src1.is_signed ? "sar" : "shr");
        other_op = inst2->src2;

        if (other_op.kind == IR_OP_CONST) {
            fprintf(out, "    %s %s %s, %lld\n", op_asm, prefix, mem_spec, (long long)other_op.int_val);
            *out_next = inst3;
            return true;
        }

        emit_load_operand(out, func, &other_op, "rcx");
        fprintf(out, "    %s %s %s, cl\n", op_asm, prefix, mem_spec);
        *out_next = inst3;
        return true;
    }

    return false;
}

static bool try_emit_fused_cmp_branch(FILE* out, const IRFunction* func, const IRBlock* b, const IRInst* inst) {
    if (inst->opcode < IR_CMP_EQ || inst->opcode > IR_CMP_GE) {
        return false;
    }

    const IRInst* next = inst->next;

    if (!next || next->opcode != IR_BR) {
        return false;
    }

    if (inst->dst.kind != IR_OP_REG || next->dst.kind != IR_OP_REG || inst->dst.reg != next->dst.reg) {
        return false;
    }

    emit_cmp_operands(out, func, inst);

    const char* jcc = "je";
    get_condition_mnemonics(inst->opcode, inst->src1.is_signed, NULL, &jcc);

    fprintf(out, "    %s .L_%.*s_%s\n", jcc, (int)func->name.len, func->name.data, next->src1.block->name);

    if (b->next_block != next->src2.block) {
        fprintf(out, "    jmp .L_%.*s_%s\n", (int)func->name.len, func->name.data, next->src2.block->name);
    }

    return true;
}

static void emit_instruction(FILE* out, const IRFunction* func, const IRBlock* b, const IRInst* inst) {
    switch (inst->opcode) {
        case IR_NOP:
            break;

        case IR_MOV: {
            if (inst->dst.kind == IR_OP_REG && inst->src1.kind == IR_OP_REG) {
                size_t dst_size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                size_t src_size = inst->src1.byte_size ? inst->src1.byte_size : 8;

                if (dst_size > src_size) {
                    const char* dst_r = reg_name((X86Reg)inst->dst.reg, (dst_size <= 4) ? 4 : 8);
                    const char* src_r = reg_name((X86Reg)inst->src1.reg, src_size);

                    if (inst->src1.is_signed) {
                        if (src_size == 4 && dst_size == 8) {
                            fprintf(out, "    movsxd %s, %s\n", dst_r, src_r);
                        } else {
                            fprintf(out, "    movsx %s, %s\n", dst_r, src_r);
                        }
                    } else {
                        fprintf(out, "    movzx %s, %s\n", dst_r, src_r);
                    }
                } else {
                    if (dst_size == 1) {
                        fprintf(out, "    movzx %s, %s\n", reg_name((X86Reg)inst->dst.reg, 4), reg_name((X86Reg)inst->src1.reg, 1));
                    } else if (dst_size == 2) {
                        fprintf(out, "    movzx %s, %s\n", reg_name((X86Reg)inst->dst.reg, 4), reg_name((X86Reg)inst->src1.reg, 2));
                    } else {
                        const char* dst_r = reg_name((X86Reg)inst->dst.reg, dst_size);
                        const char* src_r = reg_name((X86Reg)inst->src1.reg, dst_size);

                        if (strcmp(dst_r, src_r) != 0) {
                            fprintf(out, "    mov %s, %s\n", dst_r, src_r);
                        }
                    }
                }
            } else if (inst->dst.kind == IR_OP_REG && inst->src1.kind == IR_OP_CONST) {
                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                int64_t val = inst->src1.int_val;

                if (val == 0) {
                    const char* dst_r32 = reg_name((X86Reg)inst->dst.reg, 4);

                    fprintf(out, "    xor %s, %s\n", dst_r32, dst_r32);
                } else if (size == 8 && val > 0 && val <= 4294967295LL) {
                    const char* dst_r32 = reg_name((X86Reg)inst->dst.reg, 4);

                    fprintf(out, "    mov %s, %lld\n", dst_r32, (long long)val);
                } else {
                    const char* dst_r = reg_name((X86Reg)inst->dst.reg, size);

                    fprintf(out, "    mov %s, %lld\n", dst_r, (long long)val);
                }
            } else if (inst->dst.kind == IR_OP_REG && inst->src1.kind == IR_OP_STACK) {
                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                int32_t off = get_effective_stack_offset(func, inst->src1.stack_offset);
                char mem_op[64];
                format_stack_offset(mem_op, sizeof(mem_op), off);

                if (size == 1) {
                    const char* op = inst->src1.is_signed ? "movsx" : "movzx";
                    fprintf(out, "    %s %s, byte %s\n", op, reg_name((X86Reg)inst->dst.reg, 4), mem_op);
                } else if (size == 2) {
                    const char* op = inst->src1.is_signed ? "movsx" : "movzx";
                    fprintf(out, "    %s %s, word %s\n", op, reg_name((X86Reg)inst->dst.reg, 4), mem_op);
                } else if (size == 4) {
                    if (inst->src1.is_signed && inst->dst.byte_size == 8) {
                        fprintf(out, "    movsxd %s, dword %s\n", reg_name((X86Reg)inst->dst.reg, 8), mem_op);
                    } else {
                        fprintf(out, "    mov %s, dword %s\n", reg_name((X86Reg)inst->dst.reg, 4), mem_op);
                    }
                } else {
                    fprintf(out, "    mov %s, qword %s\n", reg_name((X86Reg)inst->dst.reg, 8), mem_op);
                }
            } else if (inst->dst.kind == IR_OP_STACK && inst->src1.kind == IR_OP_REG) {
                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                int32_t off = get_effective_stack_offset(func, inst->dst.stack_offset);
                char mem_op[64];
                format_stack_offset(mem_op, sizeof(mem_op), off);
                const char* prefix = x86_size_prefix(size);
                const char* src_r = reg_name((X86Reg)inst->src1.reg, size);

                fprintf(out, "    mov %s %s, %s\n", prefix, mem_op, src_r);
            } else if (inst->dst.kind == IR_OP_STACK && inst->src1.kind == IR_OP_CONST && is_signed_imm32(inst->src1.int_val)) {
                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                int32_t off = get_effective_stack_offset(func, inst->dst.stack_offset);
                char mem_op[64];
                format_stack_offset(mem_op, sizeof(mem_op), off);
                const char* prefix = x86_size_prefix(size);

                fprintf(out, "    mov %s %s, %lld\n", prefix, mem_op, (long long)inst->src1.int_val);
            } else if (inst->dst.kind == IR_OP_GLOBAL && inst->src1.kind == IR_OP_CONST && is_signed_imm32(inst->src1.int_val)) {
                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                const char* prefix = x86_size_prefix(size);

                fprintf(out, "    mov %s [%.*s], %lld\n", prefix, (int)inst->dst.global_name.len, inst->dst.global_name.data, (long long)inst->src1.int_val);
            } else {
                emit_load_operand(out, func, &inst->src1, "rax");
                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }

        case IR_LOAD: {
            size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
            int64_t disp = (inst->src2.kind == IR_OP_CONST) ? inst->src2.int_val : 0;
            const char* op = inst->dst.is_signed ? ((size == 4) ? "movsxd" : "movsx") : "movzx";

            char mem_spec[128];
            format_memory_address(mem_spec, sizeof(mem_spec), func, &inst->src1, inst->mem_index, inst->mem_scale, disp);

            if (inst->dst.kind == IR_OP_REG) {
                const char* dst_r32 = reg_name((X86Reg)inst->dst.reg, 4);
                const char* dst_r64 = reg_name((X86Reg)inst->dst.reg, 8);

                if (size == 1) {
                    fprintf(out, "    %s %s, byte %s\n", op, dst_r32, mem_spec);
                } else if (size == 2) {
                    fprintf(out, "    %s %s, word %s\n", op, dst_r32, mem_spec);
                } else if (size == 4) {
                    if (inst->dst.is_signed) {
                        fprintf(out, "    movsxd %s, dword %s\n", dst_r64, mem_spec);
                    } else {
                        fprintf(out, "    mov %s, dword %s\n", dst_r32, mem_spec);
                    }
                } else {
                    fprintf(out, "    mov %s, qword %s\n", dst_r64, mem_spec);
                }
            } else {
                emit_load_address(out, func, &inst->src1, "r11");

                char fallback_spec[64];
                if (disp == 0) {
                    snprintf(fallback_spec, sizeof(fallback_spec), "[r11]");
                } else if (disp > 0) {
                    snprintf(fallback_spec, sizeof(fallback_spec), "[r11 + %lld]", (long long)disp);
                } else {
                    snprintf(fallback_spec, sizeof(fallback_spec), "[r11 - %lld]", (long long)(-disp));
                }

                if (size == 1) {
                    fprintf(out, "    %s rax, byte %s\n", op, fallback_spec);
                } else if (size == 2) {
                    fprintf(out, "    %s rax, word %s\n", op, fallback_spec);
                } else if (size == 4) {
                    if (inst->dst.is_signed) {
                        fprintf(out, "    movsxd rax, dword %s\n", fallback_spec);
                    } else {
                        fprintf(out, "    mov eax, dword %s\n", fallback_spec);
                    }
                } else {
                    fprintf(out, "    mov rax, qword %s\n", fallback_spec);
                }

                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }

        case IR_STORE: {
            size_t size = inst->src1.byte_size ? inst->src1.byte_size : 8;
            int64_t disp = (inst->src2.kind == IR_OP_CONST) ? inst->src2.int_val : 0;
            const char* prefix = x86_size_prefix(size);

            char mem_spec[128];
            format_memory_address(mem_spec, sizeof(mem_spec), func, &inst->dst, inst->mem_index, inst->mem_scale, disp);

            if (inst->src1.kind == IR_OP_REG) {
                const char* val_r = reg_name((X86Reg)inst->src1.reg, size);
                fprintf(out, "    mov %s %s, %s\n", prefix, mem_spec, val_r);
            } else if (inst->src1.kind == IR_OP_CONST && is_signed_imm32(inst->src1.int_val)) {
                fprintf(out, "    mov %s %s, %lld\n", prefix, mem_spec, (long long)inst->src1.int_val);
            } else {
                emit_load_operand(out, func, &inst->src1, "rax");
                fprintf(out, "    mov %s %s, %s\n", prefix, mem_spec, x86_reg_name("rax", size));
            }
            break;
        }

        case IR_ADDR: {
            if (inst->src1.kind == IR_OP_STACK) {
                int32_t off = get_effective_stack_offset(func, inst->src1.stack_offset);
                char mem_op[64];
                format_stack_offset(mem_op, sizeof(mem_op), off);

                if (inst->dst.kind == IR_OP_REG) {
                    fprintf(out, "    lea %s, %s\n", reg_name((X86Reg)inst->dst.reg, 8), mem_op);
                } else {
                    fprintf(out, "    lea rax, %s\n", mem_op);
                    emit_store_from_rax(out, func, &inst->dst);
                }
            } else if (inst->src1.kind == IR_OP_GLOBAL) {
                if (inst->dst.kind == IR_OP_REG) {
                    fprintf(out, "    lea %s, [%.*s]\n", reg_name((X86Reg)inst->dst.reg, 8), (int)inst->src1.global_name.len, inst->src1.global_name.data);
                } else {
                    fprintf(out, "    lea rax, [%.*s]\n", (int)inst->src1.global_name.len, inst->src1.global_name.data);
                    emit_store_from_rax(out, func, &inst->dst);
                }
            }
            break;
        }

        case IR_GLOBAL_STR: {
            if (inst->dst.kind == IR_OP_REG) {
                fprintf(out, "    lea %s, [LC_STR_%u]\n", reg_name((X86Reg)inst->dst.reg, 8), inst->src1.str_id);
            } else {
                fprintf(out, "    lea rax, [LC_STR_%u]\n", inst->src1.str_id);
                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }

        case IR_MEMCPY: {
            emit_load_address(out, func, &inst->dst, "r10");
            emit_load_address(out, func, &inst->src1, "r11");

            if (inst->src2.kind == IR_OP_CONST) {
                int64_t bytes = inst->src2.int_val;
                int64_t offset = 0;

                while (bytes >= 8) {
                    fprintf(out, "    mov rax, [r11 + %lld]\n", (long long)offset);
                    fprintf(out, "    mov [r10 + %lld], rax\n", (long long)offset);
                    offset += 8;
                    bytes  -= 8;
                }

                if (bytes >= 4) {
                    fprintf(out, "    mov eax, dword [r11 + %lld]\n", (long long)offset);
                    fprintf(out, "    mov dword [r10 + %lld], eax\n", (long long)offset);
                    offset += 4;
                    bytes  -= 4;
                }

                if (bytes >= 2) {
                    fprintf(out, "    mov ax, word [r11 + %lld]\n", (long long)offset);
                    fprintf(out, "    mov word [r10 + %lld], ax\n", (long long)offset);
                    offset += 2;
                    bytes  -= 2;
                }

                if (bytes == 1) {
                    fprintf(out, "    mov al, byte [r11 + %lld]\n", (long long)offset);
                    fprintf(out, "    mov byte [r10 + %lld], al\n", (long long)offset);
                }
            } else {
                emit_load_operand(out, func, &inst->src2, "rdx");
                char loop_lbl[64], end_lbl[64];
                static uint32_t memcpy_id = 0;
                uint32_t cur_id = memcpy_id++;
                snprintf(loop_lbl, sizeof(loop_lbl), ".L_memcpy_loop_%u", cur_id);
                snprintf(end_lbl, sizeof(end_lbl), ".L_memcpy_end_%u", cur_id);

                fprintf(out, "    test rdx, rdx\n");
                fprintf(out, "    jz %s\n", end_lbl);
                fprintf(out, "%s:\n", loop_lbl);
                fprintf(out, "    mov al, byte [r11]\n");
                fprintf(out, "    mov byte [r10], al\n");
                fprintf(out, "    inc r11\n");
                fprintf(out, "    inc r10\n");
                fprintf(out, "    dec rdx\n");
                fprintf(out, "    jnz %s\n", loop_lbl);
                fprintf(out, "%s:\n", end_lbl);
            }
            break;
        }

        case IR_ADD: {
            emit_binary_op(out, func, inst, "add");
            break;
        }

        case IR_SUB: {
            emit_binary_op(out, func, inst, "sub");
            break;
        }

        case IR_MUL: {
            emit_binary_op(out, func, inst, "imul");
            break;
        }

        case IR_DIV:
        case IR_MOD: {
            emit_div_mod(out, func, inst);
            break;
        }

        case IR_NEG: {
            if (inst->dst.kind == IR_OP_REG && inst->src1.kind == IR_OP_REG) {
                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                const char* dst_r = reg_name((X86Reg)inst->dst.reg, size);
                const char* src_r = reg_name((X86Reg)inst->src1.reg, size);

                if (strcmp(dst_r, src_r) != 0) {
                    fprintf(out, "    mov %s, %s\n", dst_r, src_r);
                }

                fprintf(out, "    neg %s\n", dst_r);
            } else {
                emit_load_operand(out, func, &inst->src1, "rax");
                fprintf(out, "    neg rax\n");
                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }

        case IR_AND: {
            emit_binary_op(out, func, inst, "and");
            break;
        }

        case IR_OR: {
            emit_binary_op(out, func, inst, "or");
            break;
        }

        case IR_XOR: {
            emit_binary_op(out, func, inst, "xor");
            break;
        }

        case IR_NOT: {
            if (inst->dst.kind == IR_OP_REG && inst->src1.kind == IR_OP_REG) {
                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                const char* dst_r = reg_name((X86Reg)inst->dst.reg, size);
                const char* src_r = reg_name((X86Reg)inst->src1.reg, size);

                if (strcmp(dst_r, src_r) != 0) {
                    fprintf(out, "    mov %s, %s\n", dst_r, src_r);
                }

                fprintf(out, "    not %s\n", dst_r);
            } else {
                emit_load_operand(out, func, &inst->src1, "rax");
                fprintf(out, "    not rax\n");
                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }

        case IR_SHL:
        case IR_SHR: {
            const char* op_asm = (inst->opcode == IR_SHL) ? "shl" : (inst->src1.is_signed ? "sar" : "shr");

            if (inst->dst.kind == IR_OP_REG) {
                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                const char* dst_r = reg_name((X86Reg)inst->dst.reg, size);

                if (inst->src2.kind == IR_OP_CONST) {
                    if (inst->src1.kind == IR_OP_REG && inst->src1.byte_size >= size) {
                        const char* src1_r = reg_name((X86Reg)inst->src1.reg, size);

                        if (strcmp(dst_r, src1_r) != 0) {
                            fprintf(out, "    mov %s, %s\n", dst_r, src1_r);
                        }
                    } else {
                        emit_load_operand(out, func, &inst->src1, "rax");
                        fprintf(out, "    mov %s, %s\n", dst_r, x86_reg_name("rax", size));
                    }

                    fprintf(out, "    %s %s, %lld\n", op_asm, dst_r, (long long)inst->src2.int_val);
                } else {
                    emit_load_operand(out, func, &inst->src2, "rcx");

                    if (inst->src1.kind == IR_OP_REG && inst->src1.byte_size >= size) {
                        const char* src1_r = reg_name((X86Reg)inst->src1.reg, size);

                        if (strcmp(dst_r, src1_r) != 0) {
                            fprintf(out, "    mov %s, %s\n", dst_r, src1_r);
                        }
                    } else {
                        emit_load_operand(out, func, &inst->src1, "rax");
                        fprintf(out, "    mov %s, %s\n", dst_r, x86_reg_name("rax", size));
                    }

                    fprintf(out, "    %s %s, cl\n", op_asm, dst_r);
                }
            } else {
                emit_load_operand(out, func, &inst->src2, "rcx");
                emit_load_operand(out, func, &inst->src1, "rax");

                fprintf(out, "    %s rax, cl\n", op_asm);
                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }

        case IR_CMP_EQ:
        case IR_CMP_NE:
        case IR_CMP_LT:
        case IR_CMP_LE:
        case IR_CMP_GT:
        case IR_CMP_GE: {
            emit_cmp_operands(out, func, inst);

            const char* set_cc = "sete";
            get_condition_mnemonics(inst->opcode, inst->src1.is_signed, &set_cc, NULL);

            if (inst->dst.kind == IR_OP_REG) {
                size_t dst_size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                const char* dst_byte_r = reg_name((X86Reg)inst->dst.reg, 1);

                fprintf(out, "    %s %s\n", set_cc, dst_byte_r);

                if (dst_size > 1) {
                    const char* dst_wide_r = reg_name((X86Reg)inst->dst.reg, (dst_size <= 4) ? 4 : 8);
                    fprintf(out, "    movzx %s, %s\n", dst_wide_r, dst_byte_r);
                }
            } else {
                fprintf(out, "    %s al\n", set_cc);
                fprintf(out, "    movzx eax, al\n");
                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }

        case IR_JMP: {
            fprintf(out, "    jmp .L_%.*s_%s\n", (int)func->name.len, func->name.data, inst->dst.block->name);
            break;
        }

        case IR_BR: {
            emit_load_operand(out, func, &inst->dst, "rax");
            fprintf(out, "    test rax, rax\n");
            fprintf(out, "    jnz .L_%.*s_%s\n", (int)func->name.len, func->name.data, inst->src1.block->name);

            if (b->next_block != inst->src2.block) {
                fprintf(out, "    jmp .L_%.*s_%s\n", (int)func->name.len, func->name.data, inst->src2.block->name);
            }
            break;
        }

        case IR_RET: {
            if (inst->dst.kind != IR_OP_NONE) {
                emit_load_operand(out, func, &inst->dst, "rax");
            }

            if (func_needs_frame_pointer(func)) {
                fprintf(out, "    leave\n");
            }

            emit_callee_saved_pop(out, func);
            fprintf(out, "    ret\n");
            break;
        }

        case IR_TAIL_CALL:
        case IR_TAIL_CALL_PTR: {
            if (inst->opcode == IR_TAIL_CALL_PTR) {
                emit_load_operand(out, func, &inst->src1, "r11");
            }

            emit_call_arguments(out, func, inst);

            if (func_needs_frame_pointer(func)) {
                fprintf(out, "    leave\n");
            }

            emit_callee_saved_pop(out, func);

            if (inst->opcode == IR_TAIL_CALL_PTR) {
                fprintf(out, "    jmp r11\n");
            } else {
                fprintf(out, "    jmp %.*s\n", (int)inst->symbol_name.len, inst->symbol_name.data);
            }
            break;
        }

        case IR_CALL:
        case IR_CALL_PTR: {
            if (inst->opcode == IR_CALL_PTR) {
                emit_load_operand(out, func, &inst->src1, "r11");
            }

            emit_call_arguments(out, func, inst);

            if (inst->is_variadic) {
                fprintf(out, "    xor eax, eax\n");
            }

            if (inst->opcode == IR_CALL_PTR) {
                fprintf(out, "    call r11\n");
            } else {
                fprintf(out, "    call %.*s\n", (int)inst->symbol_name.len, inst->symbol_name.data);
            }

            size_t argc = inst->extra_arg_count;
            size_t stack_args =
                (argc > KLANG_ABI_GP_ARG_COUNT)
                    ? (argc - KLANG_ABI_GP_ARG_COUNT)
                    : 0;
            bool needs_padding = (stack_args % 2) != 0;
            size_t cleanup_bytes =
                (stack_args + (needs_padding ? 1 : 0)) * KLANG_ABI_GP_SLOT_SIZE;

            if (cleanup_bytes > 0) {
                fprintf(out, "    add rsp, %zu\n", cleanup_bytes);
            }

            if (inst->dst.kind != IR_OP_NONE) {
                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }

        case IR_PARAM: {
            size_t param_idx = (size_t)inst->src1.int_val;

            if (param_idx < KLANG_ABI_GP_ARG_COUNT) {
                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                const char* src_reg =
                    x86_reg_name(abi_gp_arg_reg_name(param_idx), size);

                if (inst->dst.kind == IR_OP_REG) {
                    if (size == 1) {
                        const char* dst_r32 = reg_name((X86Reg)inst->dst.reg, 4);
                        const char* ext = inst->dst.is_signed ? "movsx" : "movzx";
                        fprintf(out, "    %s %s, %s\n", ext, dst_r32, src_reg);
                    } else if (size == 2) {
                        const char* dst_r32 = reg_name((X86Reg)inst->dst.reg, 4);
                        const char* ext = inst->dst.is_signed ? "movsx" : "movzx";
                        fprintf(out, "    %s %s, %s\n", ext, dst_r32, src_reg);
                    } else if (size == 4) {
                        const char* dst_r32 = reg_name((X86Reg)inst->dst.reg, 4);
                        fprintf(out, "    mov %s, %s\n", dst_r32, src_reg);
                    } else {
                        const char* dst_r64 = reg_name((X86Reg)inst->dst.reg, 8);
                        if (strcmp(dst_r64, src_reg) != 0) {
                            fprintf(out, "    mov %s, %s\n", dst_r64, src_reg);
                        }
                    }
                } else if (inst->dst.kind == IR_OP_STACK) {
                    int32_t off = get_effective_stack_offset(func, inst->dst.stack_offset);
                    char mem_op[64];
                    format_stack_offset(mem_op, sizeof(mem_op), off);
                    const char* prefix = x86_size_prefix(size);
                    fprintf(out, "    mov %s %s, %s\n", prefix, mem_op, src_reg);
                } else if (inst->dst.kind == IR_OP_VREG) {
                    int32_t off = get_vreg_stack_offset(func, inst->dst.vreg_id);
                    fprintf(out, "    mov [rbp %d], %s\n", off, x86_reg_name(src_reg, 8));
                }
            } else {
                int32_t raw_stack_arg_off =
                    (int32_t)(
                        KLANG_ABI_FIRST_STACK_ARG_OFFSET +
                        (param_idx - KLANG_ABI_GP_ARG_COUNT) * KLANG_ABI_GP_SLOT_SIZE
                    );

                int32_t stack_arg_off = get_effective_stack_offset(func, raw_stack_arg_off);

                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;

                if (inst->dst.kind == IR_OP_REG) {
                    char mem_op[64];
                    snprintf(mem_op, sizeof(mem_op), "[rbp + %d]", stack_arg_off);
                    const char* dst_r = reg_name((X86Reg)inst->dst.reg, size);

                    if (size == 1) {
                        const char* ext = inst->dst.is_signed ? "movsx" : "movzx";
                        fprintf(out, "    %s %s, byte %s\n", ext, x86_reg_name(dst_r, 4), mem_op);
                    } else if (size == 2) {
                        const char* ext = inst->dst.is_signed ? "movsx" : "movzx";
                        fprintf(out, "    %s %s, word %s\n", ext, x86_reg_name(dst_r, 4), mem_op);
                    } else if (size == 4) {
                        if (inst->dst.is_signed) {
                            fprintf(out, "    movsxd %s, dword %s\n", dst_r, mem_op);
                        } else {
                            fprintf(out, "    mov %s, dword %s\n", dst_r, mem_op);
                        }
                    } else {
                        fprintf(out, "    mov %s, qword %s\n", dst_r, mem_op);
                    }
                } else if (inst->dst.kind == IR_OP_STACK) {
                    int32_t dst_off = get_effective_stack_offset(func, inst->dst.stack_offset);
                    char src_mem[64];
                    snprintf(src_mem, sizeof(src_mem), "[rbp + %d]", stack_arg_off);
                    char dst_mem[64];
                    format_stack_offset(dst_mem, sizeof(dst_mem), dst_off);

                    size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                    const char* prefix = x86_size_prefix(size);

                    fprintf(out, "    mov rax, %s %s\n", prefix, src_mem);
                    fprintf(out, "    mov %s rax, %s\n", prefix, dst_mem);
                } else if (inst->dst.kind == IR_OP_VREG) {
                    int32_t off = get_vreg_stack_offset(func, inst->dst.vreg_id);
                    char mem_op[64];
                    snprintf(mem_op, sizeof(mem_op), "[rbp + %d]", stack_arg_off);

                    size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                    const char* prefix = x86_size_prefix(size);

                    fprintf(out, "    mov rax, %s %s\n", prefix, mem_op);
                    fprintf(out, "    mov [rbp %d], rax\n", off);
                }
            }
            break;
        }

        case IR_ALLOCA: {
            if (inst->src1.kind == IR_OP_CONST) {
                fprintf(out, "    sub rsp, %lld\n", (long long)inst->src1.int_val);
            } else {
                emit_load_operand(out, func, &inst->src1, "rax");
                fprintf(out, "    sub rsp, rax\n");
            }

            fprintf(out, "    mov rax, rsp\n");
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_INLINE_ASM: {
            if (inst->asm_input_count > 0) {
                ArgMove moves[16];
                size_t move_count = (inst->asm_input_count <= 16) ? inst->asm_input_count : 16;

                for (size_t i = 0; i < move_count; ++i) {
                    moves[i].dst_reg = inst->asm_inputs[i].reg;
                    moves[i].src_op  = inst->asm_inputs[i].val;
                    moves[i].done    = false;
                }

                emit_parallel_register_moves(out, func, moves, move_count);
            }

            fprintf(out, "    %.*s\n", (int)inst->symbol_name.len, inst->symbol_name.data);

            if (inst->asm_output_count > 0 && inst->dst.kind != IR_OP_NONE) {
                X86Reg out_r = inst->asm_outputs[0].reg;
                size_t out_sz = inst->asm_outputs[0].byte_size;

                if (inst->dst.kind == IR_OP_REG && (X86Reg)inst->dst.reg == out_r) {
                } else if (out_r != REG_RAX) {
                    const char* src_r = reg_name(out_r, out_sz);
                    const char* dst_r = reg_name(REG_RAX, out_sz);

                    fprintf(out, "    mov %s, %s\n", dst_r, src_r);
                    emit_store_from_rax(out, func, &inst->dst);
                } else {
                    emit_store_from_rax(out, func, &inst->dst);
                }
            } else if (inst->dst.kind != IR_OP_NONE) {
                emit_store_from_rax(out, func, &inst->dst);
            }

            break;
        }

        case IR_VA_START: {
            emit_load_address(out, func, &inst->dst, "r11");

            size_t gp_offset = abi_va_gp_offset(func->abi_fixed_gp_arg_count);

            fprintf(out, "    mov dword [r11], %zu\n", gp_offset);
            fprintf(out, "    mov dword [r11 + 4], %d\n", KLANG_ABI_GP_REG_SAVE_SIZE);

            int32_t overflow_arg_off =
                get_effective_stack_offset(func, KLANG_ABI_FIRST_STACK_ARG_OFFSET);

            fprintf(out, "    lea rax, [rbp + %d]\n", overflow_arg_off);
            fprintf(out, "    mov qword [r11 + 8], rax\n");

            int32_t reg_save_off = get_effective_stack_offset(func, func->reg_save_slot);

            char reg_save_mem[64];
            format_stack_offset(reg_save_mem, sizeof(reg_save_mem), reg_save_off);

            fprintf(out, "    lea rax, %s\n", reg_save_mem);
            fprintf(out, "    mov qword [r11 + 16], rax\n");

            break;
        }

        case IR_VA_ARG: {
            emit_load_operand(out, func, &inst->src1, "rdi");

            static uint32_t va_arg_id = 0;
            uint32_t id = va_arg_id++;

            char reg_label[64];
            char done_label[64];

            snprintf(
                reg_label,
                sizeof(reg_label),
                ".L_va_arg_reg_%u",
                id
            );

            snprintf(
                done_label,
                sizeof(done_label),
                ".L_va_arg_done_%u",
                id
            );

            fprintf(
                out,
                "    mov eax, dword [rdi]\n"
            );

            fprintf(
                out,
                "    cmp eax, %d\n",
                KLANG_ABI_GP_REG_SAVE_SIZE
            );

            fprintf(
                out,
                "    jae .L_va_arg_stack_%u\n",
                id
            );

            fprintf(
                out,
                "    mov r11, qword [rdi + 16]\n"
            );

            fprintf(
                out,
                "    add r11, rax\n"
            );

            fprintf(
                out,
                "    add eax, %d\n",
                KLANG_ABI_GP_SLOT_SIZE
            );

            fprintf(
                out,
                "    mov dword [rdi], eax\n"
            );

            fprintf(
                out,
                "    jmp %s\n",
                done_label
            );

            fprintf(
                out,
                ".L_va_arg_stack_%u:\n",
                id
            );

            fprintf(
                out,
                "    mov r11, qword [rdi + 8]\n"
            );

            fprintf(
                out,
                "    add qword [rdi + 8], %d\n",
                KLANG_ABI_GP_SLOT_SIZE
            );

            fprintf(
                out,
                "%s:\n",
                done_label
            );

            fprintf(
                out,
                "    mov rax, qword [r11]\n"
            );

            emit_store_from_rax(out, func, &inst->dst);

            break;
        }

        case IR_VA_END: {
            break;
        }

        case IR_VA_COPY: {
            emit_load_operand(out, func, &inst->dst, "rdi");
            emit_load_operand(out, func, &inst->src1, "rsi");
            fprintf(out, "    mov rax, qword [rsi]\n");
            fprintf(out, "    mov qword [rdi], rax\n");
            fprintf(out, "    mov rax, qword [rsi + 8]\n");
            fprintf(out, "    mov qword [rdi + 8], rax\n");
            fprintf(out, "    mov rax, qword [rsi + 16]\n");
            fprintf(out, "    mov qword [rdi + 16], rax\n");
            break;
        }

        case IR_PHI:
            break;
    }
}

static void codegen_set_section(FILE* out, StrView custom_section, const char* default_section, StrView* active_section) {
    StrView target;

    if (custom_section.len > 0) {
        target = custom_section;
    } else {
        target = (StrView){ .data = default_section, .len = strlen(default_section) };
    }

    if (active_section->len == target.len && memcmp(active_section->data, target.data, target.len) == 0) {
        return;
    }

    if (target.len == 5 && memcmp(target.data, ".text", 5) == 0) {
        fprintf(out, "\nsection '.text' executable\n");
    } else if (target.len == 5 && memcmp(target.data, ".data", 5) == 0) {
        fprintf(out, "\nsection '.data' writeable\n");
    } else if (target.len == 4 && memcmp(target.data, ".bss", 4) == 0) {
        fprintf(out, "\nsection '.bss' writeable\n");
    } else if (target.len == 7 && memcmp(target.data, ".rodata", 7) == 0) {
        fprintf(out, "\nsection '.rodata'\n");
    } else {
        fprintf(out, "\nsection '%.*s'\n", (int)target.len, target.data);
    }

    *active_section = target;
}

static void mark_jump_targets(const IRFunction* func, bool* is_target, size_t max_id) {
    for (const IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (const IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_JMP && inst->dst.kind == IR_OP_BLOCK && inst->dst.block != NULL) {
                if (inst->dst.block->id < max_id) {
                    is_target[inst->dst.block->id] = true;
                }
            } else if (inst->opcode == IR_BR) {
                if (inst->src1.kind == IR_OP_BLOCK && inst->src1.block != NULL && inst->src1.block->id < max_id) {
                    is_target[inst->src1.block->id] = true;
                }
                if (inst->src2.kind == IR_OP_BLOCK && inst->src2.block != NULL && inst->src2.block->id < max_id) {
                    is_target[inst->src2.block->id] = true;
                }
            }
        }
    }
}

static void emit_function(FILE* out, const IRFunction* func) {
    if (func->attrs.custom_align > 0) {
        fprintf(out, "    align %zu\n", func->attrs.custom_align);
    }

    fprintf(out, "public %.*s\n", (int)func->name.len, func->name.data);
    fprintf(out, "%.*s:\n", (int)func->name.len, func->name.data);

    emit_callee_saved_push(out, func);

    bool has_frame = func_needs_frame_pointer(func) || func->is_variadic;

    if (has_frame) {
        fprintf(out, "    push rbp\n");
        fprintf(out, "    mov rbp, rsp\n");

        size_t total_stack = get_total_function_stack_size(func);

        if (total_stack > 0) {
            fprintf(out, "    sub rsp, %zu\n", total_stack);
        }

        if (func->is_variadic && func->reg_save_slot != 0) {
            int32_t slot_off = get_effective_stack_offset(func, func->reg_save_slot);

            for (size_t i = 0; i < KLANG_ABI_GP_ARG_COUNT; ++i) {
                char mem_buf[64];
                format_stack_offset(mem_buf, sizeof(mem_buf), slot_off + (int32_t)(i * KLANG_ABI_GP_SLOT_SIZE));
                fprintf(out, "    mov qword %s, %s\n", mem_buf, abi_gp_arg_reg_name(i));
            }
        }
    }

    size_t max_block_id = func->next_block_id + 1;

    ArenaTemp scratch = arena_scratch_get(NULL, 0);
    bool* is_jump_target = ARENA_NEW_ARRAY_ZERO(scratch.arena, bool, max_block_id);

    mark_jump_targets(func, is_jump_target, max_block_id);

    for (const IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        if (is_jump_target[b->id]) {
            fprintf(out, "\n.L_%.*s_%s:\n", (int)func->name.len, func->name.data, b->name);
        }

        for (const IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            const IRInst* next_fused = NULL;

            if (try_emit_fused_mem_op(out, func, inst, &next_fused)) {
                inst = next_fused;
                continue;
            }

            if (inst->opcode >= IR_CMP_EQ && inst->opcode <= IR_CMP_GE) {
                if (try_emit_fused_cmp_branch(out, func, b, inst)) {
                    inst = inst->next;
                    continue;
                }
            }

            emit_instruction(out, func, b, inst);
        }
    }

    fprintf(out, "\n");

    arena_scratch_release(scratch);
}

void codegen_emit_fasm(const IRModule* module, FILE* out) {
    if (!module || !out) {
        return;
    }

    fprintf(out, "format ELF64\n");

    StrView active_section = { .data = "", .len = 0 };

    if (module->str_count > 0) {
        codegen_set_section(out, (StrView){0}, ".rodata", &active_section);

        for (const IRStringConst* s = module->first_str; s != NULL; s = s->next) {
            fprintf(out, "    LC_STR_%u: db ", s->id);

            for (size_t i = 0; i < s->value.len; ++i) {
                fprintf(out, "0x%02X, ", (unsigned char)s->value.data[i]);
            }

            fprintf(out, "0x00\n");
        }
    }

    for (const IRGlobalVar* g = module->first_global; g != NULL; g = g->next) {
        if (g->attrs.is_extern) {
            continue;
        }

        size_t align = g->attrs.custom_align ? g->attrs.custom_align : (g->type && g->type->align ? g->type->align : 8);

        if (g->has_init && g->init_item_count > 0) {
            codegen_set_section(out, g->attrs.section_name, ".data", &active_section);

            if (align > 0) {
                fprintf(out, "    align %zu\n", align);
            }

            fprintf(out, "    public %.*s\n", (int)g->name.len, g->name.data);
            fprintf(out, "    %.*s:\n", (int)g->name.len, g->name.data);

            for (size_t i = 0; i < g->init_item_count; ++i) {
                IRDataItem* item = &g->init_items[i];

                switch (item->kind) {
                    case IR_DATA_INT: {
                        const char* dir = "dq";
                        if (item->size == 1)      dir = "db";
                        else if (item->size == 2) dir = "dw";
                        else if (item->size == 4) dir = "dd";

                        fprintf(out, "        %s %lld\n", dir, (long long)item->val);
                        break;
                    }

                    case IR_DATA_STR_REF: {
                        if (item->val == 0) {
                            fprintf(out, "        dq LC_STR_%u\n", item->str_id);
                        } else if (item->val > 0) {
                            fprintf(out, "        dq LC_STR_%u + %lld\n", item->str_id, (long long)item->val);
                        } else {
                            fprintf(out, "        dq LC_STR_%u - %lld\n", item->str_id, (long long)(-item->val));
                        }
                        break;
                    }

                    case IR_DATA_SYM_REF: {
                        if (item->val == 0) {
                            fprintf(out, "        dq %.*s\n", (int)item->sym_name.len, item->sym_name.data);
                        } else if (item->val > 0) {
                            fprintf(out, "        dq %.*s + %lld\n", (int)item->sym_name.len, item->sym_name.data, (long long)item->val);
                        } else {
                            fprintf(out, "        dq %.*s - %lld\n", (int)item->sym_name.len, item->sym_name.data, (long long)(-item->val));
                        }
                        break;
                    }

                    case IR_DATA_ZERO: {
                        if (item->size > 0) {
                            fprintf(out, "        db %zu dup (0)\n", item->size);
                        }
                        break;
                    }
                }
            }
        } else {
            codegen_set_section(out, g->attrs.section_name, ".bss", &active_section);

            if (align > 0) {
                fprintf(out, "    align %zu\n", align);
            }

            size_t size = (g->type && g->type->size) ? g->type->size : 8;

            fprintf(out, "    public %.*s\n", (int)g->name.len, g->name.data);
            fprintf(out, "    %.*s: rb %zu\n", (int)g->name.len, g->name.data, size);
        }
    }

    bool has_externs = false;

    for (const IRGlobalVar* g = module->first_global; g != NULL; g = g->next) {
        if (g->attrs.is_extern) {
            fprintf(out, "extrn %.*s\n", (int)g->name.len, g->name.data);
            has_externs = true;
        }
    }

    for (const IRFunction* f = module->first_func; f != NULL; f = f->next) {
        if (f->attrs.is_extern) {
            fprintf(out, "extrn %.*s\n", (int)f->name.len, f->name.data);
            has_externs = true;
        }
    }

    if (has_externs) {
        fprintf(out, "\n");
    }

    for (const IRFunction* f = module->first_func; f != NULL; f = f->next) {
        if (!f->attrs.is_extern) {
            if (f->attrs.is_inlined && !f->attrs.is_exported) {
                continue;
            }

            codegen_set_section(out, f->attrs.section_name, ".text", &active_section);
            emit_function(out, f);
        }
    }
}

bool codegen_generate_file(const IRModule* module, const char* output_path) {
    FILE* out = fopen(output_path, "w");

    if (!out) {
        fprintf(stderr, "klang: error: could not open output file '%s' for writing\n", output_path);
        return false;
    }

    codegen_emit_fasm(module, out);

    fclose(out);

    return true;
}