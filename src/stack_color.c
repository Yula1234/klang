#include "stack_color.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STACK_COLOR_MAX_SUCCESSORS 2


typedef struct BitMatrix {
    uint64_t* words;
    size_t    row_count;
    size_t    word_count;
} BitMatrix;


typedef struct BlockPreds {
    uint32_t* ids;
    size_t    count;
    size_t    cap;
} BlockPreds;


typedef struct StackSlotState {
    IRStackSlot* slot;
    int32_t      new_offset;
    size_t       depth;
    size_t       storage_size;
    bool         active;
    bool         escaped;
} StackSlotState;


typedef struct StackInstructionAccesses {
    uint32_t* accesses;
    size_t    access_count;
    uint32_t* defs;
    size_t    def_count;
    uint32_t* uses;
    size_t    use_count;
} StackInstructionAccesses;


typedef struct StackColorCtx {
    Arena*           arena;
    IRFunction*      func;
    size_t           block_count;
    size_t           slot_count;
    size_t           slot_word_count;

    IRBlock**        blocks;
    IRInst**         instructions;
    size_t*          block_inst_begin;
    size_t*          block_inst_end;

    uint32_t*        slot_lookup_order;
    uint32_t*        color_order;
    size_t*          conflict_degree;
    StackSlotState*  states;

    BitMatrix        block_use;
    BitMatrix        block_def;
    BitMatrix        live_in;
    BitMatrix        conflicts;

    BlockPreds*      predecessors;
    bool*            queued;
    uint32_t*        worklist;
    size_t           worklist_count;
    size_t           worklist_cap;

    uint32_t*        access_scratch;
    uint32_t*        def_scratch;
    uint32_t*        use_scratch;
    size_t           scratch_capacity;
} StackColorCtx;


static BitMatrix bitmatrix_create(Arena* arena, size_t row_count, size_t bit_count) {
    BitMatrix matrix = {
        .words      = NULL,
        .row_count  = row_count,
        .word_count = (bit_count + 63) / 64
    };

    if (row_count == 0 || matrix.word_count == 0) {
        return matrix;
    }

    assert(row_count <= SIZE_MAX / matrix.word_count);
    size_t total_words = row_count * matrix.word_count;

    assert(total_words <= SIZE_MAX / sizeof(uint64_t));
    matrix.words = ARENA_NEW_ARRAY_ZERO(arena, uint64_t, total_words);

    return matrix;
}


static inline uint64_t* bitmatrix_row(BitMatrix* matrix, size_t row) {
    assert(row < matrix->row_count);
    return matrix->words + row * matrix->word_count;
}


static inline const uint64_t* bitmatrix_const_row(const BitMatrix* matrix, size_t row) {
    assert(row < matrix->row_count);
    return matrix->words + row * matrix->word_count;
}


static inline bool bitmatrix_rows_equal(
    const BitMatrix* matrix,
    size_t lhs_row,
    const uint64_t* rhs
) {
    return memcmp(
        bitmatrix_const_row(matrix, lhs_row),
        rhs,
        matrix->word_count * sizeof(uint64_t)
    ) == 0;
}


static inline void bitmatrix_copy_row(
    BitMatrix* matrix,
    size_t row,
    const uint64_t* src
) {
    memcpy(
        bitmatrix_row(matrix, row),
        src,
        matrix->word_count * sizeof(uint64_t)
    );
}


static inline void bitmatrix_or_into(
    uint64_t* dst,
    const uint64_t* src,
    size_t word_count
) {
    for (size_t i = 0; i < word_count; ++i) {
        dst[i] |= src[i];
    }
}


static inline void bitmatrix_set(BitMatrix* matrix, size_t row, size_t bit) {
    assert(row < matrix->row_count);
    assert(bit < matrix->word_count * 64);

    bitmatrix_row(matrix, row)[bit / 64] |= UINT64_C(1) << (bit % 64);
}


static inline bool bitmatrix_test(const BitMatrix* matrix, size_t row, size_t bit) {
    assert(row < matrix->row_count);
    assert(bit < matrix->word_count * 64);

    return (bitmatrix_const_row(matrix, row)[bit / 64] &
            (UINT64_C(1) << (bit % 64))) != 0;
}


