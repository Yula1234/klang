#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

IROperand ir_op_none(void) {
    return (IROperand){ .kind = IR_OP_NONE, .byte_size = 0, .is_signed = false, .int_val = 0 };
}

IROperand ir_op_const(int64_t val, size_t byte_size, bool is_signed) {
    return (IROperand){
        .kind      = IR_OP_CONST,
        .byte_size = (byte_size == 0) ? 8 : byte_size,
        .is_signed = is_signed,
        .int_val   = val
    };
}

IROperand ir_op_vreg(uint32_t vreg_id, size_t byte_size, bool is_signed) {
    return (IROperand){
        .kind      = IR_OP_VREG,
        .byte_size = (byte_size == 0) ? 8 : byte_size,
        .is_signed = is_signed,
        .vreg_id   = vreg_id
    };
}

IROperand ir_op_stack(int32_t stack_offset, size_t byte_size, bool is_signed) {
    return (IROperand){
        .kind         = IR_OP_STACK,
        .byte_size    = (byte_size == 0) ? 8 : byte_size,
        .is_signed    = is_signed,
        .stack_offset = stack_offset
    };
}

IROperand ir_op_global(StrView name, size_t byte_size, bool is_signed) {
    return (IROperand){
        .kind        = IR_OP_GLOBAL,
        .byte_size   = (byte_size == 0) ? 8 : byte_size,
        .is_signed   = is_signed,
        .global_name = name
    };
}

IROperand ir_op_str(uint32_t str_id) {
    return (IROperand){
        .kind      = IR_OP_STR,
        .byte_size = 8,
        .str_id    = str_id
    };
}

int32_t ir_func_alloc_stack_slot(IRFunction* func, size_t size, size_t align) {
    if (align == 0) {
        align = 8;
    }
    size_t aligned_size = (size + align - 1) & ~(align - 1);
    func->stack_frame_size = (func->stack_frame_size + aligned_size + (align - 1)) & ~(align - 1);
    return -(int32_t)func->stack_frame_size;
}

IROperand ir_op_block(IRBlock* block) {
    return (IROperand){
        .kind      = IR_OP_BLOCK,
        .byte_size = 8,
        .block     = block
    };
}

IRModule* ir_module_create(Arena* arena) {
    IRModule* module = ARENA_NEW_ZERO(arena, IRModule);

    module->arena      = arena;
    module->first_func = NULL;
    module->last_func  = NULL;
    module->func_count = 0;
    module->first_str  = NULL;
    module->last_str   = NULL;
    module->str_count  = 0;

    return module;
}

IRFunction* ir_function_create(IRModule* module, StrView name, Type* return_type, size_t stack_size) {
    IRFunction* func = ARENA_NEW_ZERO(module->arena, IRFunction);

    func->arena            = module->arena;
    func->name             = name;
    func->return_type      = return_type;
    func->stack_frame_size = stack_size;
    func->next_vreg_id     = 0;
    func->next_block_id    = 0;
    func->block_count      = 0;

    IRBlock* entry = ir_block_create(func, "bb_entry");
    func->entry_block   = entry;
    func->current_block = entry;

    if (!module->first_func) {
        module->first_func = func;
        module->last_func  = func;
    } else {
        module->last_func->next = func;
        module->last_func       = func;
    }

    module->func_count++;

    return func;
}

IRBlock* ir_block_create(IRFunction* func, const char* prefix) {
    char* block_name = arena_sprintf(func->arena, "%s_%u", prefix, func->next_block_id++);
    
    IRBlock* block = ARENA_NEW_ZERO(func->arena, IRBlock);

    block->name          = block_name;
    block->id            = func->next_block_id - 1;
    block->first_inst    = NULL;
    block->last_inst     = NULL;
    block->inst_count    = 0;
    block->is_terminated = false;
    block->next_block    = NULL;

    if (!func->first_block) {
        func->first_block = block;
        func->last_block  = block;
    } else {
        func->last_block->next_block = block;
        func->last_block             = block;
    }

    func->block_count++;

    return block;
}

void ir_block_switch(IRFunction* func, IRBlock* block) {
    func->current_block = block;
}

static uint32_t ir_vreg_alloc(IRFunction* func) {
    return func->next_vreg_id++;
}

IRInst* ir_emit_inst(IRFunction* func, IROpcode op, IROperand dst, IROperand src1, IROperand src2, SourceLoc loc) {
    IRBlock* block = func->current_block;

    if (block->is_terminated) {
        return NULL;
    }

    IRInst* inst = ARENA_NEW_ZERO(func->arena, IRInst);

    inst->opcode = op;
    inst->dst    = dst;
    inst->src1   = src1;
    inst->src2   = src2;
    inst->loc    = loc;
    inst->next   = NULL;

    if (op == IR_JMP || op == IR_BR || op == IR_RET) {
        block->is_terminated = true;
    }

    if (!block->first_inst) {
        block->first_inst = inst;
        block->last_inst  = inst;
    } else {
        block->last_inst->next = inst;
        block->last_inst       = inst;
    }

    block->inst_count++;

    return inst;
}

typedef struct LoopContext {
    IRBlock*            bb_cond;
    IRBlock*            bb_end;
    struct LoopContext* prev;
} LoopContext;

typedef struct IRLower {
    Arena*       arena;
    IRModule*    module;
    IRFunction*  current_func;
    LoopContext* current_loop;

    int32_t      current_sret_slot;
} IRLower;

static IROperand ir_lower_expr(IRLower* lower, const AstExpr* expr);

static IROperand ir_lower_addr(IRLower* lower, const AstExpr* expr) {
    IRFunction* func = lower->current_func;

    if (!expr) {
        return ir_op_none();
    }

    switch (expr->kind) {
        case EXPR_VAR: {
            Symbol* sym = expr->var.symbol;
            uint32_t vreg = ir_vreg_alloc(func);
            if (sym->kind == SYM_GLOBAL_VAR) {
                ir_emit_inst(func, IR_ADDR_GLOBAL, ir_op_vreg(vreg, 8, false),
                             ir_op_global(sym->name, 8, false), ir_op_none(), expr->loc);
            } else {
                ir_emit_inst(func, IR_ADDR_STACK, ir_op_vreg(vreg, 8, false),
                             ir_op_stack(sym->stack_offset, 8, false), ir_op_none(), expr->loc);
            }
            return ir_op_vreg(vreg, 8, false);
        }

        case EXPR_INDEX: {
            IROperand ptr_op = ir_lower_expr(lower, expr->index.ptr);
            IROperand idx_op = ir_lower_expr(lower, expr->index.index);
            size_t elem_size = (expr->type && expr->type->size) ? expr->type->size : 8;

            IROperand offset_op = idx_op;
            if (elem_size > 1) {
                uint32_t scale_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8, false), idx_op,
                             ir_op_const((int64_t)elem_size, 8, false), expr->loc);
                offset_op = ir_op_vreg(scale_vreg, 8, false);
            }

            uint32_t addr_vreg = ir_vreg_alloc(func);
            ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ptr_op, offset_op, expr->loc);
            return ir_op_vreg(addr_vreg, 8, false);
        }

        case EXPR_MEMBER: {
            StructField* field = expr->member.field;
            if (type_is_pointer(expr->member.target->type)) {
                IROperand ptr_op = ir_lower_expr(lower, expr->member.target);
                uint32_t addr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ptr_op,
                             ir_op_const((int64_t)field->offset, 8, false), expr->loc);
                return ir_op_vreg(addr_vreg, 8, false);
            } else {
                IROperand base_addr = ir_lower_addr(lower, expr->member.target);
                uint32_t addr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), base_addr,
                             ir_op_const((int64_t)field->offset, 8, false), expr->loc);
                return ir_op_vreg(addr_vreg, 8, false);
            }
        }

        case EXPR_UNARY: {
            if (expr->unary.op == TOK_STAR) {
                return ir_lower_expr(lower, expr->unary.operand);
            }
            break;
        }

        default: {
            IROperand val = ir_lower_expr(lower, expr);
            if (val.kind == IR_OP_STACK) {
                uint32_t addr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADDR_STACK, ir_op_vreg(addr_vreg, 8, false),
                             val, ir_op_none(), expr->loc);
                return ir_op_vreg(addr_vreg, 8, false);
            }
            return val;
        }
    }

    return ir_op_none();
}

