#include "stack_color.h"

#include <string.h>
#include <stdlib.h>
#include <assert.h>

typedef struct StackObject {
    int32_t  old_base;
    size_t   total_size;
    size_t   align;
    int32_t  new_base;

    uint32_t start_inst;
    uint32_t end_inst;

    bool     escaped;
    bool     is_active;
} StackObject;

typedef struct StackColorCtx {
    Arena*        arena;
    IRFunction*   func;

    StackObject*  objects;
    size_t        object_count;
    size_t        object_cap;

    uint32_t*     block_start_idx;
    uint32_t*     block_end_idx;
    size_t        block_count;
} StackColorCtx;

static StackObject* find_or_add_object(StackColorCtx* ctx, int32_t base_offset, size_t size, size_t align) {
    if (size == 0) {
        size = 8;
    }

    if (align == 0) {
        align = (size >= 16) ? 16 : ((size >= 8) ? 8 : (size >= 4 ? 4 : (size >= 2 ? 2 : 1)));
    }

    for (size_t i = 0; i < ctx->object_count; ++i) {
        if (ctx->objects[i].old_base == base_offset) {
            if (size > ctx->objects[i].total_size) {
                ctx->objects[i].total_size = size;
            }

            if (align > ctx->objects[i].align) {
                ctx->objects[i].align = align;
            }

            return &ctx->objects[i];
        }
    }

    StackObject obj = {
        .old_base   = base_offset,
        .total_size = size,
        .align      = align,
        .new_base   = 0,
        .start_inst = 0,
        .end_inst   = 0,
        .escaped    = false,
        .is_active  = false
    };

    ARENA_DA_PUSH(ctx->arena, ctx->objects, ctx->object_count, ctx->object_cap, obj);

    return &ctx->objects[ctx->object_count - 1];
}

static StackObject* get_containing_object(StackColorCtx* ctx, int32_t stack_offset) {
    for (size_t i = 0; i < ctx->object_count; ++i) {
        int32_t base = ctx->objects[i].old_base;
        int32_t end  = base + (int32_t)ctx->objects[i].total_size;

        if (stack_offset >= base && stack_offset < end) {
            return &ctx->objects[i];
        }
    }

    return NULL;
}

static void track_object_access(StackObject* obj, uint32_t inst_idx) {
    if (!obj) {
        return;
    }

    if (obj->start_inst == 0 || inst_idx < obj->start_inst) {
        obj->start_inst = inst_idx;
    }

    if (inst_idx > obj->end_inst) {
        obj->end_inst = inst_idx;
    }

    obj->is_active = true;
}

static void extend_liveness_for_loop(StackColorCtx* ctx, uint32_t target_start, uint32_t loop_end) {
    for (size_t i = 0; i < ctx->object_count; ++i) {
        StackObject* obj = &ctx->objects[i];

        if (!obj->is_active) {
            continue;
        }

        if (obj->start_inst <= loop_end && obj->end_inst >= target_start) {
            if (obj->end_inst < loop_end) {
                obj->end_inst = loop_end;
            }
        }
    }
}

static void handle_backedge(StackColorCtx* ctx, const IRBlock* target, const IRBlock* current_block) {
    if (target != NULL && target->id < ctx->block_count && current_block->id < ctx->block_count) {
        uint32_t target_start  = ctx->block_start_idx[target->id];
        uint32_t current_start = ctx->block_start_idx[current_block->id];

        if (target_start <= current_start) {
            uint32_t loop_end = ctx->block_end_idx[current_block->id];
            extend_liveness_for_loop(ctx, target_start, loop_end);
        }
    }
}

static int compare_objects_by_start(const void* a, const void* b) {
    const StackObject* oa = *(const StackObject**)a;
    const StackObject* ob = *(const StackObject**)b;

    if (oa->start_inst != ob->start_inst) {
        return (oa->start_inst < ob->start_inst) ? -1 : 1;
    }

    return (oa->end_inst < ob->end_inst) ? -1 : 1;
}

