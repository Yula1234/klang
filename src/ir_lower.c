#include "ir.h"
#include "regalloc.h"
#include "eval.h"
#include "abi.h"
#include "x86.h"

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

IROperand ir_op_reg(X86Reg reg, size_t byte_size, bool is_signed) {
    return (IROperand){
        .kind      = IR_OP_REG,
        .byte_size = (byte_size == 0) ? 8 : byte_size,
        .is_signed = is_signed,
        .reg       = (uint32_t)reg
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

IROperand ir_op_block(IRBlock* block) {
    return (IROperand){
        .kind      = IR_OP_BLOCK,
        .byte_size = 8,
        .block     = block
    };
}

int32_t ir_func_alloc_stack_slot(IRFunction* func, size_t size, size_t align) {
    if (align == 0) {
        align = 8;
    }

    if (size == 0) {
        size = 1;
    }

    size_t aligned_size = (size + align - 1) & ~(align - 1);
    func->stack_frame_size = (func->stack_frame_size + aligned_size + (align - 1)) & ~(align - 1);

    int32_t offset = -(int32_t)func->stack_frame_size;

    IRStackSlot slot;
    slot.id         = (uint32_t)func->stack_slot_count;
    slot.old_offset = offset;
    slot.size       = size;
    slot.align      = align;
    slot.is_spill   = false;

    ARENA_DA_PUSH(func->arena, func->stack_slots, func->stack_slot_count, func->stack_slot_cap, slot);

    return offset;
}

IRModule* ir_module_create(Arena* arena) {
    IRModule* module = ARENA_NEW_ZERO(arena, IRModule);

    module->arena        = arena;
    module->first_func   = NULL;
    module->last_func    = NULL;
    module->func_count   = 0;
    module->first_str    = NULL;
    module->last_str     = NULL;
    module->str_count    = 0;
    module->first_global = NULL;
    module->last_global  = NULL;
    module->global_count = 0;

    return module;
}

IRFunction* ir_function_create(IRModule* module, StrView name, Type* return_type) {
    IRFunction* func = ARENA_NEW_ZERO(module->arena, IRFunction);

    func->arena            = module->arena;
    func->name             = name;
    func->return_type      = return_type;
    func->stack_frame_size = 0;
    func->next_vreg_id     = 0;
    func->next_block_id    = 0;
    func->block_count      = 0;

    IRBlock* entry = ir_block_create(func, "bb_entry");
    ir_block_switch(func, entry);
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
    IRBlock* block = ARENA_NEW_ZERO(func->arena, IRBlock);

    block->name          = prefix;
    block->id            = 0;
    block->first_inst    = NULL;
    block->last_inst     = NULL;
    block->inst_count    = 0;
    block->is_terminated = false;
    block->is_placed     = false;
    block->next_block    = NULL;

    return block;
}

void ir_block_switch(IRFunction* func, IRBlock* block) {
    assert(block != NULL);

    if (!block->is_placed) {
        block->is_placed = true;
        block->id        = func->next_block_id++;
        block->name      = arena_sprintf(func->arena, "%s_%u", block->name, block->id);

        if (!func->first_block) {
            func->first_block = block;
            func->last_block  = block;
        } else {
            func->last_block->next_block = block;
            func->last_block             = block;
        }

        func->block_count++;
    }

    func->current_block = block;
}

uint32_t ir_vreg_alloc(IRFunction* func) {
    return func->next_vreg_id++;
}

IRInst* ir_emit_inst(IRFunction* func, IROpcode op, IROperand dst, IROperand src1, IROperand src2, SourceLoc loc) {
    IRBlock* block = func->current_block;

    if (block->is_terminated) {
        return NULL;
    }

    IRInst* inst = ARENA_NEW_ZERO(func->arena, IRInst);

    inst->opcode          = op;
    inst->dst             = dst;
    inst->src1            = src1;
    inst->src2            = src2;
    inst->loc             = loc;
    inst->next            = NULL;
    inst->extra_args      = NULL;
    inst->extra_arg_count = 0;
    inst->mem_index       = REG_NONE;
    inst->mem_scale       = 0;

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

typedef struct SymbolSlot {
    const Symbol*      symbol;
    int32_t            offset;
    struct SymbolSlot* next;
} SymbolSlot;

typedef struct LoopContext {
    IRBlock*            bb_cond;
    IRBlock*            bb_end;
    struct LoopContext* prev;
} LoopContext;

typedef struct DeferEntry {
    const AstStmt*     stmt;
    struct DeferEntry* next;
} DeferEntry;

typedef struct DeferScope {
    DeferEntry*        entries;
    bool               is_loop;
    struct DeferScope* parent;
} DeferScope;

typedef struct InlineContext {
    const AstProc*        proc;
    IRBlock*              bb_return;
    IROperand             ret_val_op;
    int32_t               ret_sret_slot;
    bool                  has_return_val;
    const DeferScope*     boundary_defer_scope;
    struct InlineContext* prev;
} InlineContext;

typedef struct IRLower {
    Arena*         arena;
    IRModule*      module;
    IRFunction*    current_func;
    LoopContext*   current_loop;
    SymbolSlot*    symbol_slots;
    int32_t        current_sret_slot;
    DeferScope*    current_defer_scope;
    InlineContext* current_inline;
} IRLower;

static void symbol_slot_bind(IRLower* lower, const Symbol* sym, int32_t offset) {
    assert(sym != NULL && "Cannot bind NULL symbol to stack slot");
    
    SymbolSlot* slot = ARENA_NEW_ZERO(lower->arena, SymbolSlot);
    slot->symbol = sym;
    slot->offset = offset;
    slot->next   = lower->symbol_slots;

    lower->symbol_slots = slot;
}

static int32_t symbol_slot_lookup(const IRLower* lower, const Symbol* sym) {
    assert(sym != NULL);
    for (SymbolSlot* s = lower->symbol_slots; s != NULL; s = s->next) {
        if (s->symbol == sym) {
            return s->offset;
        }
    }
    return 0;
}

static void defer_scope_push(IRLower* lower, bool is_loop) {
    DeferScope* scope = ARENA_NEW_ZERO(lower->arena, DeferScope);

    scope->entries = NULL;
    scope->is_loop = is_loop;
    scope->parent  = lower->current_defer_scope;

    lower->current_defer_scope = scope;
}

static void defer_scope_pop(IRLower* lower) {
    if (lower->current_defer_scope) {
        lower->current_defer_scope = lower->current_defer_scope->parent;
    }
}

static void defer_register(IRLower* lower, const AstStmt* stmt) {
    if (!lower->current_defer_scope) {
        return;
    }

    DeferEntry* entry = ARENA_NEW_ZERO(lower->arena, DeferEntry);

    entry->stmt = stmt;
    entry->next = lower->current_defer_scope->entries;

    lower->current_defer_scope->entries = entry;
}

static void      ir_lower_stmt(IRLower* lower, const AstStmt* stmt);
static IROperand ir_lower_expr(IRLower* lower, const AstExpr* expr);
static IROperand ir_lower_addr(IRLower* lower, const AstExpr* expr);
static void      ir_lower_cond(IRLower* lower, const AstExpr* expr, IRBlock* bb_true, IRBlock* bb_false);

static void emit_defers_in_scope(IRLower* lower, const DeferScope* scope) {
    if (!scope) {
        return;
    }

    for (const DeferEntry* e = scope->entries; e != NULL; e = e->next) {
        ir_lower_stmt(lower, e->stmt);
    }
}

static void emit_defers_up_to_scope(IRLower* lower, const DeferScope* boundary) {
    for (const DeferScope* s = lower->current_defer_scope; s != NULL && s != boundary; s = s->parent) {
        emit_defers_in_scope(lower, s);
    }
}

static void emit_defers_up_to_loop(IRLower* lower) {
    for (const DeferScope* s = lower->current_defer_scope; s != NULL; s = s->parent) {
        emit_defers_in_scope(lower, s);

        if (s->is_loop) {
            break;
        }
    }
}

static void emit_defers_all(IRLower* lower) {
    for (const DeferScope* s = lower->current_defer_scope; s != NULL; s = s->parent) {
        emit_defers_in_scope(lower, s);
    }
}

static bool ir_try_inline_call(IRLower* lower, const AstProc* callee, AstExpr** args, size_t arg_count, IROperand* out_res, SourceLoc loc) {
    if (!callee || !callee->attrs.is_inlined || callee->attrs.is_extern || !callee->body) {
        return false;
    }

    size_t depth = 0;
    for (const InlineContext* ctx = lower->current_inline; ctx != NULL; ctx = ctx->prev) {
        if (ctx->proc == callee || depth >= 16) {
            return false;
        }
        depth++;
    }

    IRFunction* func = lower->current_func;

    IROperand* evaluated_args = NULL;
    if (arg_count > 0) {
        evaluated_args = ARENA_NEW_ARRAY(lower->arena, IROperand, arg_count);

        for (size_t i = 0; i < arg_count; ++i) {
            if (type_is_compound(args[i]->type)) {
                IROperand addr = ir_lower_addr(lower, args[i]);

                if (addr.kind == IR_OP_STACK || addr.kind == IR_OP_GLOBAL) {
                    uint32_t vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(vreg, 8, false), addr, ir_op_none(), args[i]->loc);
                    evaluated_args[i] = ir_op_vreg(vreg, 8, false);
                } else {
                    evaluated_args[i] = addr;
                }
            } else {
                evaluated_args[i] = ir_lower_expr(lower, args[i]);
            }
        }
    }

    bool ret_is_sret = type_requires_sret(callee->return_type);
    bool ret_is_void = (!callee->return_type || callee->return_type->kind == TYPE_VOID);
    int32_t ret_slot = 0;
    IROperand result_op = ir_op_none();

    if (ret_is_sret) {
        size_t ret_size  = callee->return_type->size ? callee->return_type->size : 8;
        size_t ret_align = callee->return_type->align ? callee->return_type->align : 8;
        ret_slot = ir_func_alloc_stack_slot(func, ret_size, ret_align);
        result_op = ir_op_stack(ret_slot, ret_size, false);
    } else if (!ret_is_void) {
        size_t ret_size = callee->return_type->size ? callee->return_type->size : 8;
        bool is_signed = type_is_signed(callee->return_type);
        uint32_t ret_vreg = ir_vreg_alloc(func);
        result_op = ir_op_vreg(ret_vreg, ret_size, is_signed);
    }

    SymbolSlot*  saved_symbol_slots   = lower->symbol_slots;
    LoopContext* saved_loop           = lower->current_loop;
    int32_t      saved_sret_slot      = lower->current_sret_slot;
    DeferScope*  saved_defer_scope    = lower->current_defer_scope;

    IRBlock* bb_inline_ret = ir_block_create(func, "bb_inline_ret");

    InlineContext inline_ctx = {
        .proc                 = callee,
        .bb_return            = bb_inline_ret,
        .ret_val_op           = result_op,
        .ret_sret_slot        = ret_slot,
        .has_return_val       = !ret_is_void,
        .boundary_defer_scope = saved_defer_scope,
        .prev                 = lower->current_inline
    };

    lower->current_inline = &inline_ctx;
    lower->current_loop   = NULL;

    defer_scope_push(lower, false);

    for (size_t p = 0; p < callee->param_count; ++p) {
        const AstParam* param = &callee->params[p];
        size_t var_size       = (param->type && param->type->size) ? param->type->size : 8;
        size_t var_align      = (param->type && param->type->align) ? param->type->align : 8;
        bool is_signed        = type_is_signed(param->type);

        int32_t slot = ir_func_alloc_stack_slot(func, var_size, var_align);
        symbol_slot_bind(lower, param->symbol, slot);

        if (type_is_compound(param->type)) {
            uint32_t dst_addr = ir_vreg_alloc(func);
            ir_emit_inst(func, IR_ADDR, ir_op_vreg(dst_addr, 8, false),
                         ir_op_stack(slot, var_size, false), ir_op_none(), loc);

            ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(dst_addr, 8, false),
                         evaluated_args[p], ir_op_const((int64_t)var_size, 8, false), loc);
        } else {
            ir_emit_inst(func, IR_MOV, ir_op_stack(slot, var_size, is_signed),
                         evaluated_args[p], ir_op_none(), loc);
        }
    }

    ir_lower_stmt(lower, callee->body);

    if (!func->current_block->is_terminated) {
        emit_defers_up_to_scope(lower, inline_ctx.boundary_defer_scope);
        ir_emit_inst(func, IR_JMP, ir_op_block(bb_inline_ret), ir_op_none(), ir_op_none(), loc);
    }

    defer_scope_pop(lower);

    ir_block_switch(func, bb_inline_ret);

    lower->symbol_slots        = saved_symbol_slots;
    lower->current_loop        = saved_loop;
    lower->current_sret_slot   = saved_sret_slot;
    lower->current_defer_scope = saved_defer_scope;
    lower->current_inline      = inline_ctx.prev;

    *out_res = result_op;

    return true;
}

static void ir_lower_struct_lit_into(IRLower* lower, const AstExpr* expr, IROperand dest_addr) {
    if (!expr || expr->kind != EXPR_STRUCT_LIT) {
        return;
    }

    IRFunction* func = lower->current_func;
    Type* st = expr->struct_lit.struct_type;

    if (!st) {
        return;
    }

    if (dest_addr.kind == IR_OP_GLOBAL) {
        uint32_t base_vreg = ir_vreg_alloc(func);
        ir_emit_inst(func, IR_ADDR, ir_op_vreg(base_vreg, 8, false), dest_addr, ir_op_none(), expr->loc);
        dest_addr = ir_op_vreg(base_vreg, 8, false);
    }

    for (size_t f_idx = 0; f_idx < st->structure.field_count; ++f_idx) {
        StructField* f = &st->structure.fields[f_idx];
        size_t f_size  = (f->type && f->type->size) ? f->type->size : 8;
        bool is_signed = type_is_signed(f->type);

        const AstExpr* val_expr = NULL;

        for (size_t i = 0; i < expr->struct_lit.field_count; ++i) {
            if (strview_equals(expr->struct_lit.field_names[i], f->name)) {
                val_expr = expr->struct_lit.field_values[i];
                break;
            }
        }

        if (!val_expr && f->default_value) {
            val_expr = f->default_value;
        }

        if (dest_addr.kind == IR_OP_STACK) {
            int32_t f_offset = dest_addr.stack_offset + (int32_t)f->offset;

            if (val_expr) {
                if (f->type && type_is_compound(f->type)) {
                    if (val_expr->kind == EXPR_STRUCT_LIT) {
                        ir_lower_struct_lit_into(lower, val_expr, ir_op_stack(f_offset, f_size, false));
                    } else {
                        uint32_t dst_field_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_ADDR, ir_op_vreg(dst_field_vreg, 8, false),
                                     ir_op_stack(f_offset, f_size, false), ir_op_none(), expr->loc);

                        IROperand src_field_addr = ir_lower_addr(lower, val_expr);

                        if (src_field_addr.kind == IR_OP_STACK || src_field_addr.kind == IR_OP_GLOBAL) {
                            uint32_t src_field_vreg = ir_vreg_alloc(func);
                            ir_emit_inst(func, IR_ADDR, ir_op_vreg(src_field_vreg, 8, false),
                                         src_field_addr, ir_op_none(), expr->loc);
                            src_field_addr = ir_op_vreg(src_field_vreg, 8, false);
                        }

                        ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(dst_field_vreg, 8, false),
                                     src_field_addr, ir_op_const((int64_t)f_size, 8, false), expr->loc);
                    }
                } else {
                    IROperand val = ir_lower_expr(lower, val_expr);
                    ir_emit_inst(func, IR_MOV, ir_op_stack(f_offset, f_size, is_signed), val, ir_op_none(), expr->loc);
                }
            } else {
                if (f->type && type_is_compound(f->type)) {
                    for (size_t z = 0; z < f_size; z += 8) {
                        size_t chunk = (f_size - z >= 8) ? 8 : (f_size - z);
                        ir_emit_inst(func, IR_MOV, ir_op_stack(f_offset + (int32_t)z, chunk, false),
                                     ir_op_const(0, chunk, false), ir_op_none(), expr->loc);
                    }
                } else {
                    ir_emit_inst(func, IR_MOV, ir_op_stack(f_offset, f_size, false),
                                 ir_op_const(0, f_size, false), ir_op_none(), expr->loc);
                }
            }
        } else {
            IROperand target_ptr_op = dest_addr;

            if (f->offset != 0) {
                uint32_t field_ptr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(field_ptr_vreg, 8, false),
                             dest_addr, ir_op_const((int64_t)f->offset, 8, false), expr->loc);
                target_ptr_op = ir_op_vreg(field_ptr_vreg, 8, false);
            }

            if (val_expr) {
                if (f->type && type_is_compound(f->type)) {
                    if (val_expr->kind == EXPR_STRUCT_LIT) {
                        ir_lower_struct_lit_into(lower, val_expr, target_ptr_op);
                    } else {
                        IROperand src_field_addr = ir_lower_addr(lower, val_expr);

                        if (src_field_addr.kind == IR_OP_STACK || src_field_addr.kind == IR_OP_GLOBAL) {
                            uint32_t src_field_vreg = ir_vreg_alloc(func);
                            ir_emit_inst(func, IR_ADDR, ir_op_vreg(src_field_vreg, 8, false),
                                         src_field_addr, ir_op_none(), expr->loc);
                            src_field_addr = ir_op_vreg(src_field_vreg, 8, false);
                        }

                        ir_emit_inst(func, IR_MEMCPY, target_ptr_op,
                                     src_field_addr, ir_op_const((int64_t)f_size, 8, false), expr->loc);
                    }
                } else {
                    IROperand val = ir_lower_expr(lower, val_expr);
                    val.byte_size = f_size;
                    ir_emit_inst(func, IR_STORE, target_ptr_op, val, ir_op_none(), expr->loc);
                }
            } else {
                if (f->type && type_is_compound(f->type)) {
                    for (size_t z = 0; z < f_size; z += 8) {
                        size_t chunk = (f_size - z >= 8) ? 8 : (f_size - z);
                        IROperand chunk_ptr_op = target_ptr_op;

                        if (z > 0) {
                            uint32_t chunk_ptr_vreg = ir_vreg_alloc(func);
                            ir_emit_inst(func, IR_ADD, ir_op_vreg(chunk_ptr_vreg, 8, false),
                                         target_ptr_op, ir_op_const((int64_t)z, 8, false), expr->loc);
                            chunk_ptr_op = ir_op_vreg(chunk_ptr_vreg, 8, false);
                        }

                        ir_emit_inst(func, IR_STORE, chunk_ptr_op,
                                     ir_op_const(0, chunk, false), ir_op_none(), expr->loc);
                    }
                } else {
                    ir_emit_inst(func, IR_STORE, target_ptr_op,
                                 ir_op_const(0, f_size, false), ir_op_none(), expr->loc);
                }
            }
        }
    }
}