static uint32_t register_string_literal(IRLower* lower, StrView str) {
    for (IRStringConst* s = lower->module->first_str; s != NULL; s = s->next) {
        if (s->value.len == str.len && memcmp(s->value.data, str.data, str.len) == 0) {
            return s->id;
        }
    }

    IRStringConst* sc = ARENA_NEW_ZERO(lower->arena, IRStringConst);

    sc->id    = (uint32_t)lower->module->str_count++;
    sc->value = str;
    sc->next  = NULL;

    if (!lower->module->first_str) {
        lower->module->first_str = sc;
        lower->module->last_str  = sc;
    } else {
        lower->module->last_str->next = sc;
        lower->module->last_str       = sc;
    }

    return sc->id;
}

static IROperand ir_lower_expr(IRLower* lower, const AstExpr* expr) {
    if (!expr) {
        return ir_op_none();
    }

    IRFunction* func = lower->current_func;
    size_t expr_size = (expr->type && expr->type->size) ? expr->type->size : 8;

    switch (expr->kind) {
        case EXPR_INT_LIT: {
            return ir_op_const(expr->int_val, expr_size, false);
        }

        case EXPR_STRING_LIT: {
            uint32_t str_id = register_string_literal(lower, expr->string_val);
            uint32_t vreg   = ir_vreg_alloc(func);

            ir_emit_inst(func, IR_GLOBAL_STR, ir_op_vreg(vreg, 8, false), ir_op_str(str_id), ir_op_none(), expr->loc);

            return ir_op_vreg(vreg, 8, false);
        }

        case EXPR_VAR: {
            assert(expr->var.symbol != NULL);

            if (expr->var.symbol->kind == SYM_CONST) {
                return ir_op_const(expr->var.symbol->const_val, expr_size, false);
            }

            if (expr->var.symbol->kind == SYM_GLOBAL_VAR) {
                if (expr->type && expr->type->kind == TYPE_ARRAY) {
                    uint32_t vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR_GLOBAL, ir_op_vreg(vreg, 8, false),
                                ir_op_global(expr->var.name, 8, false), ir_op_none(), expr->loc);
                    return ir_op_vreg(vreg, 8, false);
                }

                uint32_t vreg = ir_vreg_alloc(func);
                bool is_signed = type_is_signed(expr->type);
                ir_emit_inst(func, IR_LOAD_GLOBAL, ir_op_vreg(vreg, expr_size, is_signed),
                            ir_op_global(expr->var.name, expr_size, is_signed), ir_op_none(), expr->loc);
                return ir_op_vreg(vreg, expr_size, is_signed);
            }

            if (expr->type && expr->type->kind == TYPE_ARRAY) {
                uint32_t vreg = ir_vreg_alloc(func);
                int32_t offset = expr->var.symbol->stack_offset;
                ir_emit_inst(func, IR_ADDR_STACK, ir_op_vreg(vreg, 8, false),
                             ir_op_stack(offset, 8, false), ir_op_none(), expr->loc);
                return ir_op_vreg(vreg, 8, false);
            }

            uint32_t vreg = ir_vreg_alloc(func);
            int32_t offset = expr->var.symbol->stack_offset;
            bool is_signed = type_is_signed(expr->type);

            ir_emit_inst(func, IR_LOAD_STACK, ir_op_vreg(vreg, expr_size, is_signed),
                         ir_op_stack(offset, expr_size, is_signed), ir_op_none(), expr->loc);

            return ir_op_vreg(vreg, expr_size, is_signed);
        }

        case EXPR_UNARY: {
            if (expr->unary.op == TOK_STAR) {
                IROperand ptr_op = ir_lower_expr(lower, expr->unary.operand);
                uint32_t vreg = ir_vreg_alloc(func);
                bool is_signed = type_is_signed(expr->type);

                ir_emit_inst(func, IR_LOAD, ir_op_vreg(vreg, expr_size, is_signed), ptr_op, ir_op_none(), expr->loc);

                return ir_op_vreg(vreg, expr_size, is_signed);
            }

            if (expr->unary.op == TOK_AMP) {
                const AstExpr* target = expr->unary.operand;

                if (target->kind == EXPR_VAR) {
                    if (target->var.symbol->kind == SYM_GLOBAL_VAR) {
                        uint32_t vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_ADDR_GLOBAL, ir_op_vreg(vreg, 8, false),
                                    ir_op_global(target->var.name, 8, false), ir_op_none(), expr->loc);
                        return ir_op_vreg(vreg, 8, false);
                    } else {
                        uint32_t vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_ADDR_STACK, ir_op_vreg(vreg, 8, false),
                                     ir_op_stack(target->var.symbol->stack_offset, 8, false), ir_op_none(), expr->loc);
                        return ir_op_vreg(vreg, 8, false);
                    }
                }

                if (target->kind == EXPR_INDEX) {
                    IROperand ptr_op = ir_lower_expr(lower, target->index.ptr);
                    IROperand idx_op = ir_lower_expr(lower, target->index.index);
                    size_t elem_size = target->type->size ? target->type->size : 8;

                    IROperand offset_op = idx_op;
                    if (elem_size > 1) {
                        uint32_t scale_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8, false), idx_op,
                                     ir_op_const((int64_t)elem_size, 8, false), expr->loc);
                        offset_op = ir_op_vreg(scale_vreg, 8, false);
                    }

                    uint32_t addr_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ptr_op, offset_op, expr->loc);
                    return ir_op_vreg(addr_vreg, 8, false);
                }

                if (target->kind == EXPR_MEMBER) {
                    StructField* field = target->member.field;
                    if (type_is_pointer(target->member.target->type)) {
                        IROperand ptr_op = ir_lower_expr(lower, target->member.target);
                        uint32_t addr_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ptr_op,
                                     ir_op_const((int64_t)field->offset, 8, false), expr->loc);
                        return ir_op_vreg(addr_vreg, 8, false);
                    } else if (target->member.target->kind == EXPR_VAR) {
                        Symbol* sym = target->member.target->var.symbol;
                        uint32_t addr_vreg = ir_vreg_alloc(func);

                        if (sym->kind == SYM_GLOBAL_VAR) {
                            ir_emit_inst(func, IR_ADDR_GLOBAL, ir_op_vreg(addr_vreg, 8, false),
                                         ir_op_global(sym->name, 8, false), ir_op_none(), expr->loc);
                            ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ir_op_vreg(addr_vreg, 8, false),
                                         ir_op_const((int64_t)field->offset, 8, false), expr->loc);
                        } else {
                            int32_t field_off = sym->stack_offset + (int32_t)field->offset;
                            ir_emit_inst(func, IR_ADDR_STACK, ir_op_vreg(addr_vreg, 8, false),
                                         ir_op_stack(field_off, 8, false), ir_op_none(), expr->loc);
                        }

                        return ir_op_vreg(addr_vreg, 8, false);
                    }
                }
            }

            if (expr->unary.op == TOK_MINUS) {
                IROperand inner_op = ir_lower_expr(lower, expr->unary.operand);
                uint32_t vreg = ir_vreg_alloc(func);
                bool is_signed = type_is_signed(expr->type);

                ir_emit_inst(func, IR_NEG, ir_op_vreg(vreg, expr_size, is_signed), inner_op, ir_op_none(), expr->loc);

                return ir_op_vreg(vreg, expr_size, is_signed);
            }

            if (expr->unary.op == TOK_TILDE) {
                IROperand inner_op = ir_lower_expr(lower, expr->unary.operand);
                uint32_t vreg = ir_vreg_alloc(func);

                ir_emit_inst(func, IR_NOT, ir_op_vreg(vreg, expr_size, false), inner_op, ir_op_none(), expr->loc);

                return ir_op_vreg(vreg, expr_size, false);
            }

            if (expr->unary.op == TOK_BANG) {
                IROperand inner_op = ir_lower_expr(lower, expr->unary.operand);
                uint32_t vreg = ir_vreg_alloc(func);

                ir_emit_inst(func, IR_CMP_EQ, ir_op_vreg(vreg, 1, false), inner_op, ir_op_const(0, inner_op.byte_size, false), expr->loc);

                return ir_op_vreg(vreg, 1, false);
            }

            return ir_lower_expr(lower, expr->unary.operand);
        }

        case EXPR_BINARY: {
            if (expr->binary.op == TOK_AMP_AMP || expr->binary.op == TOK_PIPE_PIPE) {
                IRBlock* bb_rhs   = ir_block_create(func, "bb_logic_rhs");
                IRBlock* bb_merge = ir_block_create(func, "bb_logic_merge");

                uint32_t res_vreg = ir_vreg_alloc(func);
                int32_t tmp_slot  = -(int32_t)(func->stack_frame_size + (res_vreg + 1) * 8);

                IROperand lhs = ir_lower_expr(lower, expr->binary.lhs);

                if (expr->binary.op == TOK_AMP_AMP) {
                    ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(tmp_slot, 1, false), ir_op_const(0, 1, false), ir_op_none(), expr->loc);
                    ir_emit_inst(func, IR_BR, lhs, ir_op_block(bb_rhs), ir_op_block(bb_merge), expr->loc);

                    ir_block_switch(func, bb_rhs);
                    IROperand rhs = ir_lower_expr(lower, expr->binary.rhs);
                    uint32_t bool_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_CMP_NE, ir_op_vreg(bool_vreg, 1, false), rhs, ir_op_const(0, rhs.byte_size, false), expr->loc);
                    ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(tmp_slot, 1, false), ir_op_vreg(bool_vreg, 1, false), ir_op_none(), expr->loc);
                    if (!bb_rhs->is_terminated) {
                        ir_emit_inst(func, IR_JMP, ir_op_block(bb_merge), ir_op_none(), ir_op_none(), expr->loc);
                    }
                } else {
                    ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(tmp_slot, 1, false), ir_op_const(1, 1, false), ir_op_none(), expr->loc);
                    ir_emit_inst(func, IR_BR, lhs, ir_op_block(bb_merge), ir_op_block(bb_rhs), expr->loc);

                    ir_block_switch(func, bb_rhs);
                    IROperand rhs = ir_lower_expr(lower, expr->binary.rhs);
                    uint32_t bool_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_CMP_NE, ir_op_vreg(bool_vreg, 1, false), rhs, ir_op_const(0, rhs.byte_size, false), expr->loc);
                    ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(tmp_slot, 1, false), ir_op_vreg(bool_vreg, 1, false), ir_op_none(), expr->loc);
                    if (!bb_rhs->is_terminated) {
                        ir_emit_inst(func, IR_JMP, ir_op_block(bb_merge), ir_op_none(), ir_op_none(), expr->loc);
                    }
                }

                ir_block_switch(func, bb_merge);
                ir_emit_inst(func, IR_LOAD_STACK, ir_op_vreg(res_vreg, 1, false), ir_op_stack(tmp_slot, 1, false), ir_op_none(), expr->loc);
                return ir_op_vreg(res_vreg, 1, false);
            }

            IROperand lhs = ir_lower_expr(lower, expr->binary.lhs);
            IROperand rhs = ir_lower_expr(lower, expr->binary.rhs);
            uint32_t vreg = ir_vreg_alloc(func);
            bool is_signed = type_is_signed(expr->type);

            IROpcode op = IR_ADD;

            switch (expr->binary.op) {
                case TOK_PLUS:       op = IR_ADD;    break;
                case TOK_MINUS:      op = IR_SUB;    break;
                case TOK_STAR:       op = IR_MUL;    break;
                case TOK_SLASH:      op = IR_DIV;    break;
                case TOK_PERCENT:    op = IR_MOD;    break;
                case TOK_AMP:        op = IR_AND;    break;
                case TOK_PIPE:       op = IR_OR;     break;
                case TOK_CARET:      op = IR_XOR;    break;
                case TOK_SHL:        op = IR_SHL;    break;
                case TOK_SHR:        op = IR_SHR;    break;
                case TOK_EQ_EQ:      op = IR_CMP_EQ; break;
                case TOK_BANG_EQ:    op = IR_CMP_NE; break;
                case TOK_LESS:       op = IR_CMP_LT; break;
                case TOK_LESS_EQ:    op = IR_CMP_LE; break;
                case TOK_GREATER:    op = IR_CMP_GT; break;
                case TOK_GREATER_EQ: op = IR_CMP_GE; break;
                default: break;
            }

            ir_emit_inst(func, op, ir_op_vreg(vreg, expr_size, is_signed), lhs, rhs, expr->loc);

            return ir_op_vreg(vreg, expr_size, is_signed);
        }

        case EXPR_CAST: {
            IROperand inner_op = ir_lower_expr(lower, expr->cast.expr);
            uint32_t vreg = ir_vreg_alloc(func);
            bool is_signed = type_is_signed(expr->type);

            ir_emit_inst(func, IR_ADD, ir_op_vreg(vreg, expr_size, is_signed), inner_op, ir_op_const(0, expr_size, false), expr->loc);

            return ir_op_vreg(vreg, expr_size, is_signed);
        }

        case EXPR_INDEX: {
            IROperand ptr_op = ir_lower_expr(lower, expr->index.ptr);
            IROperand idx_op = ir_lower_expr(lower, expr->index.index);
            size_t elem_size = (expr->type && expr->type->size) ? expr->type->size : 8;
            bool is_signed = type_is_signed(expr->type);

            IROperand offset_op = idx_op;

            if (elem_size > 1) {
                uint32_t scale_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8, false), idx_op,
                             ir_op_const((int64_t)elem_size, 8, false), expr->loc);
                offset_op = ir_op_vreg(scale_vreg, 8, false);
            }

            uint32_t addr_vreg = ir_vreg_alloc(func);
            ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ptr_op, offset_op, expr->loc);

            uint32_t val_vreg = ir_vreg_alloc(func);
            ir_emit_inst(func, IR_LOAD, ir_op_vreg(val_vreg, elem_size, is_signed),
                         ir_op_vreg(addr_vreg, 8, false), ir_op_none(), expr->loc);

            return ir_op_vreg(val_vreg, elem_size, is_signed);
        }

        case EXPR_ASM: {
            IROperand dst = ir_op_none();

            if (expr->type && expr->type->kind != TYPE_VOID) {
                size_t size = expr->type->size ? expr->type->size : 8;
                bool is_signed = type_is_signed(expr->type);
                uint32_t vreg = ir_vreg_alloc(func);
                dst = ir_op_vreg(vreg, size, is_signed);
            }

            IRInst* inst = ir_emit_inst(func, IR_INLINE_ASM, dst, ir_op_none(), ir_op_none(), expr->loc);
            inst->symbol_name = expr->inline_asm.code;

            return dst;
        }

        case EXPR_STRUCT_LIT: {
            Type* st = expr->struct_lit.struct_type;
            size_t total_size = (st && st->size) ? st->size : 8;

            int32_t tmp_slot = ir_func_alloc_stack_slot(func, total_size, st ? st->align : 8);

            for (size_t i = 0; i < expr->struct_lit.field_count; ++i) {
                StructField* f = type_struct_lookup_field(st, expr->struct_lit.field_names[i]);

                if (f) {
                    IROperand val = ir_lower_expr(lower, expr->struct_lit.field_values[i]);
                    size_t f_size = (f->type && f->type->size) ? f->type->size : 8;
                    int32_t f_offset = tmp_slot + (int32_t)f->offset;

                    ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(f_offset, f_size, false), val, ir_op_none(), expr->loc);
                }
            }

            return ir_op_stack(tmp_slot, total_size, false);
        }

        case EXPR_MEMBER: {
            StructField* field = expr->member.field;
            size_t size = expr->type->size ? expr->type->size : 8;
            bool is_signed = type_is_signed(expr->type);

            if (type_is_pointer(expr->member.target->type)) {
                IROperand ptr_op = ir_lower_expr(lower, expr->member.target);
                uint32_t addr_vreg = ir_vreg_alloc(func);

                ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ptr_op,
                             ir_op_const((int64_t)field->offset, 8, false), expr->loc);

                uint32_t val_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_LOAD, ir_op_vreg(val_vreg, size, is_signed),
                             ir_op_vreg(addr_vreg, 8, false), ir_op_none(), expr->loc);

                return ir_op_vreg(val_vreg, size, is_signed);
            }

            if (expr->member.target->kind == EXPR_INDEX) {
                IROperand ptr_op = ir_lower_expr(lower, expr->member.target->index.ptr);
                IROperand idx_op = ir_lower_expr(lower, expr->member.target->index.index);
                size_t elem_size = expr->member.target->type->size ? expr->member.target->type->size : 8;

                IROperand offset_op = idx_op;
                if (elem_size > 1) {
                    uint32_t scale_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8, false), idx_op,
                                 ir_op_const((int64_t)elem_size, 8, false), expr->loc);
                    offset_op = ir_op_vreg(scale_vreg, 8, false);
                }

                uint32_t elem_addr = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(elem_addr, 8, false), ptr_op, offset_op, expr->loc);

                uint32_t field_addr = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(field_addr, 8, false), ir_op_vreg(elem_addr, 8, false),
                             ir_op_const((int64_t)field->offset, 8, false), expr->loc);

                uint32_t val_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_LOAD, ir_op_vreg(val_vreg, size, is_signed),
                             ir_op_vreg(field_addr, 8, false), ir_op_none(), expr->loc);

                return ir_op_vreg(val_vreg, size, is_signed);
            }

            if (expr->member.target->kind == EXPR_VAR) {
                Symbol* sym = expr->member.target->var.symbol;
                uint32_t vreg = ir_vreg_alloc(func);

                if (sym->kind == SYM_GLOBAL_VAR) {
                    uint32_t addr_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR_GLOBAL, ir_op_vreg(addr_vreg, 8, false),
                                 ir_op_global(sym->name, 8, false), ir_op_none(), expr->loc);
                    ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ir_op_vreg(addr_vreg, 8, false),
                                 ir_op_const((int64_t)field->offset, 8, false), expr->loc);
                    ir_emit_inst(func, IR_LOAD, ir_op_vreg(vreg, size, is_signed),
                                 ir_op_vreg(addr_vreg, 8, false), ir_op_none(), expr->loc);
                } else {
                    int32_t base_off = sym->stack_offset;
                    int32_t field_off = base_off + (int32_t)field->offset;
                    ir_emit_inst(func, IR_LOAD_STACK, ir_op_vreg(vreg, size, is_signed),
                                 ir_op_stack(field_off, size, is_signed), ir_op_none(), expr->loc);
                }

                return ir_op_vreg(vreg, size, is_signed);
            }

            return ir_op_none();
        }

        case EXPR_CALL: {
            bool ret_is_struct = (expr->type && expr->type->kind == TYPE_STRUCT);
            size_t total_args = expr->call.arg_count + (ret_is_struct ? 1 : 0);
            IROperand* args = ARENA_NEW_ARRAY(lower->arena, IROperand, total_args);
            size_t arg_idx = 0;

            int32_t ret_slot = 0;
            if (ret_is_struct) {
                size_t ret_size = expr->type->size ? expr->type->size : 8;
                size_t ret_align = expr->type->align ? expr->type->align : 8;
                ret_slot = ir_func_alloc_stack_slot(func, ret_size, ret_align);
                uint32_t ret_addr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADDR_STACK, ir_op_vreg(ret_addr_vreg, 8, false),
                             ir_op_stack(ret_slot, 8, false), ir_op_none(), expr->loc);
                args[arg_idx++] = ir_op_vreg(ret_addr_vreg, 8, false); // Скрытый sret указатель
            }

            for (size_t i = 0; i < expr->call.arg_count; ++i) {
                const AstExpr* arg = expr->call.args[i];
                if (arg->type && arg->type->kind == TYPE_STRUCT) {
                    args[arg_idx++] = ir_lower_addr(lower, arg);
                } else {
                    args[arg_idx++] = ir_lower_expr(lower, arg);
                }
            }

            if (ret_is_struct) {
                IRInst* call_inst = ir_emit_inst(func, IR_CALL, ir_op_none(), ir_op_none(), ir_op_none(), expr->loc);
                call_inst->symbol_name    = expr->call.callee_name;
                call_inst->extra_args      = args;
                call_inst->extra_arg_count = total_args;

                return ir_op_stack(ret_slot, expr->type->size, false);
            }

            uint32_t vreg = ir_vreg_alloc(func);
            bool is_signed = type_is_signed(expr->type);
            IROperand dst = (expr->type && expr->type->kind != TYPE_VOID) ? ir_op_vreg(vreg, expr_size, is_signed) : ir_op_none();

            IRInst* call_inst = ir_emit_inst(func, IR_CALL, dst, ir_op_none(), ir_op_none(), expr->loc);
            call_inst->symbol_name     = expr->call.callee_name;
            call_inst->extra_args       = args;
            call_inst->extra_arg_count  = total_args;

            return dst;
        }
    }

    return ir_op_none();
}

