#include "codegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static const char* ABI_REG_64[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };

static const char* x86_size_prefix(size_t bytes) {
    switch (bytes) {
        case 1:  return "byte";
        case 2:  return "word";
        case 4:  return "dword";
        default: return "qword";
    }
}

static const char* x86_reg_name(const char* reg64, size_t bytes) {
    if (strcmp(reg64, "rax") == 0) {
        if (bytes == 1) return "al";
        if (bytes == 2) return "ax";
        if (bytes == 4) return "eax";
        return "rax";
    }

    if (strcmp(reg64, "rcx") == 0) {
        if (bytes == 1) return "cl";
        if (bytes == 2) return "cx";
        if (bytes == 4) return "ecx";
        return "rcx";
    }

    if (strcmp(reg64, "rdx") == 0) {
        if (bytes == 1) return "dl";
        if (bytes == 2) return "dx";
        if (bytes == 4) return "edx";
        return "rdx";
    }

    if (strcmp(reg64, "rsi") == 0) {
        if (bytes == 1) return "sil";
        if (bytes == 2) return "si";
        if (bytes == 4) return "esi";
        return "rsi";
    }

    if (strcmp(reg64, "rdi") == 0) {
        if (bytes == 1) return "dil";
        if (bytes == 2) return "di";
        if (bytes == 4) return "edi";
        return "rdi";
    }

    if (strcmp(reg64, "r8") == 0) {
        if (bytes == 1) return "r8b";
        if (bytes == 2) return "r8w";
        if (bytes == 4) return "r8d";
        return "r8";
    }

    if (strcmp(reg64, "r9") == 0) {
        if (bytes == 1) return "r9b";
        if (bytes == 2) return "r9w";
        if (bytes == 4) return "r9d";
        return "r9";
    }

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
            size_t size = op->byte_size ? op->byte_size : 8;

            if (size == 1) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, byte [rbp %s%d]\n", inst, target_reg, sign, op->stack_offset);
            } else if (size == 2) {
                const char* inst = op->is_signed ? "movsx" : "movzx";
                fprintf(out, "    %s %s, word [rbp %s%d]\n", inst, target_reg, sign, op->stack_offset);
            } else if (size == 4) {
                if (op->is_signed) {
                    fprintf(out, "    movsxd %s, dword [rbp %s%d]\n", target_reg, sign, op->stack_offset);
                } else {
                    const char* reg32 = x86_reg_name(target_reg, 4);
                    fprintf(out, "    mov %s, dword [rbp %s%d]\n", reg32, sign, op->stack_offset);
                }
            } else {
                fprintf(out, "    mov %s, qword [rbp %s%d]\n", target_reg, sign, op->stack_offset);
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
    if (dst->kind == IR_OP_VREG) {
        int32_t off = get_vreg_stack_offset(func, dst->vreg_id);
        fprintf(out, "    mov [rbp %d], rax\n", off);
    } else if (dst->kind == IR_OP_STACK) {
        const char* sign = (dst->stack_offset >= 0) ? "+ " : "";
        size_t size = dst->byte_size ? dst->byte_size : 8;
        const char* prefix = x86_size_prefix(size);
        const char* reg = x86_reg_name("rax", size);

        fprintf(out, "    mov %s [rbp %s%d], %s\n", prefix, sign, dst->stack_offset, reg);
    } else if (dst->kind == IR_OP_GLOBAL) {
        size_t size = dst->byte_size ? dst->byte_size : 8;
        const char* prefix = x86_size_prefix(size);
        const char* reg = x86_reg_name("rax", size);

        fprintf(out, "    mov %s [rel %.*s], %s\n", prefix, (int)dst->global_name.len, dst->global_name.data, reg);
    }
}

static void emit_instruction(FILE* out, const IRFunction* func, const IRInst* inst) {
    switch (inst->opcode) {
        case IR_NOP:
            break;

        case IR_MOV: {
            emit_load_operand(out, func, &inst->src1, "rax");
            emit_store_from_rax(out, func, &inst->dst);
            break;
        }

        case IR_LOAD: {
            if (inst->src1.kind == IR_OP_VREG) {
                emit_load_operand(out, func, &inst->src1, "rcx");
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
            } else {
                emit_load_operand(out, func, &inst->src1, "rax");
                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }

        case IR_STORE: {
            if (inst->dst.kind == IR_OP_VREG) {
                emit_load_operand(out, func, &inst->dst, "rcx");
                emit_load_operand(out, func, &inst->src1, "rax");

                size_t size = inst->dst.byte_size ? inst->dst.byte_size : 8;
                const char* prefix = x86_size_prefix(size);
                const char* reg = x86_reg_name("rax", size);

                fprintf(out, "    mov %s [rcx], %s\n", prefix, reg);
            } else {
                emit_load_operand(out, func, &inst->src1, "rax");
                emit_store_from_rax(out, func, &inst->dst);
            }
            break;
        }

        case IR_ADDR: {
            if (inst->src1.kind == IR_OP_STACK) {
                const char* sign = (inst->src1.stack_offset >= 0) ? "+ " : "";
                fprintf(out, "    lea rax, [rbp %s%d]\n", sign, inst->src1.stack_offset);
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
            emit_load_operand(out, func, &inst->dst, "rdi");
            emit_load_operand(out, func, &inst->src1, "rsi");
            emit_load_operand(out, func, &inst->src2, "rcx");
            fprintf(out, "    cld\n");
            fprintf(out, "    rep movsb\n");
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