static void ir_lower_array_lit_into(IRLower* lower, const AstExpr* expr, IROperand dest_addr) {
    if (!expr || expr->kind != EXPR_ARRAY_LIT) {
        return;
    }

    IRFunction* func = lower->current_func;
    Type* arr_type   = expr->type;
    Type* elem_type  = (arr_type && arr_type->kind == TYPE_ARRAY) ? arr_type->array.elem_type : NULL;
    size_t elem_size = (elem_type && elem_type->size) ? elem_type->size : 8;
    bool is_signed   = type_is_signed(elem_type);

    if (dest_addr.kind == IR_OP_GLOBAL) {
        uint32_t base_vreg = ir_vreg_alloc(func);
        ir_emit_inst(func, IR_ADDR, ir_op_vreg(base_vreg, 8, false), dest_addr, ir_op_none(), expr->loc);
        dest_addr = ir_op_vreg(base_vreg, 8, false);
    }

    for (size_t i = 0; i < expr->array_lit.count; ++i) {
        const AstExpr* elem_expr = expr->array_lit.elements[i];
        size_t offset = i * elem_size;

        if (dest_addr.kind == IR_OP_STACK) {
            int32_t elem_slot = dest_addr.stack_offset + (int32_t)offset;
            IROperand elem_dest = ir_op_stack(elem_slot, elem_size, is_signed);

            if (elem_expr->kind == EXPR_STRUCT_LIT) {
                ir_lower_struct_lit_into(lower, elem_expr, elem_dest);
            } else if (elem_expr->kind == EXPR_ARRAY_LIT) {
                ir_lower_array_lit_into(lower, elem_expr, elem_dest);
            } else if (type_is_compound(elem_expr->type)) {
                uint32_t dst_field_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADDR, ir_op_vreg(dst_field_vreg, 8, false),
                             elem_dest, ir_op_none(), expr->loc);

                IROperand src_field_addr = ir_lower_addr(lower, elem_expr);

                if (src_field_addr.kind == IR_OP_STACK || src_field_addr.kind == IR_OP_GLOBAL) {
                    uint32_t src_field_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(src_field_vreg, 8, false),
                                 src_field_addr, ir_op_none(), expr->loc);
                    src_field_addr = ir_op_vreg(src_field_vreg, 8, false);
                }

                ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(dst_field_vreg, 8, false),
                             src_field_addr, ir_op_const((int64_t)elem_size, 8, false), expr->loc);
            } else {
                IROperand val = ir_lower_expr(lower, elem_expr);
                val.byte_size = elem_size;
                ir_emit_inst(func, IR_MOV, elem_dest, val, ir_op_none(), expr->loc);
            }
        } else {
            IROperand target_ptr_op = dest_addr;

            if (offset != 0) {
                uint32_t elem_ptr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(elem_ptr_vreg, 8, false),
                             dest_addr, ir_op_const((int64_t)offset, 8, false), expr->loc);
                target_ptr_op = ir_op_vreg(elem_ptr_vreg, 8, false);
            }

            if (elem_expr->kind == EXPR_STRUCT_LIT) {
                ir_lower_struct_lit_into(lower, elem_expr, target_ptr_op);
            } else if (elem_expr->kind == EXPR_ARRAY_LIT) {
                ir_lower_array_lit_into(lower, elem_expr, target_ptr_op);
            } else if (type_is_compound(elem_expr->type)) {
                IROperand src_field_addr = ir_lower_addr(lower, elem_expr);

                if (src_field_addr.kind == IR_OP_STACK || src_field_addr.kind == IR_OP_GLOBAL) {
                    uint32_t src_field_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(src_field_vreg, 8, false),
                                 src_field_addr, ir_op_none(), expr->loc);
                    src_field_addr = ir_op_vreg(src_field_vreg, 8, false);
                }

                ir_emit_inst(func, IR_MEMCPY, target_ptr_op,
                             src_field_addr, ir_op_const((int64_t)elem_size, 8, false), expr->loc);
            } else {
                IROperand val = ir_lower_expr(lower, elem_expr);
                val.byte_size = elem_size;
                ir_emit_inst(func, IR_STORE, target_ptr_op, val, ir_op_none(), expr->loc);
            }
        }
    }
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

static IROperand ir_lower_addr(IRLower* lower, const AstExpr* expr) {
    IRFunction* func = lower->current_func;

    if (!expr) {
        return ir_op_none();
    }

    switch (expr->kind) {
        case EXPR_VAR: {
            Symbol* sym = expr->var.symbol;
            size_t size = (expr->type && expr->type->size) ? expr->type->size : 8;
            bool is_signed = type_is_signed(expr->type);

            if (sym->kind == SYM_GLOBAL_VAR || sym->kind == SYM_PROC) {
                return ir_op_global(sym->name, size, is_signed);
            }

            int32_t offset = symbol_slot_lookup(lower, sym);
            return ir_op_stack(offset, size, is_signed);
        }

        case EXPR_ARRAY_LIT: {
            Type* arr_type     = expr->type;
            size_t total_size  = (arr_type && arr_type->size) ? arr_type->size : 8;
            size_t total_align = (arr_type && arr_type->align) ? arr_type->align : 8;

            int32_t tmp_slot = ir_func_alloc_stack_slot(func, total_size, total_align);
            ir_lower_array_lit_into(lower, expr, ir_op_stack(tmp_slot, total_size, false));

            return ir_op_stack(tmp_slot, total_size, false);
        }

        case EXPR_INDEX: {
            if (expr->index.ptr->type && expr->index.ptr->type->kind == TYPE_SLICE) {
                IROperand slice_addr = ir_lower_addr(lower, expr->index.ptr);

                if (slice_addr.kind == IR_OP_STACK || slice_addr.kind == IR_OP_GLOBAL) {
                    uint32_t slice_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(slice_vreg, 8, false), slice_addr, ir_op_none(), expr->loc);
                    slice_addr = ir_op_vreg(slice_vreg, 8, false);
                }

                uint32_t ptr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_LOAD, ir_op_vreg(ptr_vreg, 8, false), slice_addr, ir_op_none(), expr->loc);

                IROperand idx_op = ir_lower_expr(lower, expr->index.index);
                size_t elem_size = (expr->type && expr->type->size) ? expr->type->size : 8;
                IROperand offset_op = idx_op;

                if (elem_size > 1) {
                    uint32_t scale_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8, false), idx_op,
                                 ir_op_const((int64_t)elem_size, 8, false), expr->loc);
                    offset_op = ir_op_vreg(scale_vreg, 8, false);
                }

                uint32_t elem_addr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(elem_addr_vreg, 8, false),
                             ir_op_vreg(ptr_vreg, 8, false), offset_op, expr->loc);

                return ir_op_vreg(elem_addr_vreg, 8, false);
            }

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
            if (expr->member.target->type && expr->member.target->type->kind == TYPE_SLICE) {
                IROperand base_addr;

                if (type_is_pointer(expr->member.target->type)) {
                    base_addr = ir_lower_expr(lower, expr->member.target);
                } else {
                    base_addr = ir_lower_addr(lower, expr->member.target);

                    if (base_addr.kind == IR_OP_STACK || base_addr.kind == IR_OP_GLOBAL) {
                        uint32_t base_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_ADDR, ir_op_vreg(base_vreg, 8, false), base_addr, ir_op_none(), expr->loc);
                        base_addr = ir_op_vreg(base_vreg, 8, false);
                    }
                }

                if (expr->member.field_name.len == 3 && memcmp(expr->member.field_name.data, "ptr", 3) == 0) {
                    return base_addr;
                }

                uint32_t addr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), base_addr,
                             ir_op_const(8, 8, false), expr->loc);

                return ir_op_vreg(addr_vreg, 8, false);
            }

            StructField* field = expr->member.field;
            IROperand base_addr;

            if (type_is_pointer(expr->member.target->type)) {
                base_addr = ir_lower_expr(lower, expr->member.target);
            } else {
                base_addr = ir_lower_addr(lower, expr->member.target);

                if (base_addr.kind == IR_OP_STACK || base_addr.kind == IR_OP_GLOBAL) {
                    uint32_t base_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(base_vreg, 8, false), base_addr, ir_op_none(), expr->loc);
                    base_addr = ir_op_vreg(base_vreg, 8, false);
                }
            }

            if (field->offset == 0 && !type_is_pointer(expr->member.target->type)) {
                return base_addr;
            }

            uint32_t addr_vreg = ir_vreg_alloc(func);
            ir_emit_inst(func, IR_ADD, ir_op_vreg(addr_vreg, 8, false), base_addr,
                         ir_op_const((int64_t)field->offset, 8, false), expr->loc);

            return ir_op_vreg(addr_vreg, 8, false);
        }

        case EXPR_UNARY: {
            if (expr->unary.op == TOK_STAR) {
                return ir_lower_expr(lower, expr->unary.operand);
            }
            break;
        }

        default: {
            IROperand val = ir_lower_expr(lower, expr);
            return val;
        }
    }

    return ir_op_none();
}