static void ir_lower_stmt(IRLower* lower, const AstStmt* stmt);

static void ir_lower_stmt(IRLower* lower, const AstStmt* stmt) {
    if (!stmt) {
        return;
    }

    IRFunction* func = lower->current_func;

    switch (stmt->kind) {
        case STMT_BLOCK: {
            for (size_t i = 0; i < stmt->block.count; ++i) {
                ir_lower_stmt(lower, stmt->block.stmts[i]);
            }
            break;
        }

        case STMT_VAR_DECL: {
            Symbol* sym = stmt->var_decl.symbol;
            int32_t offset = sym->stack_offset;
            size_t size = (sym->type && sym->type->size) ? sym->type->size : 8;
            bool is_signed = type_is_signed(sym->type);

            if (stmt->var_decl.init_expr) {
                if (sym->type && sym->type->kind == TYPE_STRUCT) {
                    uint32_t dst_addr = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR_STACK, ir_op_vreg(dst_addr, 8, false),
                                 ir_op_stack(offset, 8, false), ir_op_none(), stmt->loc);
                    IROperand src_addr = ir_lower_addr(lower, stmt->var_decl.init_expr);
                    ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(dst_addr, 8, false),
                                 src_addr, ir_op_const((int64_t)size, 8, false), stmt->loc);
                } else {
                    IROperand init_val = ir_lower_expr(lower, stmt->var_decl.init_expr);
                    ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(offset, size, is_signed), init_val, ir_op_none(), stmt->loc);
                }
            }
            break;
        }

        case STMT_ASSIGN: {
            if (stmt->assign.target->type && stmt->assign.target->type->kind == TYPE_STRUCT) {
                size_t size = stmt->assign.target->type->size;
                
                IROperand dst_addr = ir_lower_addr(lower, stmt->assign.target);
                IROperand src_addr = ir_lower_addr(lower, stmt->assign.value);

                ir_emit_inst(func, IR_MEMCPY, dst_addr, src_addr, ir_op_const((int64_t)size, 8, false), stmt->loc);
                break;
            }

            IROperand val = ir_lower_expr(lower, stmt->assign.value);

            if (stmt->assign.target->kind == EXPR_VAR) {
                if (stmt->assign.target->var.symbol->kind == SYM_GLOBAL_VAR) {
                    size_t size = stmt->assign.target->type->size;
                    bool is_signed = type_is_signed(stmt->assign.target->type);
                    ir_emit_inst(func, IR_STORE_GLOBAL, ir_op_global(stmt->assign.target->var.name, size, is_signed), val, ir_op_none(), stmt->loc);
                } else {
                    int32_t offset = stmt->assign.target->var.symbol->stack_offset;
                    size_t size = stmt->assign.target->type->size;
                    bool is_signed = type_is_signed(stmt->assign.target->type);
                    ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(offset, size, is_signed), val, ir_op_none(), stmt->loc);
                }
            } else if (stmt->assign.target->kind == EXPR_UNARY && stmt->assign.target->unary.op == TOK_STAR) {
                IROperand ptr_op = ir_lower_expr(lower, stmt->assign.target->unary.operand);
                size_t size = stmt->assign.target->type->size;
                ptr_op.byte_size = size;

                ir_emit_inst(func, IR_STORE, ptr_op, val, ir_op_none(), stmt->loc);
            } else if (stmt->assign.target->kind == EXPR_INDEX) {
                IROperand ptr_op = ir_lower_expr(lower, stmt->assign.target->index.ptr);
                IROperand idx_op = ir_lower_expr(lower, stmt->assign.target->index.index);
                size_t elem_size = (stmt->assign.target->type && stmt->assign.target->type->size) ? stmt->assign.target->type->size : 8;

                IROperand offset_op = idx_op;

                if (elem_size > 1) {
                    uint32_t scale_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8, false), idx_op, 
                                 ir_op_const((int64_t)elem_size, 8, false), stmt->loc);
                    offset_op = ir_op_vreg(scale_vreg, 8, false);
                }

                uint32_t addr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ptr_op, offset_op, stmt->loc);

                IROperand addr_op = ir_op_vreg(addr_vreg, 8, false);
                addr_op.byte_size = elem_size;

                ir_emit_inst(func, IR_STORE, addr_op, val, ir_op_none(), stmt->loc);
            } else if (stmt->assign.target->kind == EXPR_MEMBER) {
                StructField* field = stmt->assign.target->member.field;
                size_t size = stmt->assign.target->type->size ? stmt->assign.target->type->size : 8;

                if (type_is_pointer(stmt->assign.target->member.target->type)) {
                    IROperand ptr_op = ir_lower_expr(lower, stmt->assign.target->member.target);
                    uint32_t addr_vreg = ir_vreg_alloc(func);

                    ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ptr_op,
                                 ir_op_const((int64_t)field->offset, 8, false), stmt->loc);

                    IROperand addr_op = ir_op_vreg(addr_vreg, 8, false);
                    addr_op.byte_size = size;

                    ir_emit_inst(func, IR_STORE, addr_op, val, ir_op_none(), stmt->loc);
                } else if (stmt->assign.target->member.target->kind == EXPR_VAR) {
                    Symbol* sym = stmt->assign.target->member.target->var.symbol;

                    if (sym->kind == SYM_GLOBAL_VAR) {
                        uint32_t addr_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_ADDR_GLOBAL, ir_op_vreg(addr_vreg, 8, false),
                                     ir_op_global(sym->name, 8, false), ir_op_none(), stmt->loc);
                        ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ir_op_vreg(addr_vreg, 8, false),
                                     ir_op_const((int64_t)field->offset, 8, false), stmt->loc);

                        IROperand addr_op = ir_op_vreg(addr_vreg, 8, false);
                        addr_op.byte_size = size;

                        ir_emit_inst(func, IR_STORE, addr_op, val, ir_op_none(), stmt->loc);
                    } else {
                        int32_t base_off = sym->stack_offset;
                        int32_t field_off = base_off + (int32_t)field->offset;

                        ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(field_off, size, false), val, ir_op_none(), stmt->loc);
                    }
                }
            }
            break;
        }

        case STMT_COMPOUND_ASSIGN: {
            IROperand old_val = ir_lower_expr(lower, stmt->compound_assign.target);
            IROperand delta   = ir_lower_expr(lower, stmt->compound_assign.value);
            size_t size       = stmt->compound_assign.target->type->size;
            bool is_signed    = type_is_signed(stmt->compound_assign.target->type);
            uint32_t vreg     = ir_vreg_alloc(func);

            IROpcode op = IR_ADD;
            switch (stmt->compound_assign.op) {
                case TOK_PLUS_EQ:    op = IR_ADD; break;
                case TOK_MINUS_EQ:   op = IR_SUB; break;
                case TOK_STAR_EQ:    op = IR_MUL; break;
                case TOK_SLASH_EQ:   op = IR_DIV; break;
                case TOK_PERCENT_EQ: op = IR_MOD; break;
                case TOK_AMP_EQ:     op = IR_AND; break;
                case TOK_PIPE_EQ:    op = IR_OR;  break;
                case TOK_CARET_EQ:   op = IR_XOR; break;
                case TOK_SHL_EQ:     op = IR_SHL; break;
                case TOK_SHR_EQ:     op = IR_SHR; break;
                default: break;
            }

            ir_emit_inst(func, op, ir_op_vreg(vreg, size, is_signed), old_val, delta, stmt->loc);

            if (stmt->compound_assign.target->kind == EXPR_VAR) {
                if (stmt->compound_assign.target->var.symbol->kind == SYM_GLOBAL_VAR) {
                    ir_emit_inst(func, IR_STORE_GLOBAL,
                                 ir_op_global(stmt->compound_assign.target->var.name, size, is_signed),
                                 ir_op_vreg(vreg, size, is_signed), ir_op_none(), stmt->loc);
                } else {
                    int32_t offset = stmt->compound_assign.target->var.symbol->stack_offset;
                    ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(offset, size, is_signed),
                                 ir_op_vreg(vreg, size, is_signed), ir_op_none(), stmt->loc);
                }
            } else if (stmt->compound_assign.target->kind == EXPR_UNARY && stmt->compound_assign.target->unary.op == TOK_STAR) {
                IROperand ptr_op = ir_lower_expr(lower, stmt->compound_assign.target->unary.operand);
                ptr_op.byte_size = size;
                ir_emit_inst(func, IR_STORE, ptr_op, ir_op_vreg(vreg, size, is_signed), ir_op_none(), stmt->loc);
            } else if (stmt->compound_assign.target->kind == EXPR_INDEX) {
                IROperand ptr_op = ir_lower_expr(lower, stmt->compound_assign.target->index.ptr);
                IROperand idx_op = ir_lower_expr(lower, stmt->compound_assign.target->index.index);
                
                size_t elem_size = (stmt->compound_assign.target->type && stmt->compound_assign.target->type->size) ? stmt->compound_assign.target->type->size : 8;

                IROperand offset_op = idx_op;

                if (elem_size > 1) {
                    uint32_t scale_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8, false), idx_op, 
                                 ir_op_const((int64_t)elem_size, 8, false), stmt->loc);
                    offset_op = ir_op_vreg(scale_vreg, 8, false);
                }

                uint32_t addr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ptr_op, offset_op, stmt->loc);

                IROperand addr_op = ir_op_vreg(addr_vreg, 8, false);
                addr_op.byte_size = elem_size;

                ir_emit_inst(func, IR_STORE, addr_op, ir_op_vreg(vreg, size, is_signed), ir_op_none(), stmt->loc);
            } else if (stmt->compound_assign.target->kind == EXPR_MEMBER) {
                StructField* field = stmt->compound_assign.target->member.field;
                size_t f_size = stmt->compound_assign.target->type->size ? stmt->compound_assign.target->type->size : 8;

                if (type_is_pointer(stmt->compound_assign.target->member.target->type)) {
                    IROperand ptr_op = ir_lower_expr(lower, stmt->compound_assign.target->member.target);
                    uint32_t addr_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ptr_op,
                                 ir_op_const((int64_t)field->offset, 8, false), stmt->loc);

                    IROperand addr_op = ir_op_vreg(addr_vreg, 8, false);
                    addr_op.byte_size = f_size;

                    ir_emit_inst(func, IR_STORE, addr_op, ir_op_vreg(vreg, size, is_signed), ir_op_none(), stmt->loc);
                } else if (stmt->compound_assign.target->member.target->kind == EXPR_VAR) {
                    Symbol* sym = stmt->compound_assign.target->member.target->var.symbol;

                    if (sym->kind == SYM_GLOBAL_VAR) {
                        uint32_t addr_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_ADDR_GLOBAL, ir_op_vreg(addr_vreg, 8, false),
                                     ir_op_global(sym->name, 8, false), ir_op_none(), stmt->loc);
                        ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), ir_op_vreg(addr_vreg, 8, false),
                                     ir_op_const((int64_t)field->offset, 8, false), stmt->loc);

                        IROperand addr_op = ir_op_vreg(addr_vreg, 8, false);
                        addr_op.byte_size = f_size;

                        ir_emit_inst(func, IR_STORE, addr_op, ir_op_vreg(vreg, size, is_signed), ir_op_none(), stmt->loc);
                    } else {
                        int32_t base_off = sym->stack_offset;
                        int32_t field_off = base_off + (int32_t)field->offset;

                        ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(field_off, f_size, is_signed), ir_op_vreg(vreg, size, is_signed), ir_op_none(), stmt->loc);
                    }
                }
            }
            break;
        }

        case STMT_RETURN: {
            if (stmt->return_stmt.expr) {
                if (stmt->return_stmt.expr->type && stmt->return_stmt.expr->type->kind == TYPE_STRUCT) {
                    size_t size = stmt->return_stmt.expr->type->size;
                    IROperand src_addr = ir_lower_addr(lower, stmt->return_stmt.expr);

                    uint32_t sret_ptr = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_LOAD_STACK, ir_op_vreg(sret_ptr, 8, false),
                                 ir_op_stack(lower->current_sret_slot, 8, false), ir_op_none(), stmt->loc);
                    ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(sret_ptr, 8, false),
                                 src_addr, ir_op_const((int64_t)size, 8, false), stmt->loc);
                    ir_emit_inst(func, IR_RET, ir_op_vreg(sret_ptr, 8, false), ir_op_none(), ir_op_none(), stmt->loc);
                    break;
                }

                IROperand ret_val = ir_lower_expr(lower, stmt->return_stmt.expr);
                ir_emit_inst(func, IR_RET, ret_val, ir_op_none(), ir_op_none(), stmt->loc);
            } else {
                ir_emit_inst(func, IR_RET, ir_op_none(), ir_op_none(), ir_op_none(), stmt->loc);
            }
            break;
        }

        case STMT_IF: {
            IRBlock* bb_then  = ir_block_create(func, "bb_if_then");
            IRBlock* bb_else  = stmt->if_stmt.else_branch ? ir_block_create(func, "bb_if_else") : NULL;
            IRBlock* bb_merge = ir_block_create(func, "bb_if_merge");

            IROperand cond = ir_lower_expr(lower, stmt->if_stmt.cond);

            ir_emit_inst(func, IR_BR, cond, ir_op_block(bb_then), 
                         ir_op_block(bb_else ? bb_else : bb_merge), stmt->loc);

            ir_block_switch(func, bb_then);
            ir_lower_stmt(lower, stmt->if_stmt.then_branch);
            
            if (!func->current_block->is_terminated) {
                ir_emit_inst(func, IR_JMP, ir_op_block(bb_merge), ir_op_none(), ir_op_none(), stmt->loc);
            }

            if (bb_else) {
                ir_block_switch(func, bb_else);
                ir_lower_stmt(lower, stmt->if_stmt.else_branch);

                if (!func->current_block->is_terminated) {
                    ir_emit_inst(func, IR_JMP, ir_op_block(bb_merge), ir_op_none(), ir_op_none(), stmt->loc);
                }
            }

            ir_block_switch(func, bb_merge);
            break;
        }

        case STMT_BREAK: {
            if (lower->current_loop) {
                ir_emit_inst(func, IR_JMP, ir_op_block(lower->current_loop->bb_end), ir_op_none(), ir_op_none(), stmt->loc);
            }
            break;
        }

        case STMT_CONTINUE: {
            if (lower->current_loop) {
                ir_emit_inst(func, IR_JMP, ir_op_block(lower->current_loop->bb_cond), ir_op_none(), ir_op_none(), stmt->loc);
            }
            break;
        }

        case STMT_WHILE: {
            IRBlock* bb_cond = ir_block_create(func, "bb_while_cond");
            IRBlock* bb_body = ir_block_create(func, "bb_while_body");
            IRBlock* bb_end  = ir_block_create(func, "bb_while_end");

            LoopContext loop_ctx = {
                .bb_cond = bb_cond,
                .bb_end  = bb_end,
                .prev    = lower->current_loop
            };
            lower->current_loop = &loop_ctx;

            ir_emit_inst(func, IR_JMP, ir_op_block(bb_cond), ir_op_none(), ir_op_none(), stmt->loc);

            ir_block_switch(func, bb_cond);
            IROperand cond = ir_lower_expr(lower, stmt->while_stmt.cond);
            ir_emit_inst(func, IR_BR, cond, ir_op_block(bb_body), ir_op_block(bb_end), stmt->loc);

            ir_block_switch(func, bb_body);
            ir_lower_stmt(lower, stmt->while_stmt.body);
            if (!func->current_block->is_terminated) {
                ir_emit_inst(func, IR_JMP, ir_op_block(bb_cond), ir_op_none(), ir_op_none(), stmt->loc);
            }

            lower->current_loop = loop_ctx.prev;

            ir_block_switch(func, bb_end);
            break;
        }

        case STMT_EXPR: {
            ir_lower_expr(lower, stmt->expr_stmt.expr);
            break;
        }
    }
}