static inline void bitmatrix_add_conflict(BitMatrix* matrix, uint32_t lhs, uint32_t rhs) {
    assert(lhs < matrix->row_count);
    assert(rhs < matrix->row_count);

    if (lhs == rhs) {
        return;
    }

    bitmatrix_set(matrix, lhs, rhs);
    bitmatrix_set(matrix, rhs, lhs);
}


static StackSlotState* find_state_for_offset(
    const StackColorCtx* ctx,
    int32_t stack_offset
) {
    if (stack_offset >= 0 || ctx->slot_count == 0) {
        return NULL;
    }

    size_t lo = 0;
    size_t hi = ctx->slot_count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        uint32_t slot_id = ctx->slot_lookup_order[mid];
        int32_t base = ctx->states[slot_id].slot->old_offset;

        if (base <= stack_offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo == 0) {
        return NULL;
    }

    uint32_t slot_id = ctx->slot_lookup_order[lo - 1];

    StackSlotState* state = &ctx->states[slot_id];
    
    int64_t base = state->slot->old_offset;
    int64_t end  = base + (int64_t)state->slot->size;

    if ((int64_t)stack_offset < base || (int64_t)stack_offset >= end) {
        return NULL;
    }

    return state;
}


static int compare_slot_offsets(
    const void* lhs_ptr,
    const void* rhs_ptr,
    void* opaque
) {
    const StackColorCtx* ctx = opaque;
    uint32_t lhs = *(const uint32_t*)lhs_ptr;
    uint32_t rhs = *(const uint32_t*)rhs_ptr;

    int32_t lhs_offset = ctx->states[lhs].slot->old_offset;
    int32_t rhs_offset = ctx->states[rhs].slot->old_offset;

    if (lhs_offset != rhs_offset) {
        return lhs_offset < rhs_offset ? -1 : 1;
    }

    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}


static int compare_color_order(
    const void* lhs_ptr,
    const void* rhs_ptr,
    void* opaque
) {
    const StackColorCtx* ctx = opaque;
    uint32_t lhs = *(const uint32_t*)lhs_ptr;
    uint32_t rhs = *(const uint32_t*)rhs_ptr;

    const StackSlotState* a = &ctx->states[lhs];
    const StackSlotState* b = &ctx->states[rhs];

    if (a->escaped != b->escaped) {
        return a->escaped ? -1 : 1;
    }

    if (a->slot->size != b->slot->size) {
        return a->slot->size > b->slot->size ? -1 : 1;
    }

    if (a->slot->align != b->slot->align) {
        return a->slot->align > b->slot->align ? -1 : 1;
    }

    if (ctx->conflict_degree[lhs] != ctx->conflict_degree[rhs]) {
        return ctx->conflict_degree[lhs] > ctx->conflict_degree[rhs] ? -1 : 1;
    }

    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}


static size_t ir_stack_successors(
    const IRBlock* block,
    IRBlock* successors[STACK_COLOR_MAX_SUCCESSORS]
) {
    assert(block != NULL);
    assert(successors != NULL);

    IRInst* term = block->last_inst;

    if (!term) {
        if (block->next_block) {
            successors[0] = block->next_block;
            return 1;
        }

        return 0;
    }

    if (term->opcode == IR_JMP &&
        term->dst.kind == IR_OP_BLOCK &&
        term->dst.block != NULL) {
        successors[0] = term->dst.block;
        return 1;
    }

    if (term->opcode == IR_BR) {
        size_t count = 0;

        if (term->src1.kind == IR_OP_BLOCK && term->src1.block != NULL) {
            successors[count++] = term->src1.block;
        }

        if (term->src2.kind == IR_OP_BLOCK &&
            term->src2.block != NULL &&
            (count == 0 || successors[0] != term->src2.block)) {
            successors[count++] = term->src2.block;
        }

        return count;
    }

    if (term->opcode == IR_RET ||
        term->opcode == IR_TAIL_CALL ||
        term->opcode == IR_TAIL_CALL_PTR) {
        return 0;
    }

    if (block->next_block) {
        successors[0] = block->next_block;
        return 1;
    }

    return 0;
}