static void collect_stack_objects(StackColorCtx* ctx) {
    IRFunction* func = ctx->func;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP) {
                continue;
            }

            if (inst->opcode == IR_ADDR && inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                find_or_add_object(ctx, inst->src1.stack_offset, inst->src1.byte_size, 0);
            }

            if (inst->opcode == IR_MEMCPY) {
                size_t cpy_size = (inst->src2.kind == IR_OP_CONST) ? (size_t)inst->src2.int_val : 8;

                if (inst->dst.kind == IR_OP_STACK && inst->dst.stack_offset < 0) {
                    find_or_add_object(ctx, inst->dst.stack_offset, cpy_size, 0);
                }

                if (inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                    find_or_add_object(ctx, inst->src1.stack_offset, cpy_size, 0);
                }
            }

            if (inst->dst.kind == IR_OP_STACK && inst->dst.stack_offset < 0) {
                if (!get_containing_object(ctx, inst->dst.stack_offset)) {
                    find_or_add_object(ctx, inst->dst.stack_offset, inst->dst.byte_size, 0);
                }
            }

            if (inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                if (!get_containing_object(ctx, inst->src1.stack_offset)) {
                    find_or_add_object(ctx, inst->src1.stack_offset, inst->src1.byte_size, 0);
                }
            }

            if (inst->src2.kind == IR_OP_STACK && inst->src2.stack_offset < 0) {
                if (!get_containing_object(ctx, inst->src2.stack_offset)) {
                    find_or_add_object(ctx, inst->src2.stack_offset, inst->src2.byte_size, 0);
                }
            }

            for (size_t a = 0; a < inst->extra_arg_count; ++a) {
                if (inst->extra_args[a].kind == IR_OP_STACK && inst->extra_args[a].stack_offset < 0) {
                    if (!get_containing_object(ctx, inst->extra_args[a].stack_offset)) {
                        find_or_add_object(ctx, inst->extra_args[a].stack_offset, inst->extra_args[a].byte_size, 0);
                    }
                }
            }

            for (size_t a = 0; a < inst->asm_input_count; ++a) {
                if (inst->asm_inputs[a].val.kind == IR_OP_STACK && inst->asm_inputs[a].val.stack_offset < 0) {
                    if (!get_containing_object(ctx, inst->asm_inputs[a].val.stack_offset)) {
                        find_or_add_object(ctx, inst->asm_inputs[a].val.stack_offset, inst->asm_inputs[a].val.byte_size, 0);
                    }
                }
            }
        }
    }
}

static void compute_stack_liveness(StackColorCtx* ctx) {
    IRFunction* func = ctx->func;
    uint32_t inst_idx = 0;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        if (b->id < ctx->block_count) {
            ctx->block_start_idx[b->id] = inst_idx + 2;
        }

        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            inst_idx += 2;

            if (inst->opcode == IR_ADDR && inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                StackObject* obj = get_containing_object(ctx, inst->src1.stack_offset);
                track_object_access(obj, inst_idx);
                if (obj) obj->escaped = true;
            }

            if (inst->opcode == IR_MEMCPY) {
                if (inst->dst.kind == IR_OP_STACK && inst->dst.stack_offset < 0) {
                    StackObject* obj = get_containing_object(ctx, inst->dst.stack_offset);
                    track_object_access(obj, inst_idx);
                }

                if (inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                    StackObject* obj = get_containing_object(ctx, inst->src1.stack_offset);
                    track_object_access(obj, inst_idx);
                }
            }

            if (inst->dst.kind == IR_OP_STACK && inst->dst.stack_offset < 0) {
                StackObject* obj = get_containing_object(ctx, inst->dst.stack_offset);
                track_object_access(obj, inst_idx);
            }

            if (inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                StackObject* obj = get_containing_object(ctx, inst->src1.stack_offset);
                track_object_access(obj, inst_idx);
            }

            if (inst->src2.kind == IR_OP_STACK && inst->src2.stack_offset < 0) {
                StackObject* obj = get_containing_object(ctx, inst->src2.stack_offset);
                track_object_access(obj, inst_idx);
            }

            for (size_t a = 0; a < inst->extra_arg_count; ++a) {
                if (inst->extra_args[a].kind == IR_OP_STACK && inst->extra_args[a].stack_offset < 0) {
                    StackObject* obj = get_containing_object(ctx, inst->extra_args[a].stack_offset);
                    track_object_access(obj, inst_idx);
                }
            }

            for (size_t a = 0; a < inst->asm_input_count; ++a) {
                if (inst->asm_inputs[a].val.kind == IR_OP_STACK && inst->asm_inputs[a].val.stack_offset < 0) {
                    StackObject* obj = get_containing_object(ctx, inst->asm_inputs[a].val.stack_offset);
                    track_object_access(obj, inst_idx);
                }
            }
        }

        if (b->id < ctx->block_count) {
            ctx->block_end_idx[b->id] = inst_idx;
        }
    }

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_JMP && inst->dst.kind == IR_OP_BLOCK) {
                handle_backedge(ctx, inst->dst.block, b);
            } else if (inst->opcode == IR_BR) {
                if (inst->src1.kind == IR_OP_BLOCK) handle_backedge(ctx, inst->src1.block, b);
                if (inst->src2.kind == IR_OP_BLOCK) handle_backedge(ctx, inst->src2.block, b);
            }
        }
    }

    for (size_t i = 0; i < ctx->object_count; ++i) {
        StackObject* obj = &ctx->objects[i];

        if (obj->escaped) {
            obj->end_inst = inst_idx;
        }
    }
}

