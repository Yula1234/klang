#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

IROperand ir_op_none(void) {
    return (IROperand){ .kind = IR_OP_NONE, .byte_size = 0, .int_val = 0 };
}

IROperand ir_op_const(int64_t val, size_t byte_size) {
    return (IROperand){
        .kind      = IR_OP_CONST,
        .byte_size = (byte_size == 0) ? 8 : byte_size,
        .int_val   = val
    };
}

IROperand ir_op_vreg(uint32_t vreg_id, size_t byte_size) {
    return (IROperand){
        .kind      = IR_OP_VREG,
        .byte_size = (byte_size == 0) ? 8 : byte_size,
        .vreg_id   = vreg_id
    };
}

IROperand ir_op_stack(int32_t stack_offset, size_t byte_size) {
    return (IROperand){
        .kind         = IR_OP_STACK,
        .byte_size    = (byte_size == 0) ? 8 : byte_size,
        .stack_offset = stack_offset
    };
}

IROperand ir_op_str(uint32_t str_id) {
    return (IROperand){
        .kind      = IR_OP_STR,
        .byte_size = 8,
        .str_id    = str_id
    };
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
} IRLower;

static IROperand ir_lower_expr(IRLower* lower, const AstExpr* expr);

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
            return ir_op_const(expr->int_val, expr_size);
        }

        case EXPR_STRING_LIT: {
            uint32_t str_id = register_string_literal(lower, expr->string_val);
            uint32_t vreg   = ir_vreg_alloc(func);

            ir_emit_inst(func, IR_GLOBAL_STR, ir_op_vreg(vreg, 8), ir_op_str(str_id), ir_op_none(), expr->loc);

            return ir_op_vreg(vreg, 8);
        }

        case EXPR_VAR: {
            assert(expr->var.symbol != NULL);

            uint32_t vreg = ir_vreg_alloc(func);
            int32_t offset = expr->var.symbol->stack_offset;

            ir_emit_inst(func, IR_LOAD_STACK, ir_op_vreg(vreg, expr_size), 
                         ir_op_stack(offset, expr_size), ir_op_none(), expr->loc);

            return ir_op_vreg(vreg, expr_size);
        }

        case EXPR_UNARY: {
            if (expr->unary.op == TOK_STAR) {
                IROperand ptr_op = ir_lower_expr(lower, expr->unary.operand);
                uint32_t vreg = ir_vreg_alloc(func);

                ir_emit_inst(func, IR_LOAD, ir_op_vreg(vreg, expr_size), ptr_op, ir_op_none(), expr->loc);

                return ir_op_vreg(vreg, expr_size);
            }

            if (expr->unary.op == TOK_MINUS) {
                IROperand inner_op = ir_lower_expr(lower, expr->unary.operand);
                uint32_t vreg = ir_vreg_alloc(func);

                ir_emit_inst(func, IR_NEG, ir_op_vreg(vreg, expr_size), inner_op, ir_op_none(), expr->loc);

                return ir_op_vreg(vreg, expr_size);
            }

            if (expr->unary.op == TOK_TILDE) {
                IROperand inner_op = ir_lower_expr(lower, expr->unary.operand);
                uint32_t vreg = ir_vreg_alloc(func);

                ir_emit_inst(func, IR_NOT, ir_op_vreg(vreg, expr_size), inner_op, ir_op_none(), expr->loc);

                return ir_op_vreg(vreg, expr_size);
            }

            if (expr->unary.op == TOK_BANG) {
                IROperand inner_op = ir_lower_expr(lower, expr->unary.operand);
                uint32_t vreg = ir_vreg_alloc(func);

                ir_emit_inst(func, IR_CMP_EQ, ir_op_vreg(vreg, 1), inner_op, ir_op_const(0, inner_op.byte_size), expr->loc);

                return ir_op_vreg(vreg, 1);
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
                    ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(tmp_slot, 1), ir_op_const(0, 1), ir_op_none(), expr->loc);
                    ir_emit_inst(func, IR_BR, lhs, ir_op_block(bb_rhs), ir_op_block(bb_merge), expr->loc);

                    ir_block_switch(func, bb_rhs);
                    IROperand rhs = ir_lower_expr(lower, expr->binary.rhs);
                    uint32_t bool_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_CMP_NE, ir_op_vreg(bool_vreg, 1), rhs, ir_op_const(0, rhs.byte_size), expr->loc);
                    ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(tmp_slot, 1), ir_op_vreg(bool_vreg, 1), ir_op_none(), expr->loc);
                    if (!bb_rhs->is_terminated) {
                        ir_emit_inst(func, IR_JMP, ir_op_block(bb_merge), ir_op_none(), ir_op_none(), expr->loc);
                    }
                } else {
                    ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(tmp_slot, 1), ir_op_const(1, 1), ir_op_none(), expr->loc);
                    ir_emit_inst(func, IR_BR, lhs, ir_op_block(bb_merge), ir_op_block(bb_rhs), expr->loc);

                    ir_block_switch(func, bb_rhs);
                    IROperand rhs = ir_lower_expr(lower, expr->binary.rhs);
                    uint32_t bool_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_CMP_NE, ir_op_vreg(bool_vreg, 1), rhs, ir_op_const(0, rhs.byte_size), expr->loc);
                    ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(tmp_slot, 1), ir_op_vreg(bool_vreg, 1), ir_op_none(), expr->loc);
                    if (!bb_rhs->is_terminated) {
                        ir_emit_inst(func, IR_JMP, ir_op_block(bb_merge), ir_op_none(), ir_op_none(), expr->loc);
                    }
                }

                ir_block_switch(func, bb_merge);
                ir_emit_inst(func, IR_LOAD_STACK, ir_op_vreg(res_vreg, 1), ir_op_stack(tmp_slot, 1), ir_op_none(), expr->loc);
                return ir_op_vreg(res_vreg, 1);
            }

            IROperand lhs = ir_lower_expr(lower, expr->binary.lhs);
            IROperand rhs = ir_lower_expr(lower, expr->binary.rhs);
            uint32_t vreg = ir_vreg_alloc(func);

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

            ir_emit_inst(func, op, ir_op_vreg(vreg, expr_size), lhs, rhs, expr->loc);

            return ir_op_vreg(vreg, expr_size);
        }

        case EXPR_CAST: {
            IROperand inner_op = ir_lower_expr(lower, expr->cast.expr);
            uint32_t vreg = ir_vreg_alloc(func);

            ir_emit_inst(func, IR_ADD, ir_op_vreg(vreg, expr_size), inner_op, ir_op_const(0, expr_size), expr->loc);

            return ir_op_vreg(vreg, expr_size);
        }

        case EXPR_INDEX: {
            IROperand ptr_op = ir_lower_expr(lower, expr->index.ptr);
            IROperand idx_op = ir_lower_expr(lower, expr->index.index);
            size_t elem_size = (expr->type && expr->type->size) ? expr->type->size : 8;

            IROperand offset_op = idx_op;

            if (elem_size > 1) {
                uint32_t scale_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8), idx_op,
                             ir_op_const((int64_t)elem_size, 8), expr->loc);
                offset_op = ir_op_vreg(scale_vreg, 8);
            }

            uint32_t addr_vreg = ir_vreg_alloc(func);
            ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8), ptr_op, offset_op, expr->loc);

            uint32_t val_vreg = ir_vreg_alloc(func);
            ir_emit_inst(func, IR_LOAD, ir_op_vreg(val_vreg, elem_size),
                         ir_op_vreg(addr_vreg, 8), ir_op_none(), expr->loc);

            return ir_op_vreg(val_vreg, elem_size);
        }

        case EXPR_ASM: {
            IROperand dst = ir_op_none();

            if (expr->type && expr->type->kind != TYPE_VOID) {
                size_t size = expr->type->size ? expr->type->size : 8;
                uint32_t vreg = ir_vreg_alloc(func);
                dst = ir_op_vreg(vreg, size);
            }

            IRInst* inst = ir_emit_inst(func, IR_INLINE_ASM, dst, ir_op_none(), ir_op_none(), expr->loc);
            inst->symbol_name = expr->inline_asm.code;

            return dst;
        }

        case EXPR_CALL: {
            size_t argc = expr->call.arg_count;
            IROperand* args = ARENA_NEW_ARRAY(lower->arena, IROperand, argc);

            for (size_t i = 0; i < argc; ++i) {
                args[i] = ir_lower_expr(lower, expr->call.args[i]);
            }

            uint32_t vreg = ir_vreg_alloc(func);
            IROperand dst = (expr->type && expr->type->kind != TYPE_VOID) ? ir_op_vreg(vreg, expr_size) : ir_op_none();

            IRInst* call_inst = ir_emit_inst(func, IR_CALL, dst, ir_op_none(), ir_op_none(), expr->loc);

            call_inst->symbol_name     = expr->call.callee_name;
            call_inst->extra_args       = args;
            call_inst->extra_arg_count  = argc;

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
            IROperand init_val = ir_lower_expr(lower, stmt->var_decl.init_expr);
            int32_t offset = stmt->var_decl.symbol->stack_offset;
            size_t size = stmt->var_decl.symbol->type->size;

            ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(offset, size), init_val, ir_op_none(), stmt->loc);
            break;
        }

        case STMT_ASSIGN: {
            IROperand val = ir_lower_expr(lower, stmt->assign.value);

            if (stmt->assign.target->kind == EXPR_VAR) {
                int32_t offset = stmt->assign.target->var.symbol->stack_offset;
                size_t size = stmt->assign.target->type->size;

                ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(offset, size), val, ir_op_none(), stmt->loc);
            } else if (stmt->assign.target->kind == EXPR_UNARY && stmt->assign.target->unary.op == TOK_STAR) {
                IROperand ptr_op = ir_lower_expr(lower, stmt->assign.target->unary.operand);
                size_t size = stmt->assign.target->type->size;
                ptr_op.byte_size = size;

                ir_emit_inst(func, IR_STORE, ptr_op, val, ir_op_none(), stmt->loc);
            } else if (stmt->assign.target->kind == EXPR_INDEX) {
                // Присваивание в элемент массива: ptr[i] = val;
                IROperand ptr_op = ir_lower_expr(lower, stmt->assign.target->index.ptr);
                IROperand idx_op = ir_lower_expr(lower, stmt->assign.target->index.index);
                size_t elem_size = (stmt->assign.target->type && stmt->assign.target->type->size) ? stmt->assign.target->type->size : 8;

                IROperand offset_op = idx_op;

                if (elem_size > 1) {
                    uint32_t scale_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8), idx_op, 
                                 ir_op_const((int64_t)elem_size, 8), stmt->loc);
                    offset_op = ir_op_vreg(scale_vreg, 8);
                }

                uint32_t addr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8), ptr_op, offset_op, stmt->loc);

                IROperand addr_op = ir_op_vreg(addr_vreg, 8);
                addr_op.byte_size = elem_size;

                ir_emit_inst(func, IR_STORE, addr_op, val, ir_op_none(), stmt->loc);
            }
            break;
        }

        case STMT_COMPOUND_ASSIGN: {
            IROperand old_val = ir_lower_expr(lower, stmt->compound_assign.target);
            IROperand delta   = ir_lower_expr(lower, stmt->compound_assign.value);
            size_t size       = stmt->compound_assign.target->type->size;
            uint32_t vreg     = ir_vreg_alloc(func);

            IROpcode op = IR_ADD;
            switch (stmt->compound_assign.op) {
                case TOK_PLUS_EQ:  op = IR_ADD; break;
                case TOK_MINUS_EQ: op = IR_SUB; break;
                case TOK_AMP_EQ:   op = IR_AND; break;
                case TOK_PIPE_EQ:  op = IR_OR;  break;
                case TOK_CARET_EQ: op = IR_XOR; break;
                case TOK_SHL_EQ:   op = IR_SHL; break;
                case TOK_SHR_EQ:   op = IR_SHR; break;
                default: break;
            }

            ir_emit_inst(func, op, ir_op_vreg(vreg, size), old_val, delta, stmt->loc);

            if (stmt->compound_assign.target->kind == EXPR_VAR) {
                int32_t offset = stmt->compound_assign.target->var.symbol->stack_offset;
                ir_emit_inst(func, IR_STORE_STACK, ir_op_stack(offset, size), ir_op_vreg(vreg, size), ir_op_none(), stmt->loc);
            } else if (stmt->compound_assign.target->kind == EXPR_UNARY && stmt->compound_assign.target->unary.op == TOK_STAR) {
                IROperand ptr_op = ir_lower_expr(lower, stmt->compound_assign.target->unary.operand);
                ptr_op.byte_size = size;
                ir_emit_inst(func, IR_STORE, ptr_op, ir_op_vreg(vreg, size), ir_op_none(), stmt->loc);
            } else if (stmt->compound_assign.target->kind == EXPR_INDEX) {
                IROperand ptr_op = ir_lower_expr(lower, stmt->compound_assign.target->index.ptr);
                IROperand idx_op = ir_lower_expr(lower, stmt->compound_assign.target->index.index);
                
                size_t elem_size = (stmt->compound_assign.target->type && stmt->compound_assign.target->type->size) ? stmt->compound_assign.target->type->size : 8;

                IROperand offset_op = idx_op;

                if (elem_size > 1) {
                    uint32_t scale_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8), idx_op, 
                                 ir_op_const((int64_t)elem_size, 8), stmt->loc);
                    offset_op = ir_op_vreg(scale_vreg, 8);
                }

                uint32_t addr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8), ptr_op, offset_op, stmt->loc);

                IROperand addr_op = ir_op_vreg(addr_vreg, 8);
                addr_op.byte_size = elem_size;

                ir_emit_inst(func, IR_STORE, addr_op, ir_op_vreg(vreg, size), ir_op_none(), stmt->loc);
            }
            break;
        }

        case STMT_RETURN: {
            IROperand ret_val = ir_op_none();

            if (stmt->return_stmt.expr) {
                ret_val = ir_lower_expr(lower, stmt->return_stmt.expr);
            }

            ir_emit_inst(func, IR_RET, ret_val, ir_op_none(), ir_op_none(), stmt->loc);
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

    IRLower lower = {
        .arena         = arena,
        .module        = module,
        .current_func  = NULL,
        .current_loop  = NULL
    };

    for (size_t i = 0; i < program->proc_count; ++i) {
        const AstProc* proc = program->procs[i];

        IRFunction* func = ir_function_create(module, proc->name, proc->return_type, proc->stack_frame_size);
        lower.current_func = func;

        for (size_t p = 0; p < proc->param_count; ++p) {
            const AstParam* param = &proc->params[p];
            int32_t offset = (int32_t)(-(int32_t)((p + 1) * 8));

            ir_emit_inst(func, IR_PARAM, ir_op_stack(offset, param->type->size), 
                         ir_op_const((int64_t)p, 8), ir_op_none(), param->loc);
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
        case IR_OP_NONE:  printf("<none>"); break;
        case IR_OP_CONST: printf("%lld", (long long)op.int_val); break;
        case IR_OP_VREG:  printf("%%v%u", op.vreg_id); break;
        case IR_OP_STACK: printf("[rbp %d]", op.stack_offset); break;
        case IR_OP_STR:   printf(".str_%u", op.str_id); break;
        case IR_OP_BLOCK: printf("%s", op.block ? op.block->name : "<null_block>"); break;
    }
}

void ir_dump_module(const IRModule* module, Arena* arena) {
    (void)arena;
    if (!module) return;

    printf("=== IR MODULE DUMP (%zu functions, %zu strings) ===\n\n", 
           module->func_count, module->str_count);

    for (IRStringConst* s = module->first_str; s != NULL; s = s->next) {
        printf(".str_%u: db \"%.*s\", 0\n", s->id, (int)s->value.len, s->value.data);
    }

    if (module->str_count > 0) {
        printf("\n");
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        printf("func %.*s() -> %s  [stack: %zu bytes] {\n", 
               (int)f->name.len, f->name.data,
               type_to_str(f->return_type, NULL),
               f->stack_frame_size);

        for (IRBlock* b = f->first_block; b != NULL; b = b->next_block) {
            printf("%s:\n", b->name);

            for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
                printf("    ");

                switch (inst->opcode) {
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

                    case IR_GLOBAL_STR:
                        ir_dump_operand(inst->dst);
                        printf(" = addr ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_MUL:
                    case IR_DIV:
                    case IR_CMP_EQ:
                    case IR_CMP_NE:
                    case IR_CMP_LT:
                    case IR_AND:
                    case IR_OR:
                    case IR_XOR:
                    case IR_SHL:
                    case IR_SHR:
                    case IR_ADD:
                    case IR_SUB:
                    case IR_CMP_GT: {
                        const char* op_name = "add";
                        if (inst->opcode == IR_SUB)    op_name = "sub";
                        if (inst->opcode == IR_MUL)    op_name = "mul";
                        if (inst->opcode == IR_DIV)    op_name = "div";
                        if (inst->opcode == IR_MOD)    op_name = "mod";
                        if (inst->opcode == IR_AND)    op_name = "and";
                        if (inst->opcode == IR_OR)     op_name = "or"; 
                        if (inst->opcode == IR_XOR)    op_name = "xor";
                        if (inst->opcode == IR_SHL)    op_name = "shl";
                        if (inst->opcode == IR_SHR)    op_name = "shr";
                        if (inst->opcode == IR_CMP_EQ) op_name = "cmp_eq";
                        if (inst->opcode == IR_CMP_NE) op_name = "cmp_ne";
                        if (inst->opcode == IR_CMP_LT) op_name = "cmp_lt";
                        if (inst->opcode == IR_CMP_GT) op_name = "cmp_gt";

                        ir_dump_operand(inst->dst);
                        printf(" = %s ", op_name);
                        ir_dump_operand(inst->src1);
                        printf(", ");
                        ir_dump_operand(inst->src2);
                        printf("\n");
                        break;
                    }

                    case IR_NEG:
                        ir_dump_operand(inst->dst);
                        printf(" = neg ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_JMP:
                        printf("jmp ");
                        ir_dump_operand(inst->dst);
                        printf("\n");
                        break;

                    case IR_BR:
                        printf("br ");
                        ir_dump_operand(inst->dst);
                        printf(", ");
                        ir_dump_operand(inst->src1);
                        printf(", ");
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
                        printf("call %.*s(", (int)inst->symbol_name.len, inst->symbol_name.data);
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

                    default:
                        break;
                }
            }
        }

        printf("}\n\n");
    }

    printf("====================================================\n");
}