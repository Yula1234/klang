#include "stack_color.h"

#include <string.h>
#include <stdlib.h>
#include <assert.h>

typedef struct SlotState {
    IRStackSlot* slot;
    int32_t      new_offset;
    uint32_t     start_inst;
    uint32_t     end_inst;
    bool         is_active;
    bool         is_escaped;
} SlotState;

typedef struct StackPackCtx {
    Arena*       arena;
    IRFunction*  func;

    SlotState*   states;
    size_t       state_count;

    uint32_t*    block_start_idx;
    uint32_t*    block_end_idx;
    size_t       block_count;
} StackPackCtx;

static SlotState* find_state_for_offset(StackPackCtx* ctx, int32_t stack_offset) {
    if (stack_offset >= 0) {
        return NULL;
    }

    for (size_t i = 0; i < ctx->state_count; ++i) {
        SlotState* st = &ctx->states[i];
        int32_t base = st->slot->old_offset;
        int32_t end  = base + (int32_t)st->slot->size;

        if (stack_offset >= base && stack_offset < end) {
            return st;
        }
    }

    return NULL;
}

static void mark_slot_access(SlotState* st, uint32_t inst_idx) {
    if (!st) {
        return;
    }

    if (!st->is_active || inst_idx < st->start_inst) {
        st->start_inst = inst_idx;
    }

    if (!st->is_active || inst_idx > st->end_inst) {
        st->end_inst = inst_idx;
    }

    st->is_active = true;
}

static void check_and_mark_operand(StackPackCtx* ctx, const IROperand* op, uint32_t inst_idx) {
    if (!op || op->kind != IR_OP_STACK || op->stack_offset >= 0) {
        return;
    }

    SlotState* st = find_state_for_offset(ctx, op->stack_offset);

    if (st != NULL) {
        mark_slot_access(st, inst_idx);
    }
}

static void extend_liveness_for_backedge(StackPackCtx* ctx, uint32_t loop_start, uint32_t loop_end) {
    for (size_t i = 0; i < ctx->state_count; ++i) {
        SlotState* st = &ctx->states[i];

        if (!st->is_active) {
            continue;
        }

        if (st->start_inst <= loop_end && st->end_inst >= loop_start) {
            if (st->end_inst < loop_end) {
                st->end_inst = loop_end;
            }

            if (st->start_inst > loop_start) {
                st->start_inst = loop_start;
            }
        }
    }
}

static int compare_slots_for_packing(const void* a, const void* b) {
    const SlotState* sa = *(const SlotState**)a;
    const SlotState* sb = *(const SlotState**)b;

    if (sa->start_inst != sb->start_inst) {
        return (sa->start_inst < sb->start_inst) ? -1 : 1;
    }

    if (sa->slot->align != sb->slot->align) {
        return (sa->slot->align > sb->slot->align) ? -1 : 1;
    }

    if (sa->slot->size != sb->slot->size) {
        return (sa->slot->size > sb->slot->size) ? -1 : 1;
    }

    return 0;
}

static void analyze_stack_liveness(StackPackCtx* ctx) {
    IRFunction* func = ctx->func;
    uint32_t inst_idx = 0;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        if (b->id < ctx->block_count) {
            ctx->block_start_idx[b->id] = inst_idx + 2;
        }

        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP) {
                continue;
            }

            inst_idx += 2;

            if (inst->opcode == IR_ADDR && inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                SlotState* st = find_state_for_offset(ctx, inst->src1.stack_offset);

                if (st != NULL) {
                    st->is_escaped = true;
                    mark_slot_access(st, inst_idx);
                }
            }

            if (inst->opcode == IR_VA_START && inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                SlotState* st = find_state_for_offset(ctx, inst->src1.stack_offset);

                if (st != NULL) {
                    st->is_escaped = true;
                    mark_slot_access(st, inst_idx);
                }
            }

            check_and_mark_operand(ctx, &inst->dst, inst_idx);
            check_and_mark_operand(ctx, &inst->src1, inst_idx);
            check_and_mark_operand(ctx, &inst->src2, inst_idx);

            for (size_t a = 0; a < inst->extra_arg_count; ++a) {
                check_and_mark_operand(ctx, &inst->extra_args[a], inst_idx);
            }

            for (size_t a = 0; a < inst->asm_input_count; ++a) {
                check_and_mark_operand(ctx, &inst->asm_inputs[a].val, inst_idx);
            }
        }

        if (b->id < ctx->block_count) {
            ctx->block_end_idx[b->id] = inst_idx;
        }
    }

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        if (b->id >= ctx->block_count) {
            continue;
        }

        IRInst* term = b->last_inst;

        if (!term) {
            continue;
        }

        if (term->opcode == IR_JMP && term->dst.kind == IR_OP_BLOCK && term->dst.block) {
            uint32_t tid = term->dst.block->id;

            if (tid < ctx->block_count && ctx->block_start_idx[tid] <= ctx->block_start_idx[b->id]) {
                extend_liveness_for_backedge(ctx, ctx->block_start_idx[tid], ctx->block_end_idx[b->id]);
            }
        } else if (term->opcode == IR_BR) {
            if (term->src1.kind == IR_OP_BLOCK && term->src1.block) {
                uint32_t tid = term->src1.block->id;

                if (tid < ctx->block_count && ctx->block_start_idx[tid] <= ctx->block_start_idx[b->id]) {
                    extend_liveness_for_backedge(ctx, ctx->block_start_idx[tid], ctx->block_end_idx[b->id]);
                }
            }

            if (term->src2.kind == IR_OP_BLOCK && term->src2.block) {
                uint32_t tid = term->src2.block->id;

                if (tid < ctx->block_count && ctx->block_start_idx[tid] <= ctx->block_start_idx[b->id]) {
                    extend_liveness_for_backedge(ctx, ctx->block_start_idx[tid], ctx->block_end_idx[b->id]);
                }
            }
        }
    }

    for (size_t i = 0; i < ctx->state_count; ++i) {
        SlotState* st = &ctx->states[i];

        if (st->is_escaped && st->is_active) {
            st->end_inst = inst_idx;
        }
    }
}