IRModule* ir_lower_program(Arena* arena, const AstProgram* program) {
    if (!program) {
        return NULL;
    }

    IRModule* module = ir_module_create(arena);

    for (size_t i = 0; i < program->global_count; ++i) {
        const AstGlobalVarDef* g = program->globals[i];

        IRGlobalVar* gv = ARENA_NEW_ZERO(arena, IRGlobalVar);
        gv->name     = g->name;
        gv->type     = g->type;
        gv->has_init = false;

        if (g->init_expr) {
            if (g->init_expr->kind == EXPR_INT_LIT) {
                gv->init_val = g->init_expr->int_val;
                gv->has_init = true;
            } else if (g->init_expr->kind == EXPR_UNARY && g->init_expr->unary.op == TOK_MINUS &&
                       g->init_expr->unary.operand->kind == EXPR_INT_LIT) {
                gv->init_val = -g->init_expr->unary.operand->int_val;
                gv->has_init = true;
            } else if (g->init_expr->kind == EXPR_VAR && g->init_expr->var.symbol &&
                       g->init_expr->var.symbol->kind == SYM_CONST) {
                gv->init_val = g->init_expr->var.symbol->const_val;
                gv->has_init = true;
            }
        }

        if (!module->first_global) {
            module->first_global = gv;
            module->last_global  = gv;
        } else {
            module->last_global->next = gv;
            module->last_global       = gv;
        }

        module->global_count++;
    }

    IRLower lower = {
        .arena              = arena,
        .module             = module,
        .current_func       = NULL,
        .current_loop       = NULL,
        .current_sret_slot  = 0
    };

    for (size_t i = 0; i < program->proc_count; ++i) {
        const AstProc* proc = program->procs[i];

        IRFunction* func = ir_function_create(module, proc->name, proc->return_type, proc->stack_frame_size);
        lower.current_func = func;

        bool has_sret = (proc->return_type && proc->return_type->kind == TYPE_STRUCT);
        size_t reg_param_idx = 0;
        int32_t sret_slot = 0;
        int32_t curr_stack_off = 0;

        if (has_sret) {
            curr_stack_off -= 8;
            sret_slot = curr_stack_off;
            ir_emit_inst(func, IR_PARAM, ir_op_stack(sret_slot, 8, false),
                         ir_op_const(0, 8, false), ir_op_none(), proc->loc);
            reg_param_idx = 1;
        }
        lower.current_sret_slot = sret_slot;

        for (size_t p = 0; p < proc->param_count; ++p) {
            const AstParam* param = &proc->params[p];
            size_t p_idx = reg_param_idx++;
            bool is_signed = type_is_signed(param->type);

            if (param->type && param->type->kind == TYPE_STRUCT) {
                size_t var_size = param->type->size ? param->type->size : 8;
                size_t alloc_size = (var_size + 7) & ~7;
                curr_stack_off -= (int32_t)alloc_size;
                int32_t local_struct_slot = curr_stack_off;

                uint32_t src_vreg = ir_vreg_alloc(func);
                if (p_idx < 6) {
                    int32_t tmp_ptr_slot = ir_func_alloc_stack_slot(func, 8, 8);
                    ir_emit_inst(func, IR_PARAM, ir_op_stack(tmp_ptr_slot, 8, false),
                                 ir_op_const((int64_t)p_idx, 8, false), ir_op_none(), param->loc);
                    ir_emit_inst(func, IR_LOAD_STACK, ir_op_vreg(src_vreg, 8, false),
                                 ir_op_stack(tmp_ptr_slot, 8, false), ir_op_none(), param->loc);
                } else {
                    int32_t stack_arg_off = (int32_t)(16 + (p_idx - 6) * 8);
                    ir_emit_inst(func, IR_LOAD_STACK, ir_op_vreg(src_vreg, 8, false),
                                 ir_op_stack(stack_arg_off, 8, false), ir_op_none(), param->loc);
                }

                uint32_t dst_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADDR_STACK, ir_op_vreg(dst_vreg, 8, false),
                             ir_op_stack(local_struct_slot, 8, false), ir_op_none(), param->loc);

                ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(dst_vreg, 8, false),
                             ir_op_vreg(src_vreg, 8, false),
                             ir_op_const((int64_t)param->type->size, 8, false), param->loc);
            } else {
                int32_t offset = 0;
                if (p_idx < 6) {
                    curr_stack_off -= 8;
                    offset = curr_stack_off;
                } else {
                    offset = (int32_t)(16 + (p_idx - 6) * 8);
                }
                ir_emit_inst(func, IR_PARAM, ir_op_stack(offset, param->type->size, is_signed),
                             ir_op_const((int64_t)p_idx, 8, false), ir_op_none(), param->loc);
            }
        }

        ir_lower_stmt(&lower, proc->body);

        if (!func->current_block->is_terminated) {
            ir_emit_inst(func, IR_RET, ir_op_none(), ir_op_none(), ir_op_none(), proc->loc);
        }
    }

    return module;
}