static IROperand ir_lower_expr(IRLower* lower, const AstExpr* expr) {
    if (!expr) {
        return ir_op_none();
    }

    IRFunction* func = lower->current_func;

    if (expr->type && expr->type->kind == TYPE_ARRAY) {
        IROperand addr = ir_lower_addr(lower, expr);

        if (addr.kind == IR_OP_STACK || addr.kind == IR_OP_GLOBAL) {
            uint32_t vreg = ir_vreg_alloc(func);
            ir_emit_inst(func, IR_ADDR, ir_op_vreg(vreg, 8, false), addr, ir_op_none(), expr->loc);
            return ir_op_vreg(vreg, 8, false);
        }

        return addr;
    }

    size_t expr_size = (expr->type && expr->type->size) ? expr->type->size : 8;
    bool is_signed   = type_is_signed(expr->type);

    switch (expr->kind) {
        case EXPR_INT_LIT:
        case EXPR_SIZEOF:
        case EXPR_ALIGNOF:
        case EXPR_OFFSETOF: {
            return ir_op_const(expr->int_val, expr_size, is_signed);
        }

        case EXPR_NULL: {
            return ir_op_const(0, expr_size, false);
        }

        case EXPR_STRING_LIT: {
            uint32_t str_id = register_string_literal(lower, expr->string_val);
            uint32_t vreg   = ir_vreg_alloc(func);

            ir_emit_inst(func, IR_GLOBAL_STR, ir_op_vreg(vreg, 8, false), ir_op_str(str_id), ir_op_none(), expr->loc);

            if (expr->type && expr->type->kind == TYPE_SLICE) {
                int32_t tmp_slot = ir_func_alloc_stack_slot(func, 16, 8);

                ir_emit_inst(func, IR_MOV, ir_op_stack(tmp_slot, 8, false),
                             ir_op_vreg(vreg, 8, false), ir_op_none(), expr->loc);
                ir_emit_inst(func, IR_MOV, ir_op_stack(tmp_slot + 8, 8, false),
                             ir_op_const((int64_t)expr->string_val.len, 8, false), ir_op_none(), expr->loc);

                return ir_op_stack(tmp_slot, 16, false);
            }

            return ir_op_vreg(vreg, 8, false);
        }

        case EXPR_VAR: {
            assert(expr->var.symbol != NULL);

            if (expr->var.symbol->kind == SYM_CONST) {
                return ir_op_const(expr->var.symbol->const_val, expr_size, is_signed);
            }

            if (expr->var.symbol->kind == SYM_PROC) {
                uint32_t vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADDR, ir_op_vreg(vreg, 8, false),
                             ir_op_global(expr->var.symbol->name, 8, false), ir_op_none(), expr->loc);
                return ir_op_vreg(vreg, 8, false);
            }

            IROperand addr = ir_lower_addr(lower, expr);

            if (type_is_compound(expr->type)) {
                return addr;
            }

            uint32_t vreg  = ir_vreg_alloc(func);

            ir_emit_inst(func, IR_MOV, ir_op_vreg(vreg, expr_size, is_signed), addr, ir_op_none(), expr->loc);

            return ir_op_vreg(vreg, expr_size, is_signed);
        }

        case EXPR_UNARY: {
            if (expr->unary.op == TOK_STAR) {
                IROperand ptr_op = ir_lower_expr(lower, expr->unary.operand);
                uint32_t vreg    = ir_vreg_alloc(func);

                ir_emit_inst(func, IR_LOAD, ir_op_vreg(vreg, expr_size, is_signed), ptr_op, ir_op_none(), expr->loc);

                return ir_op_vreg(vreg, expr_size, is_signed);
            }

            if (expr->unary.op == TOK_AMP) {
                IROperand addr = ir_lower_addr(lower, expr->unary.operand);

                if (addr.kind == IR_OP_VREG) {
                    return addr;
                }

                uint32_t vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADDR, ir_op_vreg(vreg, 8, false), addr, ir_op_none(), expr->loc);

                return ir_op_vreg(vreg, 8, false);
            }

            if (expr->unary.op == TOK_MINUS) {
                IROperand inner_op = ir_lower_expr(lower, expr->unary.operand);
                uint32_t vreg      = ir_vreg_alloc(func);

                ir_emit_inst(func, IR_NEG, ir_op_vreg(vreg, expr_size, is_signed), inner_op, ir_op_none(), expr->loc);

                return ir_op_vreg(vreg, expr_size, is_signed);
            }

            if (expr->unary.op == TOK_TILDE) {
                IROperand inner_op = ir_lower_expr(lower, expr->unary.operand);
                uint32_t vreg      = ir_vreg_alloc(func);

                ir_emit_inst(func, IR_NOT, ir_op_vreg(vreg, expr_size, false), inner_op, ir_op_none(), expr->loc);

                return ir_op_vreg(vreg, expr_size, false);
            }

            if (expr->unary.op == TOK_BANG) {
                IROperand inner_op = ir_lower_expr(lower, expr->unary.operand);
                uint32_t vreg      = ir_vreg_alloc(func);

                ir_emit_inst(func, IR_CMP_EQ, ir_op_vreg(vreg, 1, false), inner_op,
                             ir_op_const(0, inner_op.byte_size, false), expr->loc);

                return ir_op_vreg(vreg, 1, false);
            }

            return ir_lower_expr(lower, expr->unary.operand);
        }

        case EXPR_BINARY: {
            if (expr->binary.op == TOK_AMP_AMP || expr->binary.op == TOK_PIPE_PIPE) {
                IRBlock* bb_true  = ir_block_create(func, "bb_logic_true");
                IRBlock* bb_false = ir_block_create(func, "bb_logic_false");
                IRBlock* bb_merge = ir_block_create(func, "bb_logic_merge");

                int32_t tmp_slot = ir_func_alloc_stack_slot(func, 1, 1);

                ir_lower_cond(lower, expr, bb_true, bb_false);

                ir_block_switch(func, bb_true);
                ir_emit_inst(func, IR_MOV, ir_op_stack(tmp_slot, 1, false),
                             ir_op_const(1, 1, false), ir_op_none(), expr->loc);
                ir_emit_inst(func, IR_JMP, ir_op_block(bb_merge), ir_op_none(), ir_op_none(), expr->loc);

                ir_block_switch(func, bb_false);
                ir_emit_inst(func, IR_MOV, ir_op_stack(tmp_slot, 1, false),
                             ir_op_const(0, 1, false), ir_op_none(), expr->loc);
                ir_emit_inst(func, IR_JMP, ir_op_block(bb_merge), ir_op_none(), ir_op_none(), expr->loc);

                ir_block_switch(func, bb_merge);

                uint32_t res_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_MOV, ir_op_vreg(res_vreg, 1, false),
                             ir_op_stack(tmp_slot, 1, false), ir_op_none(), expr->loc);

                return ir_op_vreg(res_vreg, 1, false);
            }

            if (expr->binary.lhs->type && expr->binary.lhs->type->kind == TYPE_SLICE &&
                (expr->binary.op == TOK_EQ_EQ || expr->binary.op == TOK_BANG_EQ)) {

                IROperand lhs_addr = ir_lower_addr(lower, expr->binary.lhs);
                if (lhs_addr.kind == IR_OP_STACK || lhs_addr.kind == IR_OP_GLOBAL) {
                    uint32_t v = ir_vreg_alloc(func);

                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(v, 8, false), lhs_addr, ir_op_none(), expr->loc);

                    lhs_addr = ir_op_vreg(v, 8, false);
                }

                IROperand rhs_addr = ir_lower_addr(lower, expr->binary.rhs);

                if (rhs_addr.kind == IR_OP_STACK || rhs_addr.kind == IR_OP_GLOBAL) {
                    uint32_t v = ir_vreg_alloc(func);

                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(v, 8, false), rhs_addr, ir_op_none(), expr->loc);

                    rhs_addr = ir_op_vreg(v, 8, false);
                }

                uint32_t lhs_ptr = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_LOAD, ir_op_vreg(lhs_ptr, 8, false), lhs_addr, ir_op_none(), expr->loc);

                uint32_t rhs_ptr = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_LOAD, ir_op_vreg(rhs_ptr, 8, false), rhs_addr, ir_op_none(), expr->loc);

                uint32_t lhs_len_addr = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(lhs_len_addr, 8, false), lhs_addr, ir_op_const(8, 8, false), expr->loc);
                uint32_t lhs_len = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_LOAD, ir_op_vreg(lhs_len, 8, false), ir_op_vreg(lhs_len_addr, 8, false), ir_op_none(), expr->loc);

                uint32_t rhs_len_addr = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(rhs_len_addr, 8, false), rhs_addr, ir_op_const(8, 8, false), expr->loc);
                uint32_t rhs_len = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_LOAD, ir_op_vreg(rhs_len, 8, false), ir_op_vreg(rhs_len_addr, 8, false), ir_op_none(), expr->loc);

                if (expr->binary.op == TOK_EQ_EQ) {
                    uint32_t cmp_ptr = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_CMP_EQ, ir_op_vreg(cmp_ptr, 1, false),
                                 ir_op_vreg(lhs_ptr, 8, false), ir_op_vreg(rhs_ptr, 8, false), expr->loc);

                    uint32_t cmp_len = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_CMP_EQ, ir_op_vreg(cmp_len, 1, false),
                                 ir_op_vreg(lhs_len, 8, false), ir_op_vreg(rhs_len, 8, false), expr->loc);

                    uint32_t res_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_AND, ir_op_vreg(res_vreg, 1, false),
                                 ir_op_vreg(cmp_ptr, 1, false), ir_op_vreg(cmp_len, 1, false), expr->loc);

                    return ir_op_vreg(res_vreg, 1, false);
                } else {
                    uint32_t cmp_ptr = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_CMP_NE, ir_op_vreg(cmp_ptr, 1, false),
                                 ir_op_vreg(lhs_ptr, 8, false), ir_op_vreg(rhs_ptr, 8, false), expr->loc);

                    uint32_t cmp_len = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_CMP_NE, ir_op_vreg(cmp_len, 1, false),
                                 ir_op_vreg(lhs_len, 8, false), ir_op_vreg(rhs_len, 8, false), expr->loc);

                    uint32_t res_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_OR, ir_op_vreg(res_vreg, 1, false),
                                 ir_op_vreg(cmp_ptr, 1, false), ir_op_vreg(cmp_len, 1, false), expr->loc);

                    return ir_op_vreg(res_vreg, 1, false);
                }
            }

            IROperand lhs = ir_lower_expr(lower, expr->binary.lhs);
            IROperand rhs = ir_lower_expr(lower, expr->binary.rhs);

            if (expr->binary.op == TOK_PLUS || expr->binary.op == TOK_MINUS) {
                bool lhs_is_ptr = type_is_pointer(expr->binary.lhs->type);
                bool rhs_is_ptr = type_is_pointer(expr->binary.rhs->type);

                if (lhs_is_ptr && type_is_integer(expr->binary.rhs->type)) {
                    Type* base = expr->binary.lhs->type->ptr.base;
                    size_t elem_size = (base && base->size) ? base->size : 1;

                    if (elem_size > 1) {
                        uint32_t scale_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8, false), rhs,
                                     ir_op_const((int64_t)elem_size, 8, false), expr->loc);
                        rhs = ir_op_vreg(scale_vreg, 8, false);
                    }
                } else if (rhs_is_ptr && type_is_integer(expr->binary.lhs->type) && expr->binary.op == TOK_PLUS) {
                    Type* base = expr->binary.rhs->type->ptr.base;
                    size_t elem_size = (base && base->size) ? base->size : 1;

                    if (elem_size > 1) {
                        uint32_t scale_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8, false), lhs,
                                     ir_op_const((int64_t)elem_size, 8, false), expr->loc);
                        lhs = ir_op_vreg(scale_vreg, 8, false);
                    }
                }
            }

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

            ir_emit_inst(func, op, ir_op_vreg(vreg, expr_size, is_signed), lhs, rhs, expr->loc);

            return ir_op_vreg(vreg, expr_size, is_signed);
        }

        case EXPR_CAST: {
            IROperand inner_op = ir_lower_expr(lower, expr->cast.expr);
            uint32_t vreg      = ir_vreg_alloc(func);

            ir_emit_inst(func, IR_MOV, ir_op_vreg(vreg, expr_size, is_signed), inner_op, ir_op_none(), expr->loc);

            return ir_op_vreg(vreg, expr_size, is_signed);
        }

        case EXPR_INDEX:
        case EXPR_MEMBER: {
            IROperand addr = ir_lower_addr(lower, expr);

            if (type_is_compound(expr->type)) {
                return addr;
            }

            uint32_t vreg  = ir_vreg_alloc(func);
            ir_emit_inst(func, IR_LOAD, ir_op_vreg(vreg, expr_size, is_signed), addr, ir_op_none(), expr->loc);

            return ir_op_vreg(vreg, expr_size, is_signed);
        }

        case EXPR_ALLOCA: {
            Type* elem_t     = expr->alloca_expr.elem_type;
            size_t elem_size = (elem_t && elem_t->size) ? elem_t->size : 1;

            uint32_t ptr_vreg = ir_vreg_alloc(func);

            if (expr->alloca_expr.count_expr->kind == EXPR_INT_LIT) {
                int64_t total_bytes = expr->alloca_expr.count_expr->int_val * (int64_t)elem_size;
                int64_t aligned_bytes = (total_bytes + 15) & ~15;

                ir_emit_inst(func, IR_ALLOCA, ir_op_vreg(ptr_vreg, 8, false),
                             ir_op_const(aligned_bytes, 8, false), ir_op_none(), expr->loc);
            } else {
                IROperand count_op = ir_lower_expr(lower, expr->alloca_expr.count_expr);
                IROperand size_op  = count_op;

                if (elem_size > 1) {
                    uint32_t scale_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8, false), count_op,
                                 ir_op_const((int64_t)elem_size, 8, false), expr->loc);
                    size_op = ir_op_vreg(scale_vreg, 8, false);
                }

                uint32_t add_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(add_vreg, 8, false), size_op,
                             ir_op_const(15, 8, false), expr->loc);

                uint32_t align_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_AND, ir_op_vreg(align_vreg, 8, false), ir_op_vreg(add_vreg, 8, false),
                             ir_op_const(-16, 8, true), expr->loc);

                ir_emit_inst(func, IR_ALLOCA, ir_op_vreg(ptr_vreg, 8, false),
                             ir_op_vreg(align_vreg, 8, false), ir_op_none(), expr->loc);
            }

            return ir_op_vreg(ptr_vreg, 8, false);
        }

        case EXPR_ASM: {
            IROperand dst = ir_op_none();

            if (expr->type && expr->type->kind != TYPE_VOID) {
                uint32_t vreg = ir_vreg_alloc(func);
                dst = ir_op_vreg(vreg, expr_size, is_signed);
            }

            IRAsmOp* ir_inputs = NULL;

            if (expr->inline_asm.input_count > 0) {
                ir_inputs = ARENA_NEW_ARRAY(lower->arena, IRAsmOp, expr->inline_asm.input_count);

                for (size_t i = 0; i < expr->inline_asm.input_count; ++i) {
                    AsmOperand* in_op = &expr->inline_asm.inputs[i];
                    IROperand val = ir_lower_expr(lower, in_op->expr);

                    ir_inputs[i].reg       = in_op->reg;
                    ir_inputs[i].byte_size = in_op->byte_size ? in_op->byte_size : val.byte_size;
                    ir_inputs[i].val       = val;
                }
            }

            IRAsmOp* ir_outputs = NULL;

            if (expr->inline_asm.output_count > 0) {
                ir_outputs = ARENA_NEW_ARRAY(lower->arena, IRAsmOp, expr->inline_asm.output_count);

                for (size_t i = 0; i < expr->inline_asm.output_count; ++i) {
                    AsmOperand* out_op = &expr->inline_asm.outputs[i];

                    ir_outputs[i].reg       = out_op->reg;
                    ir_outputs[i].byte_size = out_op->byte_size ? out_op->byte_size : 8;
                    ir_outputs[i].val       = ir_op_none();
                }
            }

            uint32_t clobber_mask = 0;

            for (size_t i = 0; i < expr->inline_asm.clobber_count; ++i) {
                size_t reg_size = 0;
                X86Reg r = parse_reg_name(expr->inline_asm.clobbers[i], &reg_size);

                if (r != REG_NONE) {
                    clobber_mask |= (1 << r);
                }
            }

            IRInst* inst = ir_emit_inst(func, IR_INLINE_ASM, dst, ir_op_none(), ir_op_none(), expr->loc);
            inst->symbol_name      = expr->inline_asm.code;
            inst->asm_inputs       = ir_inputs;
            inst->asm_input_count  = expr->inline_asm.input_count;
            inst->asm_outputs      = ir_outputs;
            inst->asm_output_count = expr->inline_asm.output_count;
            inst->clobber_mask     = clobber_mask;
            inst->clobbers_memory  = expr->inline_asm.clobbers_memory;

            for (size_t i = 0; i < expr->inline_asm.output_count; ++i) {
                AsmOperand* out_op = &expr->inline_asm.outputs[i];

                if (out_op->expr != NULL) {
                    IROperand dst_addr = ir_lower_addr(lower, out_op->expr);
                    dst_addr.byte_size = out_op->byte_size;

                    uint32_t out_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_MOV, ir_op_vreg(out_vreg, out_op->byte_size, false),
                                 ir_op_reg(out_op->reg, out_op->byte_size, false), ir_op_none(), expr->loc);

                    if (dst_addr.kind == IR_OP_STACK || dst_addr.kind == IR_OP_GLOBAL) {
                        ir_emit_inst(func, IR_MOV, dst_addr,
                                     ir_op_vreg(out_vreg, out_op->byte_size, false), ir_op_none(), expr->loc);
                    } else {
                        ir_emit_inst(func, IR_STORE, dst_addr,
                                     ir_op_vreg(out_vreg, out_op->byte_size, false), ir_op_none(), expr->loc);
                    }
                }
            }

            return dst;
        }

        case EXPR_STRUCT_LIT: {
            Type* st = expr->struct_lit.struct_type;
            size_t total_size  = (st && st->size) ? st->size : 8;
            size_t total_align = (st && st->align) ? st->align : 8;

            int32_t tmp_slot = ir_func_alloc_stack_slot(func, total_size, total_align);

            ir_lower_struct_lit_into(lower, expr, ir_op_stack(tmp_slot, total_size, false));

            return ir_op_stack(tmp_slot, total_size, false);
        }

        case EXPR_ARRAY_LIT: {
            Type* arr_type     = expr->type;
            size_t total_size  = (arr_type && arr_type->size) ? arr_type->size : 8;
            size_t total_align = (arr_type && arr_type->align) ? arr_type->align : 8;

            int32_t tmp_slot = ir_func_alloc_stack_slot(func, total_size, total_align);

            ir_lower_array_lit_into(lower, expr, ir_op_stack(tmp_slot, total_size, false));

            return ir_op_stack(tmp_slot, total_size, false);
        }

        case EXPR_SLICE: {
            Type* target_type = expr->slice.target->type;
            Type* elem_type   = expr->type->slice.elem_type;
            size_t elem_size  = (elem_type && elem_type->size) ? elem_type->size : 1;

            int32_t tmp_slot = ir_func_alloc_stack_slot(func, 16, 8);

            IROperand base_ptr;
            IROperand max_len = ir_op_none();

            if (target_type->kind == TYPE_ARRAY) {
                IROperand arr_addr = ir_lower_addr(lower, expr->slice.target);

                if (arr_addr.kind == IR_OP_STACK || arr_addr.kind == IR_OP_GLOBAL) {
                    uint32_t vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(vreg, 8, false), arr_addr, ir_op_none(), expr->loc);
                    base_ptr = ir_op_vreg(vreg, 8, false);
                } else {
                    base_ptr = arr_addr;
                }

                max_len = ir_op_const((int64_t)target_type->array.count, 8, false);
            } else if (target_type->kind == TYPE_SLICE) {
                IROperand slice_addr = ir_lower_addr(lower, expr->slice.target);

                if (slice_addr.kind == IR_OP_STACK || slice_addr.kind == IR_OP_GLOBAL) {
                    uint32_t vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(vreg, 8, false), slice_addr, ir_op_none(), expr->loc);
                    slice_addr = ir_op_vreg(vreg, 8, false);
                }

                uint32_t ptr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_LOAD, ir_op_vreg(ptr_vreg, 8, false), slice_addr, ir_op_none(), expr->loc);
                base_ptr = ir_op_vreg(ptr_vreg, 8, false);

                uint32_t len_addr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADD, ir_op_vreg(len_addr_vreg, 8, false), slice_addr,
                             ir_op_const(8, 8, false), expr->loc);

                uint32_t len_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_LOAD, ir_op_vreg(len_vreg, 8, false),
                             ir_op_vreg(len_addr_vreg, 8, false), ir_op_none(), expr->loc);
                max_len = ir_op_vreg(len_vreg, 8, false);
            } else {
                base_ptr = ir_lower_expr(lower, expr->slice.target);
            }

            IROperand start_op = expr->slice.start ? ir_lower_expr(lower, expr->slice.start) : ir_op_const(0, 8, false);
            IROperand end_op   = expr->slice.end ? ir_lower_expr(lower, expr->slice.end) : max_len;

            uint32_t slice_len_vreg = ir_vreg_alloc(func);
            ir_emit_inst(func, IR_SUB, ir_op_vreg(slice_len_vreg, 8, false), end_op, start_op, expr->loc);

            IROperand offset_op = start_op;

            if (elem_size > 1) {
                uint32_t scale_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8, false), start_op,
                             ir_op_const((int64_t)elem_size, 8, false), expr->loc);
                offset_op = ir_op_vreg(scale_vreg, 8, false);
            }

            uint32_t final_ptr_vreg = ir_vreg_alloc(func);
            ir_emit_inst(func, IR_ADD, ir_op_vreg(final_ptr_vreg, 8, false), base_ptr, offset_op, expr->loc);

            ir_emit_inst(func, IR_MOV, ir_op_stack(tmp_slot, 8, false),
                         ir_op_vreg(final_ptr_vreg, 8, false), ir_op_none(), expr->loc);
            ir_emit_inst(func, IR_MOV, ir_op_stack(tmp_slot + 8, 8, false),
                         ir_op_vreg(slice_len_vreg, 8, false), ir_op_none(), expr->loc);

            return ir_op_stack(tmp_slot, 16, false);
        }

        case EXPR_TUPLE: {
            Type* tuple_type   = expr->type;
            size_t total_size  = tuple_type->size ? tuple_type->size : 8;
            size_t total_align = tuple_type->align ? tuple_type->align : 8;

            int32_t tmp_slot = ir_func_alloc_stack_slot(func, total_size, total_align);

            for (size_t i = 0; i < expr->tuple.count; ++i) {
                Type* elem_t     = tuple_type->tuple.elements[i];
                size_t elem_size = (elem_t && elem_t->size) ? elem_t->size : 8;
                int32_t elem_off = tmp_slot + (int32_t)tuple_type->tuple.offsets[i];

                IROperand val = ir_lower_expr(lower, expr->tuple.elements[i]);

                if (type_is_compound(elem_t)) {
                    uint32_t dst_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(dst_vreg, 8, false),
                                 ir_op_stack(elem_off, elem_size, false), ir_op_none(), expr->loc);

                    IROperand src_addr = val;

                    if (src_addr.kind == IR_OP_STACK || src_addr.kind == IR_OP_GLOBAL) {
                        uint32_t src_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_ADDR, ir_op_vreg(src_vreg, 8, false),
                                     src_addr, ir_op_none(), expr->loc);
                        src_addr = ir_op_vreg(src_vreg, 8, false);
                    }

                    ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(dst_vreg, 8, false),
                                 src_addr, ir_op_const((int64_t)elem_size, 8, false), expr->loc);
                } else {
                    ir_emit_inst(func, IR_MOV, ir_op_stack(elem_off, elem_size, false), val, ir_op_none(), expr->loc);
                }
            }

            return ir_op_stack(tmp_slot, total_size, false);
        }

        case EXPR_CALL: {
            const AstProc* callee_proc = NULL;

            if (expr->call.callee_sym && expr->call.callee_sym->proc_decl) {
                callee_proc = expr->call.callee_sym->proc_decl;
            }

            if (callee_proc && callee_proc->attrs.is_inlined) {
                IROperand inlined_res = ir_op_none();

                if (ir_try_inline_call(lower, callee_proc, expr->call.args, expr->call.arg_count, &inlined_res, expr->loc)) {
                    return inlined_res;
                }
            }

            bool ret_is_sret = type_requires_sret(expr->type);
            size_t total_args    = expr->call.arg_count + (ret_is_sret ? 1 : 0);
            IROperand* args      = ARENA_NEW_ARRAY(lower->arena, IROperand, total_args);
            size_t arg_idx       = 0;

            int32_t ret_slot = 0;

            if (ret_is_sret) {
                size_t ret_size  = expr->type->size ? expr->type->size : 8;
                size_t ret_align = expr->type->align ? expr->type->align : 8;
                ret_slot = ir_func_alloc_stack_slot(func, ret_size, ret_align);

                uint32_t ret_addr_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADDR, ir_op_vreg(ret_addr_vreg, 8, false),
                             ir_op_stack(ret_slot, ret_size, false), ir_op_none(), expr->loc);

                args[arg_idx++] = ir_op_vreg(ret_addr_vreg, 8, false);
            }

            for (size_t i = 0; i < expr->call.arg_count; ++i) {
                const AstExpr* arg = expr->call.args[i];

                if (type_is_compound(arg->type)) {
                    IROperand compound_addr = ir_lower_addr(lower, arg);

                    if (compound_addr.kind == IR_OP_STACK || compound_addr.kind == IR_OP_GLOBAL) {
                        uint32_t addr_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_ADDR, ir_op_vreg(addr_vreg, 8, false),
                                     compound_addr, ir_op_none(), arg->loc);
                        args[arg_idx++] = ir_op_vreg(addr_vreg, 8, false);
                    } else {
                        args[arg_idx++] = compound_addr;
                    }
                } else {
                    args[arg_idx++] = ir_lower_expr(lower, arg);
                }
            }

            IROperand dst = ir_op_none();

            if (!ret_is_sret && expr->type && expr->type->kind != TYPE_VOID) {
                uint32_t vreg = ir_vreg_alloc(func);
                dst = ir_op_vreg(vreg, expr_size, is_signed);
            }

            IROpcode opcode = (expr->call.callee_expr != NULL) ? IR_CALL_PTR : IR_CALL;
            IROperand src1  = ir_op_none();

            if (opcode == IR_CALL_PTR) {
                src1 = ir_lower_expr(lower, expr->call.callee_expr);
            }

            IRInst* call_inst = ir_emit_inst(func, opcode, dst, src1, ir_op_none(), expr->loc);
            call_inst->symbol_name     = expr->call.callee_name;
            call_inst->extra_args      = args;
            call_inst->extra_arg_count = total_args;

            bool is_callee_variadic = false;

            if (callee_proc) {
                is_callee_variadic = callee_proc->is_variadic;
            } else if (expr->call.callee_sym && expr->call.callee_sym->type && expr->call.callee_sym->type->kind == TYPE_FUNC) {
                is_callee_variadic = expr->call.callee_sym->type->func.is_variadic;
            } else if (expr->call.callee_expr && expr->call.callee_expr->type) {
                Type* t = expr->call.callee_expr->type;
                if (t->kind == TYPE_PTR) t = t->ptr.base;
                if (t->kind == TYPE_FUNC) is_callee_variadic = t->func.is_variadic;
            }

            call_inst->is_variadic = is_callee_variadic;

            if (ret_is_sret) {
                return ir_op_stack(ret_slot, expr->type->size, false);
            }

            return dst;
        }

        case EXPR_VA_START: {
            IROperand ap_addr = ir_lower_addr(lower, expr->va_op.valist_expr);

            ir_emit_inst(
                func,
                IR_VA_START,
                ap_addr,
                ir_op_stack(func->reg_save_slot, KLANG_ABI_GP_REG_SAVE_SIZE, false),
                ir_op_none(),
                expr->loc
            );

            return ir_op_none();
        }

        case EXPR_VA_ARG: {
            IROperand ap_addr = ir_lower_addr(lower, expr->va_op.valist_expr);

            Type* target_type = expr->va_op.target_type;

            size_t size = target_type->size ? target_type->size : 8;
            bool is_signed = type_is_signed(target_type);

            if (type_is_compound(target_type)) {
                size_t align = target_type->align ? target_type->align : 8;

                int32_t value_slot =
                    ir_func_alloc_stack_slot(func, size, align);

                uint32_t value_ptr_vreg = ir_vreg_alloc(func);

                ir_emit_inst(
                    func,
                    IR_VA_ARG,
                    ir_op_vreg(value_ptr_vreg, 8, false),
                    ap_addr,
                    ir_op_const(KLANG_ABI_GP_SLOT_SIZE, 8, false),
                    expr->loc
                );

                uint32_t destination_vreg = ir_vreg_alloc(func);

                ir_emit_inst(
                    func,
                    IR_ADDR,
                    ir_op_vreg(destination_vreg, 8, false),
                    ir_op_stack(value_slot, size, false),
                    ir_op_none(),
                    expr->loc
                );

                ir_emit_inst(
                    func,
                    IR_MEMCPY,
                    ir_op_vreg(destination_vreg, 8, false),
                    ir_op_vreg(value_ptr_vreg, 8, false),
                    ir_op_const((int64_t)size, 8, false),
                    expr->loc
                );

                return ir_op_stack(value_slot, size, false);
            }

            uint32_t vreg = ir_vreg_alloc(func);

            ir_emit_inst(
                func,
                IR_VA_ARG,
                ir_op_vreg(vreg, size, is_signed),
                ap_addr,
                ir_op_const(KLANG_ABI_GP_SLOT_SIZE, 8, false),
                expr->loc
            );

            return ir_op_vreg(vreg, size, is_signed);
        }

        case EXPR_VA_END: {
            IROperand ap_addr = ir_lower_addr(lower, expr->va_op.valist_expr);
            ir_emit_inst(func, IR_VA_END, ap_addr, ir_op_none(), ir_op_none(), expr->loc);
            return ir_op_none();
        }

        case EXPR_VA_COPY: {
            IROperand dst_addr = ir_lower_addr(lower, expr->va_op.valist_expr);
            IROperand src_addr = ir_lower_addr(lower, expr->va_op.src_valist_expr);
            ir_emit_inst(func, IR_VA_COPY, dst_addr, src_addr, ir_op_none(), expr->loc);
            return ir_op_none();
        }
    }

    return ir_op_none();
}

