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
            const char* sign = (op->stack_offset >= 0) ? "+ " : "";
            if (op->byte_size == 1) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, byte [rbp %s%d]\n", inst, target_reg, sign, op->stack_offset);
            } else if (op->byte_size == 2) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, word [rbp %s%d]\n", inst, target_reg, sign, op->stack_offset);
            } else if (op->byte_size == 4) {
                if (op->is_signed) {
                    fprintf(out, "    movsxd %s, dword [rbp %s%d]\n", target_reg, sign, op->stack_offset);
                } else {
                    fprintf(out, "    mov %s, dword [rbp %s%d]\n", reg64_to_32(target_reg), sign, op->stack_offset);
                }
            } else {
                fprintf(out, "    mov %s, qword [rbp %s%d]\n", target_reg, sign, op->stack_offset);
            }
            break;
        }

        case IR_OP_STR: {
            fprintf(out, "    lea %s, [LC_STR_%u]\n", target_reg, op->str_id);
            break;
        }

        case IR_OP_GLOBAL: {
            StrView gname = op->global_name;
            if (op->byte_size == 1) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, byte [rel %.*s]\n", inst, target_reg, (int)gname.len, gname.data);
            } else if (op->byte_size == 2) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, word [rel %.*s]\n", inst, target_reg, (int)gname.len, gname.data);
            } else if (op->byte_size == 4) {
                if (op->is_signed) {
                    fprintf(out, "    movsxd %s, dword [rel %.*s]\n", target_reg, (int)gname.len, gname.data);
                } else {
                    fprintf(out, "    mov %s, dword [rel %.*s]\n", reg64_to_32(target_reg), (int)gname.len, gname.data);
                }
            } else {
                fprintf(out, "    mov %s, qword [rel %.*s]\n", target_reg, (int)gname.len, gname.data);
            }
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
        const char* sign = (dst->stack_offset >= 0) ? "+ " : "";
        if (dst->byte_size == 1) {
            fprintf(out, "    mov byte [rbp %s%d], al\n", sign, dst->stack_offset);
        } else if (dst->byte_size == 2) {
            fprintf(out, "    mov word [rbp %s%d], ax\n", sign, dst->stack_offset);
        } else if (dst->byte_size == 4) {
            fprintf(out, "    mov dword [rbp %s%d], eax\n", sign, dst->stack_offset);
        } else {
            fprintf(out, "    mov qword [rbp %s%d], rax\n", sign, dst->stack_offset);
        }
    }
}

static void emit_instruction(FILE* out, const IRFunction* func, const IRInst* inst) {
    switch (inst->opcode) {
        case IR_LOAD: {
            emit_load_operand(out, func, &inst->src1, "rcx");

            if (inst->dst.byte_size == 1) {
                const char* op = inst->dst.is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s rax, byte [rcx]\n", op);
            } else if (inst->dst.byte_size == 2) {
                const char* op = inst->dst.is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s rax, word [rcx]\n", op);
            } else if (inst->dst.byte_size == 4) {
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
            fprintf(out, "    lea rax, [LC_STR_%u]\n", inst->src1.str_id);
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

        case IR_DIV:
        case IR_MOD: {
            emit_load_operand(out, func, &inst->src1, "rax");
            emit_load_operand(out, func, &inst->src2, "rcx");
            if (inst->src1.is_signed) {
                fprintf(out, "    cqo\n");
                fprintf(out, "    idiv rcx\n");
            } else {
                fprintf(out, "    xor edx, edx\n");
                fprintf(out, "    div rcx\n");
            }
            if (inst->opcode == IR_MOD) {
                fprintf(out, "    mov rax, rdx\n");
            }
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
            const char* op_asm = (inst->opcode == IR_SHL) ? "shl" : (inst->src1.is_signed ? "sar" : "shr");
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
            fprintf(out, "    ret\n");
            break;
        }

        case IR_CALL: {
            size_t argc = inst->extra_arg_count;
            size_t stack_args = (argc > 6) ? (argc - 6) : 0;

            bool needs_padding = (stack_args % 2) != 0;
            if (needs_padding) {
                fprintf(out, "    sub rsp, 8\n");
            }

            for (size_t i = argc; i > 6; --i) {
                emit_load_operand(out, func, &inst->extra_args[i - 1], "rax");
                fprintf(out, "    push rax\n");
            }

            for (size_t i = 0; i < argc && i < 6; ++i) {
                emit_load_operand(out, func, &inst->extra_args[i], ABI_REG_64[i]);
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

        case IR_ADDR_STACK: {
            fprintf(out, "    lea rax, [rbp %d]\n", inst->src1.stack_offset);
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_LOAD_GLOBAL: {
            StrView gname = inst->src1.global_name;
            if (inst->dst.byte_size == 1) {
                const char* op = inst->dst.is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s rax, byte [rel %.*s]\n", op, (int)gname.len, gname.data);
            } else if (inst->dst.byte_size == 2) {
                const char* op = inst->dst.is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s rax, word [rel %.*s]\n", op, (int)gname.len, gname.data);
            } else if (inst->dst.byte_size == 4) {
                if (inst->dst.is_signed) {
                    fprintf(out, "    movsxd rax, dword [rel %.*s]\n", (int)gname.len, gname.data);
                } else {
                    fprintf(out, "    mov eax, dword [rel %.*s]\n", (int)gname.len, gname.data);
                }
            } else {
                fprintf(out, "    mov rax, qword [rel %.*s]\n", (int)gname.len, gname.data);
            }
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_STORE_GLOBAL: {
            emit_load_operand(out, func, &inst->src1, "rax");
            StrView gname = inst->dst.global_name;
            if (inst->dst.byte_size == 1) {
                fprintf(out, "    mov byte [rel %.*s], al\n", (int)gname.len, gname.data);
            } else if (inst->dst.byte_size == 2) {
                fprintf(out, "    mov word [rel %.*s], ax\n", (int)gname.len, gname.data);
            } else if (inst->dst.byte_size == 4) {
                fprintf(out, "    mov dword [rel %.*s], eax\n", (int)gname.len, gname.data);
            } else {
                fprintf(out, "    mov qword [rel %.*s], rax\n", (int)gname.len, gname.data);
            }
            break;
        }

        case IR_ADDR_GLOBAL: {
            StrView gname = inst->src1.global_name;
            fprintf(out, "    lea rax, [rel %.*s]\n", (int)gname.len, gname.data);
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

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
            fprintf(out, "    global %.*s\n", (int)g->name.len, g->name.data);
            fprintf(out, "    %.*s: dq %lld\n", (int)g->name.len, g->name.data, (long long)g->init_val);
        }
    }
    if (has_data) fprintf(out, "\n");

    bool has_bss = false;
    for (const IRGlobalVar* g = module->first_global; g != NULL; g = g->next) {
        if (!g->has_init) {
            if (!has_bss) {
                fprintf(out, "section .bss\n");
                has_bss = true;
            }
            size_t size = g->type->size ? g->type->size : 8;
            fprintf(out, "    global %.*s\n", (int)g->name.len, g->name.data);
            fprintf(out, "    %.*s: resb %zu\n", (int)g->name.len, g->name.data, size);
        }
    }
    if (has_bss) fprintf(out, "\n");

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