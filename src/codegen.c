#include "codegen.h"
#include "regalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static const char* ABI_REG_64[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };

static const X86Reg CALLEE_SAVED_REGS[] = {
    REG_RBX,
    REG_R12,
    REG_R13,
    REG_R14,
    REG_R15
};

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

static void emit_load_operand(FILE* out, const IRFunction* func, const IROperand* op, const char* target_reg) {
    switch (op->kind) {
        case IR_OP_CONST: {
            fprintf(out, "    mov %s, %lld\n", target_reg, (long long)op->int_val);
            break;
        }

        case IR_OP_REG: {
            size_t size = op->byte_size ? op->byte_size : 8;
            const char* src_r = reg_name((X86Reg)op->reg, size);
            const char* dst_r = x86_reg_name(target_reg, size);

            if (strcmp(src_r, dst_r) != 0) {
                if (size == 1) {
                    const char* inst = op->is_signed ? "movsx" : "movzx";
                    fprintf(out, "    %s %s, %s\n", inst, target_reg, src_r);
                } else if (size == 2) {
                    const char* inst = op->is_signed ? "movsx" : "movzx";
                    fprintf(out, "    %s %s, %s\n", inst, target_reg, src_r);
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
            const char* sign = (off >= 0) ? "+ " : "";
            size_t size = op->byte_size ? op->byte_size : 8;

            if (size == 1) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, byte [rbp %s%d]\n", inst, target_reg, sign, off);
            } else if (size == 2) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, word [rbp %s%d]\n", inst, target_reg, sign, off);
            } else if (size == 4) {
                if (op->is_signed) {
                    fprintf(out, "    movsxd %s, dword [rbp %s%d]\n", target_reg, sign, off);
                } else {
                    const char* reg32 = x86_reg_name(target_reg, 4);
                    fprintf(out, "    mov %s, dword [rbp %s%d]\n", reg32, sign, off);
                }
            } else {
                fprintf(out, "    mov %s, qword [rbp %s%d]\n", target_reg, sign, off);
            }
            break;
        }

        case IR_OP_GLOBAL: {
            StrView gname = op->global_name;
            size_t size = op->byte_size ? op->byte_size : 8;

            if (size == 1) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, byte [rel %.*s]\n", inst, target_reg, (int)gname.len, gname.data);
            } else if (size == 2) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, word [rel %.*s]\n", inst, target_reg, (int)gname.len, gname.data);
            } else if (size == 4) {
                if (op->is_signed) {
                    fprintf(out, "    movsxd %s, dword [rel %.*s]\n", target_reg, (int)gname.len, gname.data);
                } else {
                    const char* reg32 = x86_reg_name(target_reg, 4);
                    fprintf(out, "    mov %s, dword [rel %.*s]\n", reg32, (int)gname.len, gname.data);
                }
            } else {
                fprintf(out, "    mov %s, qword [rel %.*s]\n", target_reg, (int)gname.len, gname.data);
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
        const char* dst_r = reg_name((X86Reg)dst->reg, size);
        const char* src_r = x86_reg_name("rax", size);

        if (strcmp(dst_r, src_r) != 0) {
            fprintf(out, "    mov %s, %s\n", dst_r, src_r);
        }
    } else if (dst->kind == IR_OP_VREG) {
        int32_t off = get_vreg_stack_offset(func, dst->vreg_id);
        fprintf(out, "    mov [rbp %d], rax\n", off);
    } else if (dst->kind == IR_OP_STACK) {
        int32_t off = get_effective_stack_offset(func, dst->stack_offset);
        const char* sign = (off >= 0) ? "+ " : "";
        size_t size = dst->byte_size ? dst->byte_size : 8;
        const char* prefix = x86_size_prefix(size);
        const char* reg = x86_reg_name("rax", size);

        fprintf(out, "    mov %s [rbp %s%d], %s\n", prefix, sign, off, reg);
    } else if (dst->kind == IR_OP_GLOBAL) {
        size_t size = dst->byte_size ? dst->byte_size : 8;
        const char* prefix = x86_size_prefix(size);
        const char* reg = x86_reg_name("rax", size);

        fprintf(out, "    mov %s [rel %.*s], %s\n", prefix, (int)dst->global_name.len, dst->global_name.data, reg);
    }
}