static void ir_lower_stmt(IRLower* lower, const AstStmt* stmt);

static void ir_lower_cond(IRLower* lower, const AstExpr* expr, IRBlock* bb_true, IRBlock* bb_false) {
    if (!expr) {
        ir_emit_inst(lower->current_func, IR_JMP, ir_op_block(bb_true), ir_op_none(), ir_op_none(), (SourceLoc){0});
        return;
    }

    IRFunction* func = lower->current_func;

    switch (expr->kind) {
        case EXPR_BINARY: {
            if (expr->binary.op == TOK_AMP_AMP) {
                IRBlock* bb_rhs = ir_block_create(func, "bb_logic_and_rhs");

                ir_lower_cond(lower, expr->binary.lhs, bb_rhs, bb_false);

                ir_block_switch(func, bb_rhs);
                ir_lower_cond(lower, expr->binary.rhs, bb_true, bb_false);

                return;
            }

            if (expr->binary.op == TOK_PIPE_PIPE) {
                IRBlock* bb_rhs = ir_block_create(func, "bb_logic_or_rhs");

                ir_lower_cond(lower, expr->binary.lhs, bb_true, bb_rhs);

                ir_block_switch(func, bb_rhs);
                ir_lower_cond(lower, expr->binary.rhs, bb_true, bb_false);

                return;
            }

            break;
        }

        case EXPR_UNARY: {
            if (expr->unary.op == TOK_BANG) {
                ir_lower_cond(lower, expr->unary.operand, bb_false, bb_true);
                return;
            }

            break;
        }

        case EXPR_INT_LIT: {
            if (expr->int_val != 0) {
                ir_emit_inst(func, IR_JMP, ir_op_block(bb_true), ir_op_none(), ir_op_none(), expr->loc);
            } else {
                ir_emit_inst(func, IR_JMP, ir_op_block(bb_false), ir_op_none(), ir_op_none(), expr->loc);
            }

            return;
        }

        case EXPR_NULL: {
            ir_emit_inst(func, IR_JMP, ir_op_block(bb_false), ir_op_none(), ir_op_none(), expr->loc);
            return;
        }

        case EXPR_VAR: {
            if (expr->var.symbol && expr->var.symbol->kind == SYM_CONST) {
                if (expr->var.symbol->const_val != 0) {
                    ir_emit_inst(func, IR_JMP, ir_op_block(bb_true), ir_op_none(), ir_op_none(), expr->loc);
                } else {
                    ir_emit_inst(func, IR_JMP, ir_op_block(bb_false), ir_op_none(), ir_op_none(), expr->loc);
                }

                return;
            }

            break;
        }

        default:
            break;
    }

    IROperand cond_op = ir_lower_expr(lower, expr);

    if (cond_op.kind == IR_OP_CONST) {
        if (cond_op.int_val != 0) {
            ir_emit_inst(func, IR_JMP, ir_op_block(bb_true), ir_op_none(), ir_op_none(), expr->loc);
        } else {
            ir_emit_inst(func, IR_JMP, ir_op_block(bb_false), ir_op_none(), ir_op_none(), expr->loc);
        }

        return;
    }

    ir_emit_inst(func, IR_BR, cond_op, ir_op_block(bb_true), ir_op_block(bb_false), expr->loc);
}