static void ir_dump_operand(IROperand op) {
    switch (op.kind) {
        case IR_OP_NONE:
            printf("<none>");
            break;

        case IR_OP_CONST:
            printf("%lld", (long long)op.int_val);
            break;

        case IR_OP_VREG:
            printf("%%v%u", op.vreg_id);
            if (op.byte_size > 0) {
                printf(":%s%zu", op.is_signed ? "i" : "u", op.byte_size * 8);
            }
            break;

        case IR_OP_STACK:
            if (op.stack_offset >= 0) {
                printf("[rbp + %d]", op.stack_offset);
            } else {
                printf("[rbp - %d]", -op.stack_offset);
            }
            if (op.byte_size > 0) {
                printf(":%s%zu", op.is_signed ? "i" : "u", op.byte_size * 8);
            }
            break;

        case IR_OP_GLOBAL:
            printf("@%.*s", (int)op.global_name.len, op.global_name.data);
            break;

        case IR_OP_STR:
            printf(".str_%u", op.str_id);
            break;

        case IR_OP_BLOCK:
            printf("@%s", op.block ? op.block->name : "<null_block>");
            break;
    }
}

static void ir_dump_escaped_string(StrView str) {
    printf("\"");
    for (size_t i = 0; i < str.len; ++i) {
        unsigned char c = (unsigned char)str.data[i];
        switch (c) {
            case '\n': printf("\\n"); break;
            case '\r': printf("\\r"); break;
            case '\t': printf("\\t"); break;
            case '\\': printf("\\\\"); break;
            case '\"': printf("\\\""); break;
            case '\0': printf("\\0"); break;
            default:
                if (c >= 32 && c <= 126) {
                    putchar(c);
                } else {
                    printf("\\x%02X", c);
                }
                break;
        }
    }
    printf("\"");
}