static inline bool is_signed_imm32(int64_t val) {
    return val >= -2147483648LL && val <= 2147483647LL;
}

static void emit_binary_op(FILE* out, const IRFunction* func, const IRInst* inst, const char* op_asm) {
    if (inst->dst.kind == IR_OP_REG) {
        size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
        const char* dst_r = reg_name((X86Reg)inst->dst.reg, size);

        bool src2_is_direct_imm = (inst->src2.kind == IR_OP_CONST && is_signed_imm32(inst->src2.int_val));

        if (src2_is_direct_imm) {
        } else if (inst->src2.kind == IR_OP_REG && inst->src2.byte_size >= size) {
        } else {
            emit_load_operand(out, func, &inst->src2, "r10");
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
        } else if (inst->src2.kind == IR_OP_REG && inst->src2.byte_size >= size) {
            const char* src2_r = reg_name((X86Reg)inst->src2.reg, size);
            fprintf(out, "    %s %s, %s\n", op_asm, dst_r, src2_r);
        } else {
            fprintf(out, "    %s %s, %s\n", op_asm, dst_r, x86_reg_name("r10", size));
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
            const char* sign = (off >= 0) ? "+ " : "";
            fprintf(out, "    mov %s, qword [rbp %s%d]\n", target_reg, sign, off);
            break;
        }

        case IR_OP_GLOBAL: {
            StrView gname = op->global_name;
            fprintf(out, "    mov %s, qword [rel %.*s]\n", target_reg, (int)gname.len, gname.data);
            break;
        }

        case IR_OP_CONST: {
            fprintf(out, "    mov %s, %lld\n", target_reg, (long long)op->int_val);
            break;
        }

        default:
            break;
    }
}