static void build_cfg_edges(StackColorCtx* ctx) {
    for (size_t block_id = 0; block_id < ctx->block_count; ++block_id) {
        IRBlock* block = ctx->blocks[block_id];

        if (block == NULL) {
            continue;
        }

        IRBlock* successors[STACK_COLOR_MAX_SUCCESSORS];
        size_t successor_count = ir_stack_successors(block, successors);

        for (size_t i = 0; i < successor_count; ++i) {
            uint32_t successor_id = successors[i]->id;

            assert(successor_id < ctx->block_count);
            assert(ctx->blocks[successor_id] == successors[i]);

            BlockPreds* preds = &ctx->predecessors[successor_id];
            bool duplicate = false;

            for (size_t p = 0; p < preds->count; ++p) {
                if (preds->ids[p] == (uint32_t)block_id) {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate) {
                ARENA_DA_PUSH(
                    ctx->arena,
                    preds->ids,
                    preds->count,
                    preds->cap,
                    (uint32_t)block_id
                );
            }
        }
    }
}


static bool stack_operand_is_read(const IRInst* inst, size_t operand_index) {
    switch (inst->opcode) {
        case IR_MOV:
        case IR_LOAD:
        case IR_GLOBAL_STR:
        case IR_ALLOCA:
            return operand_index == 1;

        case IR_STORE:
            return operand_index == 0 || operand_index == 1;

        case IR_MEMCPY:
            return operand_index == 1 || operand_index == 2;

        case IR_ADDR:
        case IR_PARAM:
        case IR_VA_START:
            return false;

        case IR_CALL:
        case IR_CALL_PTR:
        case IR_TAIL_CALL:
        case IR_TAIL_CALL_PTR:
            return operand_index == 1 || operand_index == 2;

        case IR_VA_ARG:
            return operand_index == 1;

        case IR_VA_END:
            return false;

        case IR_VA_COPY:
            return true;

        case IR_BR:
        case IR_RET:
            return true;

        default:
            return operand_index != 0;
    }
}


static bool stack_operand_is_write(const IRInst* inst, size_t operand_index) {
    switch (inst->opcode) {
        case IR_MOV:
        case IR_LOAD:
        case IR_GLOBAL_STR:
        case IR_ALLOCA:
        case IR_MEMCPY:
        case IR_CALL:
        case IR_CALL_PTR:
        case IR_TAIL_CALL:
        case IR_TAIL_CALL_PTR:
        case IR_PARAM:
        case IR_VA_START:
        case IR_VA_ARG:
            return operand_index == 0;

        case IR_STORE:
        case IR_ADDR:
        case IR_VA_END:
        case IR_VA_COPY:
            return false;

        default:
            return operand_index == 0;
    }
}


static bool stack_operand_escapes(const IRInst* inst, size_t operand_index) {
    if (inst->opcode == IR_ADDR && operand_index == 1) {
        return true;
    }

    if (inst->opcode == IR_VA_START) {
        return operand_index == 0 || operand_index == 1;
    }

    return false;
}


static void access_add_unique(uint32_t* items, size_t* count, uint32_t value) {
    for (size_t i = 0; i < *count; ++i) {
        if (items[i] == value) {
            return;
        }
    }

    items[(*count)++] = value;
}


static void collect_instruction_accesses(
    StackColorCtx* ctx,
    const IRInst* inst,
    StackInstructionAccesses* accesses
) {
    accesses->access_count = 0;
    accesses->def_count    = 0;
    accesses->use_count    = 0;

    const IROperand* operands[3] = {
        &inst->dst,
        &inst->src1,
        &inst->src2
    };

    for (size_t i = 0; i < 3; ++i) {
        const IROperand* operand = operands[i];

        if (operand->kind != IR_OP_STACK || operand->stack_offset >= 0) {
            continue;
        }

        StackSlotState* state = find_state_for_offset(ctx, operand->stack_offset);
        assert(state != NULL);

        uint32_t slot_id = state->slot->id;
        state->active = true;

        access_add_unique(
            accesses->accesses,
            &accesses->access_count,
            slot_id
        );

        if (stack_operand_is_read(inst, i)) {
            access_add_unique(
                accesses->uses,
                &accesses->use_count,
                slot_id
            );
        }

        if (stack_operand_is_write(inst, i)) {
            access_add_unique(
                accesses->defs,
                &accesses->def_count,
                slot_id
            );
        }

        if (stack_operand_escapes(inst, i)) {
            state->escaped = true;
        }
    }

    for (size_t i = 0; i < inst->extra_arg_count; ++i) {
        const IROperand* operand = &inst->extra_args[i];

        if (operand->kind != IR_OP_STACK || operand->stack_offset >= 0) {
            continue;
        }

        StackSlotState* state = find_state_for_offset(ctx, operand->stack_offset);
        assert(state != NULL);

        uint32_t slot_id = state->slot->id;
        state->active = true;

        access_add_unique(
            accesses->accesses,
            &accesses->access_count,
            slot_id
        );
        access_add_unique(
            accesses->uses,
            &accesses->use_count,
            slot_id
        );
    }

    for (size_t i = 0; i < inst->asm_input_count; ++i) {
        const IROperand* operand = &inst->asm_inputs[i].val;

        if (operand->kind != IR_OP_STACK || operand->stack_offset >= 0) {
            continue;
        }

        StackSlotState* state = find_state_for_offset(ctx, operand->stack_offset);
        assert(state != NULL);

        uint32_t slot_id = state->slot->id;
        state->active = true;

        access_add_unique(
            accesses->accesses,
            &accesses->access_count,
            slot_id
        );
        access_add_unique(
            accesses->uses,
            &accesses->use_count,
            slot_id
        );
    }
}


static size_t compute_scratch_capacity(const IRFunction* func) {
    size_t capacity = 3;

    for (const IRBlock* block = func->first_block; block != NULL; block = block->next_block) {
        for (const IRInst* inst = block->first_inst; inst != NULL; inst = inst->next) {
            size_t count = 3 + inst->extra_arg_count + inst->asm_input_count;

            if (count > capacity) {
                capacity = count;
            }
        }
    }

    return capacity;
}


static size_t flatten_instructions(StackColorCtx* ctx) {
    size_t total = 0;

    for (size_t block_id = 0; block_id < ctx->block_count; ++block_id) {
        IRBlock* block = ctx->blocks[block_id];

        ctx->block_inst_begin[block_id] = total;

        if (block == NULL) {
            ctx->block_inst_end[block_id] = total;
            continue;
        }

        for (IRInst* inst = block->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode != IR_NOP) {
                total++;
            }
        }

        ctx->block_inst_end[block_id] = total;
    }

    if (total != 0) {
        ctx->instructions = ARENA_NEW_ARRAY(ctx->arena, IRInst*, total);

        size_t index = 0;
        for (size_t block_id = 0; block_id < ctx->block_count; ++block_id) {
            IRBlock* block = ctx->blocks[block_id];

            if (block == NULL) {
                continue;
            }

            for (IRInst* inst = block->first_inst; inst != NULL; inst = inst->next) {
                if (inst->opcode != IR_NOP) {
                    ctx->instructions[index++] = inst;
                }
            }
        }

        assert(index == total);
    }

    return total;
}