static void pack_active_slots(StackPackCtx* ctx) {
    size_t active_count = 0;

    for (size_t i = 0; i < ctx->state_count; ++i) {
        if (ctx->states[i].is_active) {
            active_count++;
        }
    }

    if (active_count == 0) {
        ctx->func->stack_frame_size = 0;
        return;
    }

    SlotState** sorted = ARENA_NEW_ARRAY(ctx->arena, SlotState*, active_count);
    size_t s_idx = 0;

    for (size_t i = 0; i < ctx->state_count; ++i) {
        if (ctx->states[i].is_active) {
            sorted[s_idx++] = &ctx->states[i];
        }
    }

    qsort(sorted, active_count, sizeof(SlotState*), compare_slots_for_packing);

    size_t max_total_frame = 0;

    for (size_t i = 0; i < active_count; ++i) {
        SlotState* curr = sorted[i];

        size_t align = curr->slot->align ? curr->slot->align : 8;
        size_t sz    = curr->slot->size ? curr->slot->size : 8;

        size_t candidate_depth = 0;
        bool   placed          = false;

        while (!placed) {
            candidate_depth = (candidate_depth + align - 1) & ~(align - 1);

            int32_t cand_low  = -(int32_t)(candidate_depth + sz);
            int32_t cand_high = -(int32_t)candidate_depth;

            bool collision = false;

            for (size_t j = 0; j < i; ++j) {
                SlotState* prev = sorted[j];

                bool time_overlap = !(prev->end_inst <= curr->start_inst || curr->end_inst <= prev->start_inst);

                if (time_overlap) {
                    int32_t prev_low  = prev->new_offset;
                    int32_t prev_high = prev->new_offset + (int32_t)prev->slot->size;

                    bool space_overlap = (cand_low < prev_high && cand_high > prev_low);

                    if (space_overlap) {
                        collision       = true;
                        candidate_depth = (size_t)(-prev_low);
                        break;
                    }
                }
            }

            if (!collision) {
                curr->new_offset = cand_low;
                placed           = true;

                size_t frame_end = candidate_depth + sz;

                if (frame_end > max_total_frame) {
                    max_total_frame = frame_end;
                }
            }
        }
    }

    if (active_count < ctx->state_count && ctx->func->stack_frame_size > max_total_frame) {
        max_total_frame = ctx->func->stack_frame_size;
    }

    ctx->func->stack_frame_size = (max_total_frame + 15) & ~15;
}

static void rewrite_operand(StackPackCtx* ctx, IROperand* op) {
    if (!op || op->kind != IR_OP_STACK || op->stack_offset >= 0) {
        return;
    }

    SlotState* st = find_state_for_offset(ctx, op->stack_offset);

    if (st != NULL && st->is_active && st->new_offset != 0) {
        int32_t offset_in_slot = op->stack_offset - st->slot->old_offset;
        op->stack_offset       = st->new_offset + offset_in_slot;
    }
}

static void rewrite_function_operands(StackPackCtx* ctx) {
    IRFunction* func = ctx->func;

    if (func->is_variadic && func->reg_save_slot < 0) {
        SlotState* st = find_state_for_offset(ctx, func->reg_save_slot);

        if (st != NULL && st->is_active && st->new_offset != 0) {
            int32_t offset_in_slot = func->reg_save_slot - st->slot->old_offset;
            func->reg_save_slot    = st->new_offset + offset_in_slot;
        }
    }

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP) {
                continue;
            }

            rewrite_operand(ctx, &inst->dst);
            rewrite_operand(ctx, &inst->src1);
            rewrite_operand(ctx, &inst->src2);

            for (size_t a = 0; a < inst->extra_arg_count; ++a) {
                rewrite_operand(ctx, &inst->extra_args[a]);
            }

            for (size_t a = 0; a < inst->asm_input_count; ++a) {
                rewrite_operand(ctx, &inst->asm_inputs[a].val);
            }
        }
    }
}

void stack_color_run_on_function(Arena* arena, IRFunction* func) {
    if (!func || !func->first_block || func->stack_slot_count == 0) {
        if (func) {
            func->stack_frame_size = 0;
        }
        return;
    }

    size_t block_count = func->next_block_id + 1;
    size_t slot_count  = func->stack_slot_count;

    SlotState* states = ARENA_NEW_ARRAY_ZERO(arena, SlotState, slot_count);

    for (size_t i = 0; i < slot_count; ++i) {
        states[i].slot       = &func->stack_slots[i];
        states[i].new_offset = 0;
        states[i].start_inst = 0;
        states[i].end_inst   = 0;
        states[i].is_active  = false;
        states[i].is_escaped = false;
    }

    StackPackCtx ctx = {
        .arena           = arena,
        .func            = func,
        .states          = states,
        .state_count     = slot_count,
        .block_start_idx = ARENA_NEW_ARRAY_ZERO(arena, uint32_t, block_count),
        .block_end_idx   = ARENA_NEW_ARRAY_ZERO(arena, uint32_t, block_count),
        .block_count     = block_count
    };

    analyze_stack_liveness(&ctx);
    pack_active_slots(&ctx);
    rewrite_function_operands(&ctx);
}

void stack_color_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        stack_color_run_on_function(arena, f);
    }
}