static void ir_lower_stmt(IRLower* lower, const AstStmt* stmt) {
    if (!stmt) {
        return;
    }

    IRFunction* func = lower->current_func;

    switch (stmt->kind) {
        case STMT_BLOCK: {
            defer_scope_push(lower, false);

            for (size_t i = 0; i < stmt->block.count; ++i) {
                ir_lower_stmt(lower, stmt->block.stmts[i]);
            }

            if (!func->current_block->is_terminated) {
                emit_defers_in_scope(lower, lower->current_defer_scope);
            }

            defer_scope_pop(lower);
            break;
        }

        case STMT_DEFER: {
            defer_register(lower, stmt->defer_stmt.stmt);
            break;
        }

        case STMT_VAR_DECL: {
            Symbol* sym    = stmt->var_decl.symbol;
            size_t size    = (sym->type && sym->type->size) ? sym->type->size : 8;
            size_t align   = (sym->type && sym->type->align) ? sym->type->align : 8;
            bool is_signed = type_is_signed(sym->type);

            int32_t offset = ir_func_alloc_stack_slot(func, size, align);
            symbol_slot_bind(lower, sym, offset);

            if (stmt->var_decl.init_expr) {
                if (stmt->var_decl.init_expr->kind == EXPR_STRUCT_LIT) {
                    ir_lower_struct_lit_into(lower, stmt->var_decl.init_expr, ir_op_stack(offset, size, false));
                } else if (stmt->var_decl.init_expr->kind == EXPR_ARRAY_LIT) {
                    ir_lower_array_lit_into(lower, stmt->var_decl.init_expr, ir_op_stack(offset, size, false));
                } else if (type_is_compound(sym->type)) {
                    uint32_t dst_addr = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(dst_addr, 8, false),
                                 ir_op_stack(offset, size, false), ir_op_none(), stmt->loc);

                    IROperand src_addr = ir_lower_addr(lower, stmt->var_decl.init_expr);

                    if (src_addr.kind == IR_OP_STACK || src_addr.kind == IR_OP_GLOBAL) {
                        uint32_t src_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_ADDR, ir_op_vreg(src_vreg, 8, false),
                                     src_addr, ir_op_none(), stmt->loc);
                        src_addr = ir_op_vreg(src_vreg, 8, false);
                    }

                    ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(dst_addr, 8, false),
                                 src_addr, ir_op_const((int64_t)size, 8, false), stmt->loc);
                } else {
                    IROperand init_val = ir_lower_expr(lower, stmt->var_decl.init_expr);
                    ir_emit_inst(func, IR_MOV, ir_op_stack(offset, size, is_signed),
                                 init_val, ir_op_none(), stmt->loc);
                }
            }
            break;
        }

        case STMT_DESTRUCTURE_DECL: {
            IROperand tuple_op = ir_lower_expr(lower, stmt->destructure_decl.init_expr);
            Type* tuple_t      = stmt->destructure_decl.init_expr->type;

            if (tuple_op.kind != IR_OP_STACK) {
                int32_t tmp_slot = ir_func_alloc_stack_slot(func, tuple_t->size, tuple_t->align);
                uint32_t dst_addr = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADDR, ir_op_vreg(dst_addr, 8, false),
                             ir_op_stack(tmp_slot, tuple_t->size, false), ir_op_none(), stmt->loc);
                ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(dst_addr, 8, false),
                             tuple_op, ir_op_const((int64_t)tuple_t->size, 8, false), stmt->loc);
                tuple_op = ir_op_stack(tmp_slot, tuple_t->size, false);
            }

            for (size_t i = 0; i < stmt->destructure_decl.count; ++i) {
                Symbol* sym = stmt->destructure_decl.symbols[i];

                if (!sym) {
                    continue;
                }

                Type* elem_t      = tuple_t->tuple.elements[i];
                size_t elem_size  = (elem_t && elem_t->size) ? elem_t->size : 8;
                size_t elem_align = (elem_t && elem_t->align) ? elem_t->align : 8;
                bool is_signed    = type_is_signed(elem_t);

                int32_t var_slot = ir_func_alloc_stack_slot(func, elem_size, elem_align);
                symbol_slot_bind(lower, sym, var_slot);

                int32_t elem_offset = tuple_op.stack_offset + (int32_t)tuple_t->tuple.offsets[i];

                if (type_is_compound(elem_t)) {
                    uint32_t dst_addr = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(dst_addr, 8, false),
                                 ir_op_stack(var_slot, elem_size, false), ir_op_none(), stmt->loc);

                    uint32_t src_addr = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(src_addr, 8, false),
                                 ir_op_stack(elem_offset, elem_size, false), ir_op_none(), stmt->loc);

                    ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(dst_addr, 8, false),
                                 ir_op_vreg(src_addr, 8, false),
                                 ir_op_const((int64_t)elem_size, 8, false), stmt->loc);
                } else {
                    uint32_t tmp_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_MOV, ir_op_vreg(tmp_vreg, elem_size, is_signed),
                                 ir_op_stack(elem_offset, elem_size, is_signed), ir_op_none(), stmt->loc);
                    ir_emit_inst(func, IR_MOV, ir_op_stack(var_slot, elem_size, is_signed),
                                 ir_op_vreg(tmp_vreg, elem_size, is_signed), ir_op_none(), stmt->loc);
                }
            }

            break;
        }

        case STMT_DESTRUCTURE_ASSIGN: {
            IROperand tuple_op = ir_lower_expr(lower, stmt->destructure_assign.value);
            Type* tuple_t      = stmt->destructure_assign.value->type;

            if (tuple_op.kind != IR_OP_STACK) {
                int32_t tmp_slot = ir_func_alloc_stack_slot(func, tuple_t->size, tuple_t->align);
                uint32_t dst_addr = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADDR, ir_op_vreg(dst_addr, 8, false),
                             ir_op_stack(tmp_slot, tuple_t->size, false), ir_op_none(), stmt->loc);

                IROperand src_addr = tuple_op;
                if (src_addr.kind == IR_OP_GLOBAL) {
                    uint32_t src_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(src_vreg, 8, false),
                                 src_addr, ir_op_none(), stmt->loc);
                    src_addr = ir_op_vreg(src_vreg, 8, false);
                }

                ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(dst_addr, 8, false),
                             src_addr, ir_op_const((int64_t)tuple_t->size, 8, false), stmt->loc);

                tuple_op = ir_op_stack(tmp_slot, tuple_t->size, false);
            }

            for (size_t i = 0; i < stmt->destructure_assign.count; ++i) {
                AstExpr* target = stmt->destructure_assign.targets[i];

                if (target->kind == EXPR_VAR && target->var.name.len == 1 && target->var.name.data[0] == '_') {
                    continue;
                }

                Type* elem_t     = tuple_t->tuple.elements[i];
                size_t elem_size = (elem_t && elem_t->size) ? elem_t->size : 8;
                bool is_signed   = type_is_signed(elem_t);

                IROperand dst_addr  = ir_lower_addr(lower, target);
                int32_t elem_offset = tuple_op.stack_offset + (int32_t)tuple_t->tuple.offsets[i];

                if (type_is_compound(elem_t)) {
                    if (dst_addr.kind == IR_OP_STACK || dst_addr.kind == IR_OP_GLOBAL) {
                        uint32_t dst_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_ADDR, ir_op_vreg(dst_vreg, 8, false),
                                     dst_addr, ir_op_none(), stmt->loc);
                        dst_addr = ir_op_vreg(dst_vreg, 8, false);
                    }

                    uint32_t src_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(src_vreg, 8, false),
                                 ir_op_stack(elem_offset, elem_size, false), ir_op_none(), stmt->loc);

                    ir_emit_inst(func, IR_MEMCPY, dst_addr, ir_op_vreg(src_vreg, 8, false),
                                 ir_op_const((int64_t)elem_size, 8, false), stmt->loc);
                } else {
                    uint32_t tmp_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_MOV, ir_op_vreg(tmp_vreg, elem_size, is_signed),
                                 ir_op_stack(elem_offset, elem_size, is_signed), ir_op_none(), stmt->loc);

                    if (dst_addr.kind == IR_OP_STACK || dst_addr.kind == IR_OP_GLOBAL) {
                        dst_addr.byte_size = elem_size;
                        ir_emit_inst(func, IR_MOV, dst_addr, ir_op_vreg(tmp_vreg, elem_size, is_signed), ir_op_none(), stmt->loc);
                    } else {
                        ir_emit_inst(func, IR_STORE, dst_addr, ir_op_vreg(tmp_vreg, elem_size, is_signed), ir_op_none(), stmt->loc);
                    }
                }
            }

            break;
        }

        case STMT_ASSIGN: {
            IROperand dst_addr = ir_lower_addr(lower, stmt->assign.target);

            if (stmt->assign.value->kind == EXPR_STRUCT_LIT) {
                if (dst_addr.kind == IR_OP_STACK || dst_addr.kind == IR_OP_GLOBAL) {
                    ir_lower_struct_lit_into(lower, stmt->assign.value, dst_addr);
                } else {
                    ir_lower_struct_lit_into(lower, stmt->assign.value, dst_addr);
                }
            } else if (stmt->assign.value->kind == EXPR_ARRAY_LIT) {
                ir_lower_array_lit_into(lower, stmt->assign.value, dst_addr);
            } else if (type_is_compound(stmt->assign.target->type)) {
                size_t size = stmt->assign.target->type->size;
                IROperand src_addr = ir_lower_addr(lower, stmt->assign.value);

                if (dst_addr.kind == IR_OP_STACK || dst_addr.kind == IR_OP_GLOBAL) {
                    uint32_t dst_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(dst_vreg, 8, false),
                                 dst_addr, ir_op_none(), stmt->loc);
                    dst_addr = ir_op_vreg(dst_vreg, 8, false);
                }

                if (src_addr.kind == IR_OP_STACK || src_addr.kind == IR_OP_GLOBAL) {
                    uint32_t src_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_ADDR, ir_op_vreg(src_vreg, 8, false),
                                 src_addr, ir_op_none(), stmt->loc);
                    src_addr = ir_op_vreg(src_vreg, 8, false);
                }

                ir_emit_inst(func, IR_MEMCPY, dst_addr, src_addr,
                             ir_op_const((int64_t)size, 8, false), stmt->loc);
            } else {
                IROperand val = ir_lower_expr(lower, stmt->assign.value);
                size_t target_size = stmt->assign.target->type->size ? stmt->assign.target->type->size : 8;

                val.byte_size = target_size;

                if (dst_addr.kind == IR_OP_STACK || dst_addr.kind == IR_OP_GLOBAL) {
                    dst_addr.byte_size = target_size;
                    ir_emit_inst(func, IR_MOV, dst_addr, val, ir_op_none(), stmt->loc);
                } else {
                    ir_emit_inst(func, IR_STORE, dst_addr, val, ir_op_none(), stmt->loc);
                }
            }
            break;
        }

        case STMT_COMPOUND_ASSIGN: {
            IROperand old_val = ir_lower_expr(lower, stmt->compound_assign.target);
            IROperand delta   = ir_lower_expr(lower, stmt->compound_assign.value);
            size_t size       = stmt->compound_assign.target->type->size;
            bool is_signed    = type_is_signed(stmt->compound_assign.target->type);

            if (stmt->compound_assign.op == TOK_PLUS_EQ || stmt->compound_assign.op == TOK_MINUS_EQ) {
                if (type_is_pointer(stmt->compound_assign.target->type) && type_is_integer(stmt->compound_assign.value->type)) {
                    Type* base = stmt->compound_assign.target->type->ptr.base;
                    size_t elem_size = (base && base->size) ? base->size : 1;

                    if (elem_size > 1) {
                        uint32_t scale_vreg = ir_vreg_alloc(func);
                        ir_emit_inst(func, IR_MUL, ir_op_vreg(scale_vreg, 8, false), delta,
                                     ir_op_const((int64_t)elem_size, 8, false), stmt->loc);
                        delta = ir_op_vreg(scale_vreg, 8, false);
                    }
                }
            }

            uint32_t vreg = ir_vreg_alloc(func);
            IROpcode op   = IR_ADD;

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

            IROperand dst_addr = ir_lower_addr(lower, stmt->compound_assign.target);

            if (dst_addr.kind == IR_OP_STACK || dst_addr.kind == IR_OP_GLOBAL) {
                dst_addr.byte_size = size;
                ir_emit_inst(func, IR_MOV, dst_addr, ir_op_vreg(vreg, size, is_signed), ir_op_none(), stmt->loc);
            } else {
                ir_emit_inst(func, IR_STORE, dst_addr, ir_op_vreg(vreg, size, is_signed), ir_op_none(), stmt->loc);
            }
            break;
        }

        case STMT_RETURN: {
            if (lower->current_inline != NULL) {
                InlineContext* inline_ctx = lower->current_inline;

                if (stmt->return_stmt.expr) {
                    if (type_requires_sret(stmt->return_stmt.expr->type)) {
                        size_t size = stmt->return_stmt.expr->type->size;

                        if (stmt->return_stmt.expr->kind == EXPR_STRUCT_LIT) {
                            ir_lower_struct_lit_into(lower, stmt->return_stmt.expr,
                                                     ir_op_stack(inline_ctx->ret_sret_slot, size, false));
                        } else {
                            IROperand src_addr = ir_lower_addr(lower, stmt->return_stmt.expr);

                            if (src_addr.kind == IR_OP_STACK || src_addr.kind == IR_OP_GLOBAL) {
                                uint32_t src_vreg = ir_vreg_alloc(func);
                                ir_emit_inst(func, IR_ADDR, ir_op_vreg(src_vreg, 8, false),
                                             src_addr, ir_op_none(), stmt->loc);
                                src_addr = ir_op_vreg(src_vreg, 8, false);
                            }

                            uint32_t dst_vreg = ir_vreg_alloc(func);
                            ir_emit_inst(func, IR_ADDR, ir_op_vreg(dst_vreg, 8, false),
                                         ir_op_stack(inline_ctx->ret_sret_slot, size, false), ir_op_none(), stmt->loc);

                            ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(dst_vreg, 8, false),
                                         src_addr, ir_op_const((int64_t)size, 8, false), stmt->loc);
                        }
                    } else {
                        IROperand ret_val = ir_lower_expr(lower, stmt->return_stmt.expr);
                        ir_emit_inst(func, IR_MOV, inline_ctx->ret_val_op, ret_val, ir_op_none(), stmt->loc);
                    }
                }

                emit_defers_up_to_scope(lower, inline_ctx->boundary_defer_scope);
                ir_emit_inst(func, IR_JMP, ir_op_block(inline_ctx->bb_return), ir_op_none(), ir_op_none(), stmt->loc);
                break;
            }

            if (stmt->return_stmt.expr) {
                if (type_requires_sret(stmt->return_stmt.expr->type)) {
                    size_t size = stmt->return_stmt.expr->type->size;

                    uint32_t sret_ptr = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_MOV, ir_op_vreg(sret_ptr, 8, false),
                                 ir_op_stack(lower->current_sret_slot, 8, false), ir_op_none(), stmt->loc);

                    if (stmt->return_stmt.expr->kind == EXPR_STRUCT_LIT) {
                        ir_lower_struct_lit_into(lower, stmt->return_stmt.expr, ir_op_vreg(sret_ptr, 8, false));
                    } else {
                        IROperand src_addr = ir_lower_addr(lower, stmt->return_stmt.expr);

                        if (src_addr.kind == IR_OP_STACK || src_addr.kind == IR_OP_GLOBAL) {
                            uint32_t src_vreg = ir_vreg_alloc(func);
                            ir_emit_inst(func, IR_ADDR, ir_op_vreg(src_vreg, 8, false),
                                         src_addr, ir_op_none(), stmt->loc);
                            src_addr = ir_op_vreg(src_vreg, 8, false);
                        }

                        ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(sret_ptr, 8, false),
                                     src_addr, ir_op_const((int64_t)size, 8, false), stmt->loc);
                    }

                    emit_defers_all(lower);

                    ir_emit_inst(func, IR_RET, ir_op_vreg(sret_ptr, 8, false), ir_op_none(), ir_op_none(), stmt->loc);
                    break;
                }

                IROperand ret_val = ir_lower_expr(lower, stmt->return_stmt.expr);

                if (ret_val.kind == IR_OP_STACK || ret_val.kind == IR_OP_GLOBAL) {
                    uint32_t tmp_vreg = ir_vreg_alloc(func);
                    ir_emit_inst(func, IR_MOV, ir_op_vreg(tmp_vreg, ret_val.byte_size, ret_val.is_signed),
                                 ret_val, ir_op_none(), stmt->loc);
                    ret_val = ir_op_vreg(tmp_vreg, ret_val.byte_size, ret_val.is_signed);
                }

                emit_defers_all(lower);

                ir_emit_inst(func, IR_RET, ret_val, ir_op_none(), ir_op_none(), stmt->loc);
            } else {
                emit_defers_all(lower);

                ir_emit_inst(func, IR_RET, ir_op_none(), ir_op_none(), ir_op_none(), stmt->loc);
            }
            break;
        }

        case STMT_IF: {
            IRBlock* bb_then  = ir_block_create(func, "bb_if_then");
            IRBlock* bb_else  = stmt->if_stmt.else_branch ? ir_block_create(func, "bb_if_else") : NULL;
            IRBlock* bb_merge = ir_block_create(func, "bb_if_merge");

            IRBlock* bb_false = bb_else ? bb_else : bb_merge;

            ir_lower_cond(lower, stmt->if_stmt.cond, bb_then, bb_false);

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
                emit_defers_up_to_loop(lower);

                ir_emit_inst(func, IR_JMP, ir_op_block(lower->current_loop->bb_end),
                             ir_op_none(), ir_op_none(), stmt->loc);
            }
            break;
        }

        case STMT_CONTINUE: {
            LoopContext* loop = lower->current_loop;

            while (loop && !loop->bb_cond) {
                loop = loop->prev;
            }

            if (loop && loop->bb_cond) {
                emit_defers_up_to_loop(lower);

                ir_emit_inst(func, IR_JMP, ir_op_block(loop->bb_cond),
                             ir_op_none(), ir_op_none(), stmt->loc);
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
            ir_lower_cond(lower, stmt->while_stmt.cond, bb_body, bb_end);

            ir_block_switch(func, bb_body);

            defer_scope_push(lower, true);

            ir_lower_stmt(lower, stmt->while_stmt.body);

            if (!func->current_block->is_terminated) {
                emit_defers_in_scope(lower, lower->current_defer_scope);
                ir_emit_inst(func, IR_JMP, ir_op_block(bb_cond), ir_op_none(), ir_op_none(), stmt->loc);
            }

            defer_scope_pop(lower);

            lower->current_loop = loop_ctx.prev;

            ir_block_switch(func, bb_end);
            break;
        }

        case STMT_FOR: {
            IRBlock* bb_cond = ir_block_create(func, "bb_for_cond");
            IRBlock* bb_body = ir_block_create(func, "bb_for_body");
            IRBlock* bb_step = ir_block_create(func, "bb_for_step");
            IRBlock* bb_end  = ir_block_create(func, "bb_for_end");

            if (stmt->for_stmt.init) {
                ir_lower_stmt(lower, stmt->for_stmt.init);
            }

            LoopContext loop_ctx = {
                .bb_cond = bb_step,
                .bb_end  = bb_end,
                .prev    = lower->current_loop
            };
            lower->current_loop = &loop_ctx;

            ir_emit_inst(func, IR_JMP, ir_op_block(bb_cond), ir_op_none(), ir_op_none(), stmt->loc);

            ir_block_switch(func, bb_cond);

            if (stmt->for_stmt.cond) {
                ir_lower_cond(lower, stmt->for_stmt.cond, bb_body, bb_end);
            } else {
                ir_emit_inst(func, IR_JMP, ir_op_block(bb_body), ir_op_none(), ir_op_none(), stmt->loc);
            }

            ir_block_switch(func, bb_body);

            defer_scope_push(lower, true);

            ir_lower_stmt(lower, stmt->for_stmt.body);

            if (!func->current_block->is_terminated) {
                emit_defers_in_scope(lower, lower->current_defer_scope);
                ir_emit_inst(func, IR_JMP, ir_op_block(bb_step), ir_op_none(), ir_op_none(), stmt->loc);
            }

            defer_scope_pop(lower);

            ir_block_switch(func, bb_step);

            if (stmt->for_stmt.step) {
                ir_lower_stmt(lower, stmt->for_stmt.step);
            }

            if (!func->current_block->is_terminated) {
                ir_emit_inst(func, IR_JMP, ir_op_block(bb_cond), ir_op_none(), ir_op_none(), stmt->loc);
            }

            lower->current_loop = loop_ctx.prev;

            ir_block_switch(func, bb_end);
            break;
        }

        case STMT_SWITCH: {
            IRBlock* bb_switch_end = ir_block_create(func, "bb_switch_end");

            size_t case_count = stmt->switch_stmt.case_count;
            IRBlock** case_blocks = ARENA_NEW_ARRAY(lower->arena, IRBlock*, case_count);
            IRBlock* default_block = NULL;

            for (size_t i = 0; i < case_count; ++i) {
                if (stmt->switch_stmt.cases[i].is_default) {
                    default_block = ir_block_create(func, "bb_case_default");
                    case_blocks[i] = default_block;
                } else {
                    case_blocks[i] = ir_block_create(func, "bb_case");
                }
            }

            if (!default_block) {
                default_block = bb_switch_end;
            }

            IROperand cond_op = ir_lower_expr(lower, stmt->switch_stmt.cond);

            for (size_t i = 0; i < case_count; ++i) {
                const AstSwitchCase* c = &stmt->switch_stmt.cases[i];
                if (c->is_default) {
                    continue;
                }

                for (size_t p = 0; p < c->pattern_count; ++p) {
                    const AstCasePattern* pat = &c->patterns[p];
                    IRBlock* bb_next_check = ir_block_create(func, "bb_switch_next");

                    if (pat->is_range) {
                        IRBlock* bb_check_upper = ir_block_create(func, "bb_switch_range_hi");
                        uint32_t cmp_ge = ir_vreg_alloc(func);

                        ir_emit_inst(func, IR_CMP_GE, ir_op_vreg(cmp_ge, 1, false), cond_op,
                                     ir_op_const(pat->const_start, cond_op.byte_size, cond_op.is_signed), pat->loc);

                        ir_emit_inst(func, IR_BR, ir_op_vreg(cmp_ge, 1, false),
                                     ir_op_block(bb_check_upper), ir_op_block(bb_next_check), pat->loc);

                        ir_block_switch(func, bb_check_upper);

                        uint32_t cmp_le = ir_vreg_alloc(func);

                        ir_emit_inst(func, IR_CMP_LE, ir_op_vreg(cmp_le, 1, false), cond_op,
                                     ir_op_const(pat->const_end, cond_op.byte_size, cond_op.is_signed), pat->loc);

                        ir_emit_inst(func, IR_BR, ir_op_vreg(cmp_le, 1, false),
                                     ir_op_block(case_blocks[i]), ir_op_block(bb_next_check), pat->loc);
                    } else {
                        uint32_t cmp_eq = ir_vreg_alloc(func);

                        ir_emit_inst(func, IR_CMP_EQ, ir_op_vreg(cmp_eq, 1, false), cond_op,
                                     ir_op_const(pat->const_start, cond_op.byte_size, cond_op.is_signed), pat->loc);

                        ir_emit_inst(func, IR_BR, ir_op_vreg(cmp_eq, 1, false),
                                     ir_op_block(case_blocks[i]), ir_op_block(bb_next_check), pat->loc);
                    }

                    ir_block_switch(func, bb_next_check);
                }
            }

            if (!func->current_block->is_terminated) {
                ir_emit_inst(func, IR_JMP, ir_op_block(default_block), ir_op_none(), ir_op_none(), stmt->loc);
            }

            LoopContext switch_ctx = {
                .bb_cond = NULL,
                .bb_end  = bb_switch_end,
                .prev    = lower->current_loop
            };

            lower->current_loop = &switch_ctx;

            for (size_t i = 0; i < case_count; ++i) {
                const AstSwitchCase* c = &stmt->switch_stmt.cases[i];
                ir_block_switch(func, case_blocks[i]);

                defer_scope_push(lower, false);

                for (size_t s = 0; s < c->stmt_count; ++s) {
                    ir_lower_stmt(lower, c->stmts[s]);
                }

                if (!func->current_block->is_terminated) {
                    emit_defers_in_scope(lower, lower->current_defer_scope);
                    ir_emit_inst(func, IR_JMP, ir_op_block(bb_switch_end), ir_op_none(), ir_op_none(), c->loc);
                }

                defer_scope_pop(lower);
            }

            lower->current_loop = switch_ctx.prev;

            ir_block_switch(func, bb_switch_end);
            break;
        }

        case STMT_EXPR: {
            ir_lower_expr(lower, stmt->expr_stmt.expr);
            break;
        }
    }
}