static void build_local_liveness(StackColorCtx* ctx) {
    StackInstructionAccesses accesses = {
        .accesses     = ctx->access_scratch,
        .access_count = 0,
        .defs         = ctx->def_scratch,
        .def_count    = 0,
        .uses         = ctx->use_scratch,
        .use_count    = 0
    };

    bool* defined = ARENA_NEW_ARRAY_ZERO(ctx->arena, bool, ctx->slot_count);

    for (size_t block_id = 0; block_id < ctx->block_count; ++block_id) {
        if (ctx->blocks[block_id] == NULL) {
            continue;
        }

        memset(defined, 0, ctx->slot_count * sizeof(bool));

        for (size_t i = ctx->block_inst_begin[block_id];
             i < ctx->block_inst_end[block_id];
             ++i) {
            collect_instruction_accesses(ctx, ctx->instructions[i], &accesses);

            for (size_t j = 0; j < accesses.use_count; ++j) {
                uint32_t slot_id = accesses.uses[j];

                if (!defined[slot_id]) {
                    bitmatrix_set(&ctx->block_use, block_id, slot_id);
                }
            }

            for (size_t j = 0; j < accesses.def_count; ++j) {
                uint32_t slot_id = accesses.defs[j];
                defined[slot_id] = true;
                bitmatrix_set(&ctx->block_def, block_id, slot_id);
            }
        }
    }
}


