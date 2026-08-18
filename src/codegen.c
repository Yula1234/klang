#include "codegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static const char* ABI_REG_64[] = { "rdi", "rsi", "rdx", "rcx", "r8",  "r9"  };
static const char* ABI_REG_32[] = { "edi", "esi", "edx", "ecx", "r8d", "r9d" };
static const char* ABI_REG_16[] = { "di",  "si",  "dx",  "cx",  "r8w", "r9w" };
static const char* ABI_REG_8[]  = { "dil", "sil", "dl",  "cl",  "r8b", "r9b" };

static const char* reg64_to_32(const char* reg64) {
    if (strcmp(reg64, "rax") == 0) return "eax";
    if (strcmp(reg64, "rcx") == 0) return "ecx";
    if (strcmp(reg64, "rdx") == 0) return "edx";
    if (strcmp(reg64, "rbx") == 0) return "ebx";
    if (strcmp(reg64, "rsi") == 0) return "esi";
    if (strcmp(reg64, "rdi") == 0) return "edi";
    if (strcmp(reg64, "r8")  == 0) return "r8d";
    if (strcmp(reg64, "r9")  == 0) return "r9d";
    return reg64;
}

static inline int32_t get_vreg_stack_offset(const IRFunction* func, uint32_t vreg_id) {
    size_t base_offset = func->stack_frame_size;

    return -(int32_t)(base_offset + (vreg_id + 1) * 8);
}

static inline size_t get_total_function_stack_size(const IRFunction* func) {
    size_t raw_stack = func->stack_frame_size + (func->next_vreg_id * 8);

    return (raw_stack + 15) & ~15;
}

static void emit_load_operand(FILE* out, const IRFunction* func, const IROperand* op, const char* target_reg) {
    switch (op->kind) {
        case IR_OP_CONST: {
            fprintf(out, "    mov %s, %lld\n", target_reg, (long long)op->int_val);
            break;
        }

        case IR_OP_VREG: {
            int32_t off = get_vreg_stack_offset(func, op->vreg_id);
            fprintf(out, "    mov %s, [rbp %d]\n", target_reg, off);
            break;
        }

        case IR_OP_STACK: {
            if (op->byte_size == 1) {
                fprintf(out, "    movzx %s, byte [rbp %d]\n", target_reg, op->stack_offset);
            } else if (op->byte_size == 2) {
                fprintf(out, "    movzx %s, word [rbp %d]\n", target_reg, op->stack_offset);
            } else if (op->byte_size == 4) {
                fprintf(out, "    mov %s, dword [rbp %d]\n", reg64_to_32(target_reg), op->stack_offset);
            } else {
                fprintf(out, "    mov %s, qword [rbp %d]\n", target_reg, op->stack_offset);
            }
            break;
        }
        
        case IR_OP_STR: {
            fprintf(out, "    lea %s, [.str_%u]\n", target_reg, op->str_id);
            break;
        }

        case IR_OP_NONE:
        case IR_OP_BLOCK:
            break;
    }
}

static void emit_store_from_rax(FILE* out, const IRFunction* func, const IROperand* dst) {
    if (dst->kind == IR_OP_VREG) {
        int32_t off = get_vreg_stack_offset(func, dst->vreg_id);
        fprintf(out, "    mov [rbp %d], rax\n", off);
    } else if (dst->kind == IR_OP_STACK) {
        if (dst->byte_size == 1) {
            fprintf(out, "    mov byte [rbp %d], al\n", dst->stack_offset);
        } else if (dst->byte_size == 2) {
            fprintf(out, "    mov word [rbp %d], ax\n", dst->stack_offset);
        } else if (dst->byte_size == 4) {
            fprintf(out, "    mov dword [rbp %d], eax\n", dst->stack_offset);
        } else {
            fprintf(out, "    mov qword [rbp %d], rax\n", dst->stack_offset);
        }
    }
}