static void color_and_pack_slots(StackColorCtx* ctx) {
    if (ctx->object_count == 0) {
        ctx->func->stack_frame_size = 0;
        return;
    }

    size_t active_count = 0;
    for (size_t i = 0; i < ctx->object_count; ++i) {
        if (ctx->objects[i].is_active) {
            active_count++;
        }
    }

    if (active_count == 0) {
        ctx->func->stack_frame_size = 0;
        return;
    }

    StackObject** sorted = ARENA_NEW_ARRAY(ctx->arena, StackObject*, active_count);
    size_t sort_idx = 0;

    for (size_t i = 0; i < ctx->object_count; ++i) {
        if (ctx->objects[i].is_active) {
            sorted[sort_idx++] = &ctx->objects[i];
        }
    }

    qsort(sorted, active_count, sizeof(StackObject*), compare_objects_by_start);

    size_t max_total_frame = 0;

    for (size_t i = 0; i < active_count; ++i) {
        StackObject* curr = sorted[i];

        size_t align = curr->align ? curr->align : 8;
        size_t sz    = curr->total_size ? curr->total_size : 8;

        size_t candidate_depth = 0;
        bool   found_slot      = false;

        while (!found_slot) {
            candidate_depth = (candidate_depth + align - 1) & ~(align - 1);

            int32_t cand_low  = -(int32_t)(candidate_depth + sz);
            int32_t cand_high = -(int32_t)candidate_depth;

            bool collision = false;

            for (size_t j = 0; j < i; ++j) {
                StackObject* prev = sorted[j];

                bool intervals_overlap = !(prev->end_inst <= curr->start_inst || curr->end_inst <= prev->start_inst);

                if (intervals_overlap) {
                    int32_t prev_low  = prev->new_base;
                    int32_t prev_high = prev->new_base + (int32_t)prev->total_size;

                    bool ranges_overlap = (cand_low < prev_high && cand_high > prev_low);

                    if (ranges_overlap) {
                        collision = true;
                        candidate_depth = (size_t)(-prev_low);
                        break;
                    }
                }
            }

            if (!collision) {
                curr->new_base   = cand_low;
                found_slot       = true;

                size_t frame_end = candidate_depth + sz;

                if (frame_end > max_total_frame) {
                    max_total_frame = frame_end;
                }
            }
        }
    }

    ctx->func->stack_frame_size = (max_total_frame + 15) & ~15;
}

static void rewrite_stack_operand(StackColorCtx* ctx, IROperand* op) {
    if (!op || op->kind != IR_OP_STACK || op->stack_offset >= 0) {
        return;
    }

    StackObject* obj = get_containing_object(ctx, op->stack_offset);

    if (obj != NULL && obj->new_base != 0) {
        int32_t offset_in_obj = op->stack_offset - obj->old_base;
        op->stack_offset      = obj->new_base + offset_in_obj;
    }
}

static void rewrite_function_stack_operands(StackColorCtx* ctx) {
    IRFunction* func = ctx->func;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP) {
                continue;
            }

            rewrite_stack_operand(ctx, &inst->dst);
            rewrite_stack_operand(ctx, &inst->src1);
            rewrite_stack_operand(ctx, &inst->src2);

            for (size_t a = 0; a < inst->extra_arg_count; ++a) {
                rewrite_stack_operand(ctx, &inst->extra_args[a]);
            }

            for (size_t a = 0; a < inst->asm_input_count; ++a) {
                rewrite_stack_operand(ctx, &inst->asm_inputs[a].val);
            }
        }
    }
}

void stack_color_run_on_function(Arena* arena, IRFunction* func) {
    if (!func || !func->first_block) {
        return;
    }

    size_t block_count = func->next_block_id + 1;

    StackColorCtx ctx = {
        .arena           = arena,
        .func            = func,
        .objects         = NULL,
        .object_count    = 0,
        .object_cap      = 0,
        .block_start_idx = ARENA_NEW_ARRAY_ZERO(arena, uint32_t, block_count),
        .block_end_idx   = ARENA_NEW_ARRAY_ZERO(arena, uint32_t, block_count),
        .block_count     = block_count
    };

    collect_stack_objects(&ctx);
    compute_stack_liveness(&ctx);
    color_and_pack_slots(&ctx);
    rewrite_function_stack_operands(&ctx);
}

void stack_color_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        stack_color_run_on_function(arena, f);
    }
}