static void emit_instruction(FILE* out, const IRFunction* func, const IRInst* inst) {
    switch (inst->opcode) {
        case IR_NOP:
            break;

        case IR_MOV: {
            if (inst->dst.kind == IR_OP_REG && inst->src1.kind == IR_OP_REG) {
                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                const char* dst_r = reg_name((X86Reg)inst->dst.reg, size);
                const char* src_r = reg_name((X86Reg)inst->src1.reg, size);

                if (strcmp(dst_r, src_r) != 0) {
                    fprintf(out, "    mov %s, %s\n", dst_r, src_r);
                }
            } else {
                emit_load_operand(out, func, &inst->src1, "rax");
                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }

        case IR_LOAD: {
            emit_load_address(out, func, &inst->src1, "rcx");

            size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;

            if (size == 1) {
                const char* op = inst->dst.is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s rax, byte [rcx]\n", op);
            } else if (size == 2) {
                const char* op = inst->dst.is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s rax, word [rcx]\n", op);
            } else if (size == 4) {
                if (inst->dst.is_signed) {
                    fprintf(out, "    movsxd rax, dword [rcx]\n");
                } else {
                    fprintf(out, "    mov eax, dword [rcx]\n");
                }
            } else {
                fprintf(out, "    mov rax, qword [rcx]\n");
            }

            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_STORE: {
            emit_load_address(out, func, &inst->dst, "rcx");
            emit_load_operand(out, func, &inst->src1, "rax");

            size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
            const char* prefix = x86_size_prefix(size);
            const char* reg = x86_reg_name("rax", size);

            fprintf(out, "    mov %s [rcx], %s\n", prefix, reg);
            break;
        }

        case IR_ADDR: {
            if (inst->src1.kind == IR_OP_STACK) {
                int32_t off = get_effective_stack_offset(func, inst->src1.stack_offset);
                const char* sign = (off >= 0) ? "+ " : "";
                fprintf(out, "    lea rax, [rbp %s%d]\n", sign, off);
            } else if (inst->src1.kind == IR_OP_GLOBAL) {
                fprintf(out, "    lea rax, [rel %.*s]\n", (int)inst->src1.global_name.len, inst->src1.global_name.data);
            }
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_GLOBAL_STR: {
            fprintf(out, "    lea rax, [LC_STR_%u]\n", inst->src1.str_id);
            emit_store_from_rax(out, func, &inst->dst);
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
            emit_load_operand(out, func, &inst->src2, "r10");
            emit_load_operand(out, func, &inst->src1, "rax");

            if (inst->src1.is_signed) {
                fprintf(out, "    cqo\n");
                fprintf(out, "    idiv r10\n");
            } else {
                fprintf(out, "    xor edx, edx\n");
                fprintf(out, "    div r10\n");
            }

            if (inst->opcode == IR_MOD) {
                fprintf(out, "    mov rax, rdx\n");
            }

            emit_store_from_rax(out, func, &inst->dst);
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

                if (inst->src1.kind == IR_OP_REG) {
                    const char* src1_r = reg_name((X86Reg)inst->src1.reg, size);

                    if (strcmp(dst_r, src1_r) != 0) {
                        fprintf(out, "    mov %s, %s\n", dst_r, src1_r);
                    }
                } else {
                    emit_load_operand(out, func, &inst->src1, "rax");
                    fprintf(out, "    mov %s, %s\n", dst_r, x86_reg_name("rax", size));
                }

                if (inst->src2.kind == IR_OP_CONST) {
                    fprintf(out, "    %s %s, %lld\n", op_asm, dst_r, (long long)inst->src2.int_val);
                } else {
                    emit_load_operand(out, func, &inst->src2, "rcx");
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
            emit_load_operand(out, func, &inst->src2, "r10");
            emit_load_operand(out, func, &inst->src1, "rax");
            fprintf(out, "    cmp rax, r10\n");

            const char* set_cc = "sete";
            bool is_signed = inst->src1.is_signed;

            if (inst->opcode == IR_CMP_NE) set_cc = "setne";
            if (inst->opcode == IR_CMP_LT) set_cc = is_signed ? "setl" : "setb";
            if (inst->opcode == IR_CMP_LE) set_cc = is_signed ? "setle" : "setbe";
            if (inst->opcode == IR_CMP_GT) set_cc = is_signed ? "setg" : "seta";
            if (inst->opcode == IR_CMP_GE) set_cc = is_signed ? "setge" : "setae";

            fprintf(out, "    %s al\n", set_cc);
            fprintf(out, "    movzx rax, al\n");
            emit_store_from_rax(out, func, &inst->dst);
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
            fprintf(out, "    jmp .L_%.*s_%s\n", (int)func->name.len, func->name.data, inst->src2.block->name);
            break;
        }

        case IR_RET: {
            if (inst->dst.kind != IR_OP_NONE) {
                emit_load_operand(out, func, &inst->dst, "rax");
            }

            fprintf(out, "    leave\n");
            emit_callee_saved_pop(out, func);
            fprintf(out, "    ret\n");
            break;
        }

        case IR_CALL: {
            size_t argc = inst->extra_arg_count;
            size_t stack_args = (argc > 6) ? (argc - 6) : 0;
            size_t reg_args   = (argc > 6) ? 6 : argc;

            bool needs_padding = (stack_args % 2) != 0;

            if (needs_padding) {
                fprintf(out, "    sub rsp, 8\n");
            }

            for (size_t i = argc; i > 6; --i) {
                emit_load_operand(out, func, &inst->extra_args[i - 1], "rax");
                fprintf(out, "    push rax\n");
            }

            for (size_t i = 0; i < reg_args; ++i) {
                emit_load_operand(out, func, &inst->extra_args[i], "rax");
                fprintf(out, "    push rax\n");
            }

            for (size_t i = reg_args; i > 0; --i) {
                fprintf(out, "    pop %s\n", ABI_REG_64[i - 1]);
            }

            fprintf(out, "    call %.*s\n", (int)inst->symbol_name.len, inst->symbol_name.data);

            size_t cleanup_bytes = (stack_args + (needs_padding ? 1 : 0)) * 8;

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

            if (param_idx < 6) {
                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                const char* prefix = x86_size_prefix(size);
                const char* reg = x86_reg_name(ABI_REG_64[param_idx], size);
                const char* sign = (inst->dst.stack_offset >= 0) ? "+ " : "";

                fprintf(out, "    mov %s [rbp %s%d], %s\n", prefix, sign, inst->dst.stack_offset, reg);
            }
            break;
        }

        case IR_INLINE_ASM: {
            fprintf(out, "    %.*s\n", (int)inst->symbol_name.len, inst->symbol_name.data);

            if (inst->dst.kind != IR_OP_NONE) {
                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }
    }
}

static void emit_function(FILE* out, const IRFunction* func) {
    fprintf(out, "global %.*s\n", (int)func->name.len, func->name.data);
    fprintf(out, "%.*s:\n", (int)func->name.len, func->name.data);

    emit_callee_saved_push(out, func);

    fprintf(out, "    push rbp\n");
    fprintf(out, "    mov rbp, rsp\n");

    size_t total_stack = get_total_function_stack_size(func);

    if (total_stack > 0) {
        fprintf(out, "    sub rsp, %zu\n", total_stack);
    }

    fprintf(out, "\n");

    for (const IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        fprintf(out, ".L_%.*s_%s:\n", (int)func->name.len, func->name.data, b->name);

        for (const IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            emit_instruction(out, func, inst);
        }

        fprintf(out, "\n");
    }
}

void codegen_emit_nasm(const IRModule* module, FILE* out) {
    if (!module || !out) {
        return;
    }

    fprintf(out, "default rel\n\n");

    if (module->str_count > 0) {
        fprintf(out, "section .rodata\n");

        for (const IRStringConst* s = module->first_str; s != NULL; s = s->next) {
            fprintf(out, "    LC_STR_%u: db ", s->id);

            for (size_t i = 0; i < s->value.len; ++i) {
                fprintf(out, "0x%02X, ", (unsigned char)s->value.data[i]);
            }

            fprintf(out, "0x00\n");
        }

        fprintf(out, "\n");
    }

    bool has_data = false;

    for (const IRGlobalVar* g = module->first_global; g != NULL; g = g->next) {
        if (g->has_init) {
            if (!has_data) {
                fprintf(out, "section .data\n");
                has_data = true;
            }

            size_t size = (g->type && g->type->size) ? g->type->size : 8;
            const char* dir = "dq";

            if (size == 1)      dir = "db";
            else if (size == 2) dir = "dw";
            else if (size == 4) dir = "dd";

            fprintf(out, "    global %.*s\n", (int)g->name.len, g->name.data);
            fprintf(out, "    %.*s: %s %lld\n", (int)g->name.len, g->name.data, dir, (long long)g->init_val);
        }
    }

    if (has_data) {
        fprintf(out, "\n");
    }

    bool has_bss = false;

    for (const IRGlobalVar* g = module->first_global; g != NULL; g = g->next) {
        if (!g->has_init) {
            if (!has_bss) {
                fprintf(out, "section .bss\n");
                has_bss = true;
            }

            size_t size = (g->type && g->type->size) ? g->type->size : 8;
            fprintf(out, "    global %.*s\n", (int)g->name.len, g->name.data);
            fprintf(out, "    %.*s: resb %zu\n", (int)g->name.len, g->name.data, size);
        }
    }

    if (has_bss) {
        fprintf(out, "\n");
    }

    fprintf(out, "section .text\n\n");

    for (const IRFunction* f = module->first_func; f != NULL; f = f->next) {
        emit_function(out, f);
    }
}

bool codegen_generate_file(const IRModule* module, const char* output_path) {
    FILE* out = fopen(output_path, "w");

    if (!out) {
        fprintf(stderr, "klang: error: could not open output file '%s' for writing\n", output_path);
        return false;
    }

    codegen_emit_nasm(module, out);

    fclose(out);

    return true;
}