static void ir_add_global_item(IRLower* lower, IRGlobalVar* gv, IRDataItem item) {
    ARENA_DA_PUSH(lower->arena, gv->init_items, gv->init_item_count, gv->init_item_cap, item);
}

static void ir_emit_global_data(IRLower* lower, IRGlobalVar* gv, const AstExpr* expr, Type* type, size_t target_size) {
    if (!type) {
        if (target_size > 0) {
            ir_add_global_item(lower, gv, (IRDataItem){ .kind = IR_DATA_ZERO, .size = target_size });
        }
        return;
    }

    size_t type_size = type->size ? type->size : target_size;

    if (!expr) {
        if (type_size > 0) {
            ir_add_global_item(lower, gv, (IRDataItem){ .kind = IR_DATA_ZERO, .size = type_size });
        }
        return;
    }

    if (expr->kind == EXPR_STRING_LIT) {
        uint32_t str_id = register_string_literal(lower, expr->string_val);

        if (type->kind == TYPE_SLICE) {
            ir_add_global_item(lower, gv, (IRDataItem){ .kind = IR_DATA_STR_REF, .size = 8, .str_id = str_id });
            ir_add_global_item(lower, gv, (IRDataItem){ .kind = IR_DATA_INT, .size = 8, .val = (int64_t)expr->string_val.len });
            return;
        }

        if (type->kind == TYPE_ARRAY) {
            size_t copy_len = expr->string_val.len;

            if (copy_len > type_size) {
                copy_len = type_size;
            }

            for (size_t i = 0; i < copy_len; ++i) {
                ir_add_global_item(lower, gv, (IRDataItem){
                    .kind = IR_DATA_INT,
                    .size = 1,
                    .val  = (int64_t)(unsigned char)expr->string_val.data[i]
                });
            }

            if (type_size > copy_len) {
                ir_add_global_item(lower, gv, (IRDataItem){
                    .kind = IR_DATA_ZERO,
                    .size = type_size - copy_len
                });
            }
            return;
        }

        ir_add_global_item(lower, gv, (IRDataItem){ .kind = IR_DATA_STR_REF, .size = 8, .str_id = str_id });
        return;
    }

    if (expr->kind == EXPR_STRUCT_LIT) {
        Type* st = expr->struct_lit.struct_type ? expr->struct_lit.struct_type : type;

        if (st && (st->kind == TYPE_STRUCT || st->kind == TYPE_UNION)) {
            size_t current_offset = 0;

            for (size_t f_idx = 0; f_idx < st->structure.field_count; ++f_idx) {
                StructField* f = &st->structure.fields[f_idx];
                size_t f_size  = (f->type && f->type->size) ? f->type->size : 8;

                if (f->offset > current_offset) {
                    size_t pad = f->offset - current_offset;
                    ir_add_global_item(lower, gv, (IRDataItem){ .kind = IR_DATA_ZERO, .size = pad });
                    current_offset = f->offset;
                }

                const AstExpr* val_expr = NULL;

                for (size_t i = 0; i < expr->struct_lit.field_count; ++i) {
                    if (strview_equals(expr->struct_lit.field_names[i], f->name)) {
                        val_expr = expr->struct_lit.field_values[i];
                        break;
                    }
                }

                if (!val_expr && f->default_value) {
                    val_expr = f->default_value;
                }

                ir_emit_global_data(lower, gv, val_expr, f->type, f_size);
                current_offset += f_size;

                if (st->kind == TYPE_UNION) {
                    break;
                }
            }

            if (type_size > current_offset) {
                ir_add_global_item(lower, gv, (IRDataItem){ .kind = IR_DATA_ZERO, .size = type_size - current_offset });
            }
            return;
        }
    }

    if (expr->kind == EXPR_ARRAY_LIT) {
        Type* elem_type = (type->kind == TYPE_ARRAY) ? type->array.elem_type :
                          (type->kind == TYPE_SLICE ? type->slice.elem_type : NULL);
        size_t elem_size = (elem_type && elem_type->size) ? elem_type->size : 8;

        for (size_t i = 0; i < expr->array_lit.count; ++i) {
            ir_emit_global_data(lower, gv, expr->array_lit.elements[i], elem_type, elem_size);
        }

        size_t written_size = expr->array_lit.count * elem_size;

        if (type_size > written_size) {
            ir_add_global_item(lower, gv, (IRDataItem){ .kind = IR_DATA_ZERO, .size = type_size - written_size });
        }
        return;
    }

    if (expr->kind == EXPR_VAR) {
        Symbol* sym = expr->var.symbol;

        if (sym && sym->kind == SYM_CONST) {
            ir_add_global_item(lower, gv, (IRDataItem){
                .kind = IR_DATA_INT,
                .size = type_size,
                .val  = sym->const_val
            });
            return;
        }

        if (sym && (sym->kind == SYM_PROC || sym->kind == SYM_GLOBAL_VAR)) {
            ir_add_global_item(lower, gv, (IRDataItem){
                .kind     = IR_DATA_SYM_REF,
                .size     = 8,
                .sym_name = sym->name,
                .val      = 0
            });
            return;
        }
    }

    if (expr->kind == EXPR_UNARY && expr->unary.op == TOK_AMP) {
        const AstExpr* op = expr->unary.operand;

        if (op->kind == EXPR_VAR && op->var.symbol) {
            ir_add_global_item(lower, gv, (IRDataItem){
                .kind     = IR_DATA_SYM_REF,
                .size     = 8,
                .sym_name = op->var.symbol->name,
                .val      = 0
            });
            return;
        }

        if (op->kind == EXPR_MEMBER && op->member.target->kind == EXPR_VAR && op->member.target->var.symbol) {
            size_t off = op->member.field ? op->member.field->offset : 0;

            ir_add_global_item(lower, gv, (IRDataItem){
                .kind     = IR_DATA_SYM_REF,
                .size     = 8,
                .sym_name = op->member.target->var.symbol->name,
                .val      = (int64_t)off
            });
            return;
        }
    }

    if (expr->kind == EXPR_NULL) {
        ir_add_global_item(lower, gv, (IRDataItem){
            .kind = IR_DATA_INT,
            .size = type_size,
            .val  = 0
        });
        return;
    }

    int64_t const_val = 0;

    if (eval_expr_const_int(NULL, expr, &const_val)) {
        ir_add_global_item(lower, gv, (IRDataItem){
            .kind = IR_DATA_INT,
            .size = type_size,
            .val  = const_val
        });
        return;
    }

    ir_add_global_item(lower, gv, (IRDataItem){ .kind = IR_DATA_ZERO, .size = type_size });
}