static void enqueue_block(StackColorCtx* ctx, uint32_t block_id) {
    assert(block_id < ctx->block_count);

    if (ctx->queued[block_id]) {
        return;
    }

    if (ctx->worklist_count == ctx->worklist_cap) {
        size_t old_cap = ctx->worklist_cap;
        size_t new_cap = old_cap == 0 ? 16 : old_cap * 2;

        assert(new_cap > old_cap);
        assert(new_cap <= SIZE_MAX / sizeof(uint32_t));

        ctx->worklist = arena_realloc(
            ctx->arena,
            ctx->worklist,
            old_cap * sizeof(uint32_t),
            new_cap * sizeof(uint32_t)
        );
        ctx->worklist_cap = new_cap;
    }

    ctx->queued[block_id] = true;
    ctx->worklist[ctx->worklist_count++] = block_id;
}


static uint32_t dequeue_block(StackColorCtx* ctx) {
    assert(ctx->worklist_count != 0);

    uint32_t block_id = ctx->worklist[--ctx->worklist_count];
    ctx->queued[block_id] = false;
    return block_id;
}


static void compute_block_live_out(
    StackColorCtx* ctx,
    uint32_t block_id,
    uint64_t* live_out
) {
    memset(live_out, 0, ctx->slot_word_count * sizeof(uint64_t));

    IRBlock* block = ctx->blocks[block_id];
    assert(block != NULL);

    IRBlock* successors[STACK_COLOR_MAX_SUCCESSORS];
    size_t successor_count = ir_stack_successors(block, successors);

    for (size_t i = 0; i < successor_count; ++i) {
        uint32_t successor_id = successors[i]->id;
        assert(successor_id < ctx->block_count);

        bitmatrix_or_into(
            live_out,
            bitmatrix_const_row(&ctx->live_in, successor_id),
            ctx->slot_word_count
        );
    }
}


static void solve_stack_liveness(StackColorCtx* ctx) {
    for (size_t i = 0; i < ctx->block_count; ++i) {
        uint32_t block_id = (uint32_t)(ctx->block_count - 1 - i);

        if (ctx->blocks[block_id] != NULL) {
            enqueue_block(ctx, block_id);
        }
    }

    uint64_t* new_out = ARENA_NEW_ARRAY(ctx->arena, uint64_t, ctx->slot_word_count);
    uint64_t* new_in  = ARENA_NEW_ARRAY(ctx->arena, uint64_t, ctx->slot_word_count);

    while (ctx->worklist_count != 0) {
        uint32_t block_id = dequeue_block(ctx);

        compute_block_live_out(ctx, block_id, new_out);

        const uint64_t* use = bitmatrix_const_row(&ctx->block_use, block_id);
        const uint64_t* def = bitmatrix_const_row(&ctx->block_def, block_id);

        for (size_t word = 0; word < ctx->slot_word_count; ++word) {
            new_in[word] = use[word] | (new_out[word] & ~def[word]);
        }

        bool changed = !bitmatrix_rows_equal(&ctx->live_in, block_id, new_in);
        bitmatrix_copy_row(&ctx->live_in, block_id, new_in);

        if (!changed) {
            continue;
        }

        const BlockPreds* preds = &ctx->predecessors[block_id];

        for (size_t i = 0; i < preds->count; ++i) {
            enqueue_block(ctx, preds->ids[i]);
        }
    }
}


static void add_conflicts_with_live(
    StackColorCtx* ctx,
    uint32_t slot_id,
    const uint64_t* live
) {
    for (size_t word = 0; word < ctx->slot_word_count; ++word) {
        uint64_t bits = live[word];

        while (bits != 0) {
            unsigned bit = (unsigned)__builtin_ctzll(bits);
            uint32_t other_id = (uint32_t)(word * 64 + bit);

            if (other_id < ctx->slot_count) {
                bitmatrix_add_conflict(&ctx->conflicts, slot_id, other_id);
            }

            bits &= bits - 1;
        }
    }
}