static void emit_instruction(FILE* out, const IRFunction* func, const IRInst* inst) {
    switch (inst->opcode) {
        case IR_LOAD: {
            emit_load_operand(out, func, &inst->src1, "rcx");

            if (inst->dst.byte_size == 1) {
                fprintf(out, "    movzx rax, byte [rcx]\n");
            } else if (inst->dst.byte_size == 2) {
                fprintf(out, "    movzx rax, word [rcx]\n");
            } else if (inst->dst.byte_size == 4) {
                fprintf(out, "    mov eax, dword [rcx]\n");
            } else {
                fprintf(out, "    mov rax, qword [rcx]\n");
            }

            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_STORE: {
            emit_load_operand(out, func, &inst->dst, "rcx");
            emit_load_operand(out, func, &inst->src1, "rax");

            if (inst->dst.byte_size == 1) {
                fprintf(out, "    mov byte [rcx], al\n");
            } else if (inst->dst.byte_size == 2) {
                fprintf(out, "    mov word [rcx], ax\n");
            } else if (inst->dst.byte_size == 4) {
                fprintf(out, "    mov dword [rcx], eax\n");
            } else {
                fprintf(out, "    mov qword [rcx], rax\n");
            }
            break;
        }

        case IR_LOAD_STACK: {
            emit_load_operand(out, func, &inst->src1, "rax");
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_STORE_STACK: {
            emit_load_operand(out, func, &inst->src1, "rax");
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_GLOBAL_STR: {
            fprintf(out, "    lea rax, [str_%u]\n", inst->src1.str_id);
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_ADD: {
            emit_load_operand(out, func, &inst->src1, "rax");
            emit_load_operand(out, func, &inst->src2, "rcx");
            fprintf(out, "    add rax, rcx\n");
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_SUB: {
            emit_load_operand(out, func, &inst->src1, "rax");
            emit_load_operand(out, func, &inst->src2, "rcx");
            fprintf(out, "    sub rax, rcx\n");
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_MUL: {
            emit_load_operand(out, func, &inst->src1, "rax");
            emit_load_operand(out, func, &inst->src2, "rcx");
            fprintf(out, "    imul rax, rcx\n");
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_DIV: {
            emit_load_operand(out, func, &inst->src1, "rax");
            emit_load_operand(out, func, &inst->src2, "rcx");
            fprintf(out, "    cqo\n");
            fprintf(out, "    idiv rcx\n");
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_NEG: {
            emit_load_operand(out, func, &inst->src1, "rax");
            fprintf(out, "    neg rax\n");
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_AND:
        case IR_OR:
        case IR_XOR: {
            emit_load_operand(out, func, &inst->src1, "rax");
            emit_load_operand(out, func, &inst->src2, "rcx");

            const char* op_asm = "and";
            if (inst->opcode == IR_OR)  op_asm = "or";
            if (inst->opcode == IR_XOR) op_asm = "xor";

            fprintf(out, "    %s rax, rcx\n", op_asm);
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_NOT: {
            emit_load_operand(out, func, &inst->src1, "rax");
            fprintf(out, "    not rax\n");
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_SHL:
        case IR_SHR: {
            emit_load_operand(out, func, &inst->src1, "rax");
            emit_load_operand(out, func, &inst->src2, "rcx");

            const char* op_asm = (inst->opcode == IR_SHL) ? "shl" : "shr";

            fprintf(out, "    %s rax, cl\n", op_asm);
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_CMP_EQ:
        case IR_CMP_NE:
        case IR_CMP_LT:
        case IR_CMP_LE:
        case IR_CMP_GT:
        case IR_CMP_GE: {
            emit_load_operand(out, func, &inst->src1, "rax");
            emit_load_operand(out, func, &inst->src2, "rcx");
            fprintf(out, "    cmp rax, rcx\n");

            const char* set_cc = "sete";
            if (inst->opcode == IR_CMP_NE) set_cc = "setne";
            if (inst->opcode == IR_CMP_LT) set_cc = "setl";
            if (inst->opcode == IR_CMP_LE) set_cc = "setle";
            if (inst->opcode == IR_CMP_GT) set_cc = "setg";
            if (inst->opcode == IR_CMP_GE) set_cc = "setge";

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
            fprintf(out, "    ret\n");
            break;
        }

        case IR_CALL: {
            size_t argc = inst->extra_arg_count;

            for (size_t i = 0; i < argc && i < 6; ++i) {
                emit_load_operand(out, func, &inst->extra_args[i], ABI_REG_64[i]);
            }

            fprintf(out, "    call %.*s\n", (int)inst->symbol_name.len, inst->symbol_name.data);

            if (inst->dst.kind != IR_OP_NONE) {
                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }

        case IR_PARAM: {
            size_t param_idx = (size_t)inst->src1.int_val;

            if (param_idx < 6) {
                if (inst->dst.byte_size == 1) {
                    fprintf(out, "    mov byte [rbp %d], %s\n", inst->dst.stack_offset, ABI_REG_8[param_idx]);
                } else if (inst->dst.byte_size == 2) {
                    fprintf(out, "    mov word [rbp %d], %s\n", inst->dst.stack_offset, ABI_REG_16[param_idx]);
                } else if (inst->dst.byte_size == 4) {
                    fprintf(out, "    mov dword [rbp %d], %s\n", inst->dst.stack_offset, ABI_REG_32[param_idx]);
                } else {
                    fprintf(out, "    mov qword [rbp %d], %s\n", inst->dst.stack_offset, ABI_REG_64[param_idx]);
                }
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

        case IR_ADDR_STACK:
        case IR_NOP:
            break;
    }
}

static void emit_function(FILE* out, const IRFunction* func) {
    fprintf(out, "global %.*s\n", (int)func->name.len, func->name.data);
    fprintf(out, "%.*s:\n", (int)func->name.len, func->name.data);

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

    fprintf(out, "; ========================================================\n");
    fprintf(out, "; Generated by klang compiler (x86_64 NASM)\n");
    fprintf(out, "; ========================================================\n\n");
    fprintf(out, "default rel\n\n");

    if (module->str_count > 0) {
        fprintf(out, "section .rodata\n");

        for (const IRStringConst* s = module->first_str; s != NULL; s = s->next) {
            fprintf(out, "    str_%u: db ", s->id);

            for (size_t i = 0; i < s->value.len; ++i) {
                fprintf(out, "0x%02X, ", (unsigned char)s->value.data[i]);
            }

            fprintf(out, "0x00\n");
        }

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