IRModule* ir_lower_program(Arena* arena, const AstProgram* program) {
    if (!program) {
        return NULL;
    }

    IRModule* module = ir_module_create(arena);

    IRLower lower = {
        .arena               = arena,
        .module              = module,
        .current_func        = NULL,
        .current_loop        = NULL,
        .symbol_slots        = NULL,
        .current_sret_slot   = 0,
        .current_defer_scope = NULL,
        .current_inline      = NULL
    };

    for (size_t i = 0; i < program->global_count; ++i) {
        const AstGlobalVarDef* g = program->globals[i];

        IRGlobalVar* gv = ARENA_NEW_ZERO(arena, IRGlobalVar);
        gv->name            = g->name;
        gv->type            = g->type;
        gv->has_init        = (g->init_expr != NULL && !g->attrs.is_extern);
        gv->init_items      = NULL;
        gv->init_item_count = 0;
        gv->init_item_cap   = 0;
        gv->attrs           = g->attrs;

        if (gv->has_init) {
            size_t total_size = (g->type && g->type->size) ? g->type->size : 8;
            ir_emit_global_data(&lower, gv, g->init_expr, g->type, total_size);
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

    for (size_t i = 0; i < program->proc_count; ++i) {
        const AstProc* proc = program->procs[i];

        if (proc->is_generic) {
            continue;
        }

        IRFunction* func = ir_function_create(module, proc->name, proc->return_type);
        func->attrs                 = proc->attrs;
        func->is_variadic           = proc->is_variadic;
        func->abi_fixed_gp_arg_count =
            abi_function_fixed_gp_arg_count(proc->return_type, proc->param_count);

        if (func->is_variadic) {
            func->reg_save_slot = ir_func_alloc_stack_slot(func, KLANG_ABI_GP_REG_SAVE_SIZE, 16);
        }

        lower.current_func          = func;
        lower.symbol_slots          = NULL;
        lower.current_defer_scope   = NULL;

        size_t hidden_gp_arg_count =
            abi_function_hidden_gp_arg_count(proc->return_type);

        size_t reg_param_idx = 0;
        int32_t sret_slot = 0;

        if (hidden_gp_arg_count != 0) {
            sret_slot = ir_func_alloc_stack_slot(func, 8, 8);

            ir_emit_inst(
                func,
                IR_PARAM,
                ir_op_stack(sret_slot, 8, false),
                ir_op_const(0, 8, false),
                ir_op_none(),
                proc->loc
            );

            reg_param_idx += hidden_gp_arg_count;
        }

        lower.current_sret_slot = sret_slot;

        int32_t* struct_local_slots = ARENA_NEW_ARRAY(arena, int32_t, proc->param_count);
        int32_t* struct_ptr_slots   = ARENA_NEW_ARRAY(arena, int32_t, proc->param_count);

        for (size_t p = 0; p < proc->param_count; ++p) {
            const AstParam* param = &proc->params[p];
            size_t p_idx          = reg_param_idx++;
            bool is_signed        = type_is_signed(param->type);
            size_t var_size       = (param->type && param->type->size) ? param->type->size : 8;
            size_t var_align      = (param->type && param->type->align) ? param->type->align : 8;

            if (type_is_compound(param->type)) {
                struct_local_slots[p] = ir_func_alloc_stack_slot(func, var_size, var_align);
                symbol_slot_bind(&lower, param->symbol, struct_local_slots[p]);

                if (p_idx < 6) {
                    struct_ptr_slots[p] = ir_func_alloc_stack_slot(func, 8, 8);
                    ir_emit_inst(func, IR_PARAM, ir_op_stack(struct_ptr_slots[p], 8, false),
                                 ir_op_const((int64_t)p_idx, 8, false), ir_op_none(), param->loc);
                } else {
                    struct_ptr_slots[p] = (int32_t)(16 + (p_idx - 6) * 8);
                }
            } else {
                if (p_idx < 6) {
                    int32_t slot = ir_func_alloc_stack_slot(func, var_size, var_align);
                    symbol_slot_bind(&lower, param->symbol, slot);
                    ir_emit_inst(func, IR_PARAM, ir_op_stack(slot, var_size, is_signed),
                                 ir_op_const((int64_t)p_idx, 8, false), ir_op_none(), param->loc);
                } else {
                    int32_t stack_arg_off = (int32_t)(16 + (p_idx - 6) * 8);
                    symbol_slot_bind(&lower, param->symbol, stack_arg_off);
                }
            }
        }

        for (size_t p = 0; p < proc->param_count; ++p) {
            const AstParam* param = &proc->params[p];

            if (type_is_compound(param->type)) {
                size_t var_size = param->type->size ? param->type->size : 8;
                uint32_t src_vreg = ir_vreg_alloc(func);

                ir_emit_inst(func, IR_MOV, ir_op_vreg(src_vreg, 8, false),
                             ir_op_stack(struct_ptr_slots[p], 8, false), ir_op_none(), param->loc);

                uint32_t dst_vreg = ir_vreg_alloc(func);
                ir_emit_inst(func, IR_ADDR, ir_op_vreg(dst_vreg, 8, false),
                             ir_op_stack(struct_local_slots[p], var_size, false), ir_op_none(), param->loc);

                ir_emit_inst(func, IR_MEMCPY, ir_op_vreg(dst_vreg, 8, false),
                             ir_op_vreg(src_vreg, 8, false),
                             ir_op_const((int64_t)var_size, 8, false), param->loc);
            }
        }

        defer_scope_push(&lower, false);

        ir_lower_stmt(&lower, proc->body);

        if (!func->current_block->is_terminated) {
            emit_defers_in_scope(&lower, lower.current_defer_scope);
            ir_emit_inst(func, IR_RET, ir_op_none(), ir_op_none(), ir_op_none(), proc->loc);
        }

        defer_scope_pop(&lower);
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

        case IR_OP_REG:
            printf("%%%s", reg_name((X86Reg)op.reg, op.byte_size));
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

    if (module->str_count > 0) {
        printf("; --- String Constants ---\n");
        for (IRStringConst* s = module->first_str; s != NULL; s = s->next) {
            printf(".str_%u = ", s->id);
            ir_dump_escaped_string(s->value);
            printf("\n");
        }
        printf("\n");
    }

    if (module->global_count > 0) {
        printf("; --- Global Variables ---\n");
        for (IRGlobalVar* g = module->first_global; g != NULL; g = g->next) {
            printf("global @%.*s: %s", (int)g->name.len, g->name.data,
                   type_to_str(g->type, dump_arena));
            if (g->has_init) {
                printf(" = {");
                for (size_t i = 0; i < g->init_item_count; ++i) {
                    IRDataItem* item = &g->init_items[i];
                    switch (item->kind) {
                        case IR_DATA_INT:
                            printf(" %lld", (long long)item->val);
                            break;
                        case IR_DATA_STR_REF:
                            printf(" str(%u)", item->str_id);
                            break;
                        case IR_DATA_SYM_REF:
                            printf(" sym(%.*s)", (int)item->sym_name.len, item->sym_name.data);
                            break;
                        case IR_DATA_ZERO:
                            printf(" zero(%zu)", item->size);
                            break;
                    }
                    if (i < g->init_item_count - 1) printf(",");
                }
                printf(" }\n");
            } else {
                printf(" (uninitialized, %zu bytes)\n", g->type && g->type->size ? g->type->size : 8);
            }
        }
        printf("\n");
    }

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

                    case IR_MOV:
                        ir_dump_operand(inst->dst);
                        printf(" = mov ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_LOAD:
                        ir_dump_operand(inst->dst);
                        printf(" = load.%zu ", inst->dst.byte_size);
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_STORE:
                        printf("store.%zu ", inst->dst.byte_size);
                        ir_dump_operand(inst->dst);
                        printf(", ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_ADDR:
                        ir_dump_operand(inst->dst);
                        printf(" = addr ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_ALLOCA:
                        ir_dump_operand(inst->dst);
                        printf(" = alloca size: ");
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
                    case IR_CALL_PTR:
                    case IR_TAIL_CALL:
                    case IR_TAIL_CALL_PTR:
                        if (inst->dst.kind != IR_OP_NONE) {
                            ir_dump_operand(inst->dst);
                            printf(" = ");
                        }
                        if (inst->opcode == IR_CALL_PTR || inst->opcode == IR_TAIL_CALL_PTR) {
                            printf(inst->opcode == IR_TAIL_CALL_PTR ? "tail_call_ptr " : "call_ptr ");
                            ir_dump_operand(inst->src1);
                            printf("(");
                        } else {
                            printf(inst->opcode == IR_TAIL_CALL ? "tail_call @%.*s(" : "call @%.*s(",
                                   (int)inst->symbol_name.len, inst->symbol_name.data);
                        }
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

                    case IR_VA_START:
                        printf("va_start ");
                        ir_dump_operand(inst->dst);
                        printf("\n");
                        break;

                    case IR_VA_ARG:
                        ir_dump_operand(inst->dst);
                        printf(" = va_arg ");
                        ir_dump_operand(inst->src1);
                        printf(", slot_size: ");
                        ir_dump_operand(inst->src2);
                        printf("\n");
                        break;

                    case IR_VA_END:
                        printf("va_end ");
                        ir_dump_operand(inst->dst);
                        printf("\n");
                        break;

                    case IR_VA_COPY:
                        printf("va_copy ");
                        ir_dump_operand(inst->dst);
                        printf(", ");
                        ir_dump_operand(inst->src1);
                        printf("\n");
                        break;

                    case IR_PHI:
                        if (inst->dst.kind != IR_OP_NONE) {
                            ir_dump_operand(inst->dst);
                            printf(" = ");
                        }

                        printf("phi ");
                        
                        for (size_t i = 0; i < inst->extra_arg_count; i += 2) {
                            ir_dump_operand(inst->extra_args[i]);

                            printf(" from ");
                            
                            ir_dump_operand(inst->extra_args[i + 1]);
                            if (i + 2 < inst->extra_arg_count) printf(", ");
                        }
                        
                        printf("\n");
                        break;
                }
            }
        }

        printf("}\n\n");
    }
}

void ir_eliminate_nops(IRFunction* func) {
    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        IRInst* prev = NULL;
        IRInst* curr = b->first_inst;

        while (curr != NULL) {
            if (curr->opcode == IR_NOP) {
                if (prev) {
                    prev->next = curr->next;
                } else {
                    b->first_inst = curr->next;
                }

                if (curr == b->last_inst) {
                    b->last_inst = prev;
                }

                b->inst_count--;
                curr = curr->next;
            } else {
                prev = curr;
                curr = curr->next;
            }
        }
    }
}