static void build_conflict_graph(StackColorCtx* ctx) {
    StackInstructionAccesses accesses = {
        .accesses     = ctx->access_scratch,
        .access_count = 0,
        .defs         = ctx->def_scratch,
        .def_count    = 0,
        .uses         = ctx->use_scratch,
        .use_count    = 0
    };

    uint64_t* live = ARENA_NEW_ARRAY(ctx->arena, uint64_t, ctx->slot_word_count);

    for (size_t block_id = 0; block_id < ctx->block_count; ++block_id) {
        if (ctx->blocks[block_id] == NULL) {
            continue;
        }

        compute_block_live_out(ctx, (uint32_t)block_id, live);

        for (size_t i = ctx->block_inst_end[block_id];
             i > ctx->block_inst_begin[block_id];) {
            --i;
            collect_instruction_accesses(ctx, ctx->instructions[i], &accesses);

            for (size_t j = 0; j < accesses.access_count; ++j) {
                add_conflicts_with_live(ctx, accesses.accesses[j], live);
            }

            for (size_t j = 0; j < accesses.access_count; ++j) {
                for (size_t k = j + 1; k < accesses.access_count; ++k) {
                    bitmatrix_add_conflict(
                        &ctx->conflicts,
                        accesses.accesses[j],
                        accesses.accesses[k]
                    );
                }
            }

            for (size_t j = 0; j < accesses.def_count; ++j) {
                uint32_t slot_id = accesses.defs[j];
                live[slot_id / 64] &= ~(UINT64_C(1) << (slot_id % 64));
            }

            for (size_t j = 0; j < accesses.use_count; ++j) {
                uint32_t slot_id = accesses.uses[j];
                live[slot_id / 64] |= UINT64_C(1) << (slot_id % 64);
            }
        }
    }

    for (size_t i = 0; i < ctx->slot_count; ++i) {
        if (!ctx->states[i].escaped) {
            continue;
        }

        for (size_t j = 0; j < ctx->slot_count; ++j) {
            if (i != j) {
                bitmatrix_add_conflict(&ctx->conflicts, (uint32_t)i, (uint32_t)j);
            }
        }
    }
}


static void compute_conflict_degrees(StackColorCtx* ctx) {
    for (size_t slot_id = 0; slot_id < ctx->slot_count; ++slot_id) {
        const uint64_t* row = bitmatrix_const_row(&ctx->conflicts, slot_id);
        size_t degree = 0;

        for (size_t word = 0; word < ctx->slot_word_count; ++word) {
            degree += (size_t)__builtin_popcountll(row[word]);
        }

        ctx->conflict_degree[slot_id] = degree;
    }
}


static bool ranges_overlap(
    size_t lhs_begin,
    size_t lhs_size,
    size_t rhs_begin,
    size_t rhs_size
) {
    assert(lhs_begin <= SIZE_MAX - lhs_size);
    assert(rhs_begin <= SIZE_MAX - rhs_size);

    size_t lhs_end = lhs_begin + lhs_size;
    size_t rhs_end = rhs_begin + rhs_size;

    return lhs_begin < rhs_end && rhs_begin < lhs_end;
}