void ir_dump_module(const IRModule* module, Arena* arena) {
    if (!module) return;
    Arena* dump_arena = arena ? arena : module->arena;

    printf("; Functions: %zu, Globals: %zu, Strings: %zu\n\n",
           module->func_count, module->global_count, module->str_count);

    // 1. Строковые литералы
    if (module->str_count > 0) {
        printf("; --- String Constants ---\n");
        for (IRStringConst* s = module->first_str; s != NULL; s = s->next) {
            printf(".str_%u = ", s->id);
            ir_dump_escaped_string(s->value);
            printf("\n");
        }
        printf("\n");
    }

    // 2. Глобальные переменные
    if (module->global_count > 0) {
        printf("; --- Global Variables ---\n");
        for (IRGlobalVar* g = module->first_global; g != NULL; g = g->next) {
            printf("global @%.*s: %s", (int)g->name.len, g->name.data,
                   type_to_str(g->type, dump_arena));
            if (g->has_init) {
                printf(" = %lld\n", (long long)g->init_val);
            } else {
                printf(" (uninitialized, %zu bytes)\n", g->type && g->type->size ? g->type->size : 8);
            }
        }
        printf("\n");
    }

    // 3. Процедуры
    printf("; --- Functions ---\n");
    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        printf("func @%.*s() -> %s [stack_frame: %zu bytes, vregs: %u] {\n",
               (int)f->name.len, f->name.data,
               type_to_str(f->return_type, dump_arena),
               f->stack_frame_size,
               f->next_vreg_id);

        for (IRBlock* b = f->first_block; b != NULL; b = b->next_block) {
            printf("%s:\n", b->name);

            for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
                printf("    ");

                switch (inst->opcode) {
                    case IR_NOP:
                        printf("nop\n");
                        break;

                    // Работа с памятью и стеком
                    case IR_LOAD:
                        ir_dump_operand(inst->dst);
                        printf(" = load.%zu [", inst->dst.byte_size);
                        ir_dump_operand(inst->src1);
                        printf("]\n");
                        break;

                    case IR_STORE:
                        printf("store.%zu [", inst->dst.byte_size);
                        ir_dump_operand(inst->dst);
                        printf("], ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_LOAD_STACK:
                        ir_dump_operand(inst->dst);
                        printf(" = load_stack.%zu ", inst->dst.byte_size);
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_STORE_STACK:
                        printf("store_stack.%zu ", inst->dst.byte_size);
                        ir_dump_operand(inst->dst);
                        printf(", ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_ADDR_STACK:
                        ir_dump_operand(inst->dst);
                        printf(" = addr_stack ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    // Глобалы и строки
                    case IR_LOAD_GLOBAL:
                        ir_dump_operand(inst->dst);
                        printf(" = load_global.%zu ", inst->dst.byte_size);
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_STORE_GLOBAL:
                        printf("store_global.%zu ", inst->dst.byte_size);
                        ir_dump_operand(inst->dst);
                        printf(", ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_ADDR_GLOBAL:
                        ir_dump_operand(inst->dst);
                        printf(" = addr_global ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_GLOBAL_STR:
                        ir_dump_operand(inst->dst);
                        printf(" = addr_str ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_MEMCPY:
                        printf("memcpy ");
                        ir_dump_operand(inst->dst);
                        printf(", ");
                        ir_dump_operand(inst->src1);
                        printf(", size: ");
                        ir_dump_operand(inst->src2);
                        printf("\n");
                        break;

                    // Арифметика, битовые операции и сравнения
                    case IR_ADD:
                    case IR_SUB:
                    case IR_MUL:
                    case IR_DIV:
                    case IR_MOD:
                    case IR_AND:
                    case IR_OR:
                    case IR_XOR:
                    case IR_SHL:
                    case IR_SHR:
                    case IR_CMP_EQ:
                    case IR_CMP_NE:
                    case IR_CMP_LT:
                    case IR_CMP_LE:
                    case IR_CMP_GT:
                    case IR_CMP_GE: {
                        const char* op_name = "unknown";
                        switch (inst->opcode) {
                            case IR_ADD:    op_name = "add"; break;
                            case IR_SUB:    op_name = "sub"; break;
                            case IR_MUL:    op_name = "mul"; break;
                            case IR_DIV:    op_name = "div"; break;
                            case IR_MOD:    op_name = "mod"; break;
                            case IR_AND:    op_name = "and"; break;
                            case IR_OR:     op_name = "or";  break;
                            case IR_XOR:    op_name = "xor"; break;
                            case IR_SHL:    op_name = "shl"; break;
                            case IR_SHR:    op_name = "shr"; break;
                            case IR_CMP_EQ: op_name = "cmp_eq"; break;
                            case IR_CMP_NE: op_name = "cmp_ne"; break;
                            case IR_CMP_LT: op_name = "cmp_lt"; break;
                            case IR_CMP_LE: op_name = "cmp_le"; break;
                            case IR_CMP_GT: op_name = "cmp_gt"; break;
                            case IR_CMP_GE: op_name = "cmp_ge"; break;
                            default: break;
                        }

                        ir_dump_operand(inst->dst);
                        printf(" = %s ", op_name);
                        ir_dump_operand(inst->src1);
                        printf(", ");
                        ir_dump_operand(inst->src2);
                        printf("\n");
                        break;
                    }

                    // Унарные операции
                    case IR_NEG:
                        ir_dump_operand(inst->dst);
                        printf(" = neg ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_NOT:
                        ir_dump_operand(inst->dst);
                        printf(" = not ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    // Управление потоком (Control Flow)
                    case IR_JMP:
                        printf("jmp ");
                        ir_dump_operand(inst->dst);
                        printf("\n");
                        break;

                    case IR_BR:
                        printf("br ");
                        ir_dump_operand(inst->dst);
                        printf(", then: ");
                        ir_dump_operand(inst->src1);
                        printf(", else: ");
                        ir_dump_operand(inst->src2);
                        printf("\n");
                        break;

                    case IR_RET:
                        printf("ret");
                        if (inst->dst.kind != IR_OP_NONE) {
                            printf(" ");
                            ir_dump_operand(inst->dst);
                        }
                        printf("\n");
                        break;

                    case IR_CALL:
                        if (inst->dst.kind != IR_OP_NONE) {
                            ir_dump_operand(inst->dst);
                            printf(" = ");
                        }
                        printf("call @%.*s(", (int)inst->symbol_name.len, inst->symbol_name.data);
                        for (size_t i = 0; i < inst->extra_arg_count; ++i) {
                            ir_dump_operand(inst->extra_args[i]);
                            if (i + 1 < inst->extra_arg_count) printf(", ");
                        }
                        printf(")\n");
                        break;

                    case IR_PARAM:
                        printf("param [arg%lld] -> ", (long long)inst->src1.int_val);
                        ir_dump_operand(inst->dst);
                        printf("\n");
                        break;

                    case IR_INLINE_ASM:
                        if (inst->dst.kind != IR_OP_NONE) {
                            ir_dump_operand(inst->dst);
                            printf(" = ");
                        }
                        printf("asm \"%.*s\"\n", (int)inst->symbol_name.len, inst->symbol_name.data);
                        break;
                }
            }
        }

        printf("}\n\n");
    }
}