static void color_stack_slots(StackColorCtx* ctx) {
    size_t active_count = 0;

    for (size_t slot_id = 0; slot_id < ctx->slot_count; ++slot_id) {
        ctx->states[slot_id].new_offset = 0;
        ctx->states[slot_id].depth = 0;
        ctx->states[slot_id].storage_size = 0;

        if (ctx->states[slot_id].active) {
            ctx->color_order[active_count++] = (uint32_t)slot_id;
        }
    }

    if (active_count == 0) {
        ctx->func->stack_frame_size = 0;
        return;
    }

    compute_conflict_degrees(ctx);

    qsort_r(
        ctx->color_order,
        active_count,
        sizeof(uint32_t),
        compare_color_order,
        ctx
    );

    size_t frame_end = 0;

    for (size_t i = 0; i < active_count; ++i) {
        uint32_t slot_id = ctx->color_order[i];
        StackSlotState* state = &ctx->states[slot_id];

        size_t size = state->slot->size;
        size_t align = state->slot->align;
        size_t storage_size = (size + align - 1) & ~(align - 1);

        assert(size > 0);
        assert(align > 0);
        assert((align & (align - 1)) == 0);
        assert(storage_size >= size);

        state->storage_size = storage_size;

        size_t depth = 0;

        for (;;) {
            assert(depth <= SIZE_MAX - (align - 1));
            depth = (depth + align - 1) & ~(align - 1);
            assert(depth <= SIZE_MAX - storage_size);

            bool collision = false;
            size_t next_depth = depth + 1;

            for (size_t j = 0; j < i; ++j) {
                uint32_t other_id = ctx->color_order[j];
                const StackSlotState* other = &ctx->states[other_id];

                if (!bitmatrix_test(&ctx->conflicts, slot_id, other_id)) {
                    continue;
                }

                if (!ranges_overlap(
                        depth,
                        storage_size,
                        other->depth,
                        other->storage_size)) {
                    continue;
                }

                size_t other_end = other->depth + other->storage_size;
                if (other_end > next_depth) {
                    next_depth = other_end;
                }

                collision = true;
            }

            if (!collision) {
                size_t end = depth + storage_size;
                assert(end <= INT32_MAX);

                state->depth      = depth;
                state->new_offset = -(int32_t)end;

                if (end > frame_end) {
                    frame_end = end;
                }

                break;
            }

            depth = next_depth;
        }
    }

    assert(frame_end <= INT32_MAX);
    assert(frame_end <= SIZE_MAX - 15);

    ctx->func->stack_frame_size = (frame_end + 15) & ~((size_t)15);
}


static void rewrite_operand(StackColorCtx* ctx, IROperand* operand) {
    if (!operand || operand->kind != IR_OP_STACK || operand->stack_offset >= 0) {
        return;
    }

    StackSlotState* state = find_state_for_offset(ctx, operand->stack_offset);
    assert(state != NULL);
    assert(state->active);
    assert(state->new_offset != 0);

    int64_t offset_in_slot =
        (int64_t)operand->stack_offset - (int64_t)state->slot->old_offset;

    assert(offset_in_slot >= 0);
    assert((uint64_t)offset_in_slot < state->slot->size);

    int64_t new_offset = (int64_t)state->new_offset + offset_in_slot;
    assert(new_offset >= INT32_MIN && new_offset <= INT32_MAX);

    operand->stack_offset = (int32_t)new_offset;
}


static void rewrite_function_operands(StackColorCtx* ctx) {
    IRFunction* func = ctx->func;

    if (func->reg_save_slot < 0) {
        StackSlotState* state = find_state_for_offset(ctx, func->reg_save_slot);

        assert(state != NULL);
        assert(state->active);

        int64_t offset_in_slot =
            (int64_t)func->reg_save_slot - (int64_t)state->slot->old_offset;
        int64_t new_offset = (int64_t)state->new_offset + offset_in_slot;

        assert(new_offset >= INT32_MIN && new_offset <= INT32_MAX);
        func->reg_save_slot = (int32_t)new_offset;
    }

    for (IRBlock* block = func->first_block; block != NULL; block = block->next_block) {
        for (IRInst* inst = block->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP) {
                continue;
            }

            rewrite_operand(ctx, &inst->dst);
            rewrite_operand(ctx, &inst->src1);
            rewrite_operand(ctx, &inst->src2);

            for (size_t i = 0; i < inst->extra_arg_count; ++i) {
                rewrite_operand(ctx, &inst->extra_args[i]);
            }

            for (size_t i = 0; i < inst->asm_input_count; ++i) {
                rewrite_operand(ctx, &inst->asm_inputs[i].val);
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

    StackColorCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.arena           = arena;
    ctx.func            = func;
    ctx.block_count     = func->next_block_id;
    ctx.slot_count      = func->stack_slot_count;
    ctx.slot_word_count = (ctx.slot_count + 63) / 64;
    ctx.scratch_capacity = compute_scratch_capacity(func);

    assert(ctx.block_count > 0);
    assert(ctx.slot_count > 0);
    assert(ctx.scratch_capacity > 0);

    ctx.blocks = ARENA_NEW_ARRAY_ZERO(arena, IRBlock*, ctx.block_count);
    ctx.block_inst_begin = ARENA_NEW_ARRAY_ZERO(arena, size_t, ctx.block_count);
    ctx.block_inst_end   = ARENA_NEW_ARRAY_ZERO(arena, size_t, ctx.block_count);

    for (IRBlock* block = func->first_block; block != NULL; block = block->next_block) {
        assert(block->id < ctx.block_count);
        assert(ctx.blocks[block->id] == NULL);
        ctx.blocks[block->id] = block;
    }

    ctx.states = ARENA_NEW_ARRAY_ZERO(arena, StackSlotState, ctx.slot_count);
    ctx.slot_lookup_order = ARENA_NEW_ARRAY(arena, uint32_t, ctx.slot_count);
    ctx.color_order = ARENA_NEW_ARRAY(arena, uint32_t, ctx.slot_count);
    ctx.conflict_degree = ARENA_NEW_ARRAY_ZERO(arena, size_t, ctx.slot_count);
    ctx.predecessors = ARENA_NEW_ARRAY_ZERO(arena, BlockPreds, ctx.block_count);
    ctx.queued = ARENA_NEW_ARRAY_ZERO(arena, bool, ctx.block_count);

    for (size_t slot_id = 0; slot_id < ctx.slot_count; ++slot_id) {
        IRStackSlot* slot = &func->stack_slots[slot_id];

        assert(slot->id == slot_id);
        assert(slot->old_offset < 0);
        assert(slot->size > 0);
        assert(slot->align > 0);
        assert((slot->align & (slot->align - 1)) == 0);
        assert(slot->size <= SIZE_MAX - (slot->align - 1));

        int64_t slot_end = (int64_t)slot->old_offset + (int64_t)slot->size;
        assert(slot_end <= 0);
        assert(slot_end >= INT32_MIN);

        ctx.states[slot_id].slot = slot;
        ctx.states[slot_id].storage_size = (slot->size + slot->align - 1) & ~(slot->align - 1);
        ctx.slot_lookup_order[slot_id] = (uint32_t)slot_id;
    }

    qsort_r(
        ctx.slot_lookup_order,
        ctx.slot_count,
        sizeof(uint32_t),
        compare_slot_offsets,
        &ctx
    );

    for (size_t i = 1; i < ctx.slot_count; ++i) {
        uint32_t prev_id = ctx.slot_lookup_order[i - 1];
        uint32_t curr_id = ctx.slot_lookup_order[i];

        int64_t prev_end =
            (int64_t)ctx.states[prev_id].slot->old_offset +
            (int64_t)ctx.states[prev_id].slot->size;

        assert(prev_end <= (int64_t)ctx.states[curr_id].slot->old_offset);
    }

    ctx.block_use = bitmatrix_create(arena, ctx.block_count, ctx.slot_count);
    ctx.block_def = bitmatrix_create(arena, ctx.block_count, ctx.slot_count);
    ctx.live_in  = bitmatrix_create(arena, ctx.block_count, ctx.slot_count);
    ctx.conflicts = bitmatrix_create(arena, ctx.slot_count, ctx.slot_count);

    ctx.access_scratch = ARENA_NEW_ARRAY(arena, uint32_t, ctx.scratch_capacity);
    ctx.def_scratch    = ARENA_NEW_ARRAY(arena, uint32_t, ctx.scratch_capacity);
    ctx.use_scratch    = ARENA_NEW_ARRAY(arena, uint32_t, ctx.scratch_capacity);

    size_t instruction_count = flatten_instructions(&ctx);

    if (func->reg_save_slot < 0) {
        StackSlotState* reg_save_state = find_state_for_offset(&ctx, func->reg_save_slot);
        assert(reg_save_state != NULL);
        reg_save_state->active = true;
        reg_save_state->escaped = true;
    }

    if (instruction_count == 0) {
        func->stack_frame_size = 0;
        return;
    }

    build_cfg_edges(&ctx);

    build_local_liveness(&ctx);
    
    solve_stack_liveness(&ctx);
    
    build_conflict_graph(&ctx);
    
    color_stack_slots(&ctx);
    
    rewrite_function_operands(&ctx);
}


void stack_color_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* func = module->first_func; func != NULL; func = func->next) {
        stack_color_run_on_function(arena, func);
    }
}
