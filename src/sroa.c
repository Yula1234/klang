#include "sroa.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef enum SROAAccessKind {
    SROA_ACCESS_READ = 1u << 0,
    SROA_ACCESS_WRITE = 1u << 1,
    SROA_ACCESS_COPY = 1u << 2,
    SROA_ACCESS_OPAQUE = 1u << 3
} SROAAccessKind;

typedef struct SROAInterval {
    size_t begin;
    size_t end;
    uint32_t flags;
} SROAInterval;

typedef struct SROASlice {
    size_t offset;
    size_t size;
    uint32_t access_flags;
    bool blocked;
    int32_t new_slot;
} SROASlice;

typedef enum SROAPtrStateKind {
    SROA_PTR_UNKNOWN = 0,
    SROA_PTR_KNOWN,
    SROA_PTR_CONFLICT
} SROAPtrStateKind;

typedef struct SROAPtrState {
    SROAPtrStateKind kind;
    int32_t base_offset;
    size_t offset;
    size_t extent;
} SROAPtrState;

typedef struct SROAObject {
    IRStackSlot* slot;
    SROAInterval* accesses;
    size_t access_count;
    size_t access_cap;
    SROAInterval* escapes;
    size_t escape_count;
    size_t escape_cap;
    SROASlice* slices;
    size_t slice_count;
    size_t slice_cap;
    bool unknown_access;
} SROAObject;

typedef struct SROAContext {
    Arena* arena;
    IRFunction* func;
    SROAObject* objects;
    size_t object_count;
    size_t object_cap;
    SROAPtrState* ptrs;
    size_t ptr_count;
} SROAContext;

static bool add_overflow_size(size_t a, size_t b, size_t* out) {
    if (b > SIZE_MAX - a) {
        return true;
    }

    *out = a + b;
    return false;
}

static bool range_end(size_t begin, size_t size, size_t limit, size_t* end) {
    if (size == 0) {
        return false;
    }

    if (add_overflow_size(begin, size, end)) {
        return false;
    }

    return *end <= limit;
}

static bool ranges_overlap(size_t a_begin, size_t a_end, size_t b_begin, size_t b_end) {
    return a_begin < b_end && b_begin < a_end;
}

static void push_interval(Arena* arena, SROAInterval** data, size_t* count, size_t* cap, SROAInterval value) {
    ARENA_DA_PUSH(arena, *data, *count, *cap, value);
}

static void push_slice(Arena* arena, SROASlice** data, size_t* count, size_t* cap, SROASlice value) {
    ARENA_DA_PUSH(arena, *data, *count, *cap, value);
}

static SROAObject* find_object_by_offset(SROAContext* ctx, int32_t offset) {
    for (size_t i = 0; i < ctx->object_count; ++i) {
        SROAObject* object = &ctx->objects[i];
        int64_t begin = object->slot->old_offset;
        int64_t end = begin + (int64_t)object->slot->size;

        if ((int64_t)offset >= begin && (int64_t)offset < end) {
            return object;
        }
    }

    return NULL;
}

static SROAObject* find_object_exact(SROAContext* ctx, int32_t offset) {
    for (size_t i = 0; i < ctx->object_count; ++i) {
        if (ctx->objects[i].slot->old_offset == offset) {
            return &ctx->objects[i];
        }
    }

    return NULL;
}

static SROAObject* find_object_for_range(SROAContext* ctx, int32_t offset, size_t size) {
    SROAObject* object = find_object_by_offset(ctx, offset);

    if (!object) {
        return NULL;
    }

    size_t relative = (size_t)((int64_t)offset - (int64_t)object->slot->old_offset);
    size_t end;

    if (!range_end(relative, size, object->slot->size, &end)) {
        return NULL;
    }

    return object;
}

static void add_access(SROAContext* ctx, SROAObject* object, size_t offset, size_t size, uint32_t flags) {
    size_t end;

    if (!range_end(offset, size, object->slot->size, &end)) {
        object->unknown_access = true;
        return;
    }

    SROAInterval interval = {
        .begin = offset,
        .end = end,
        .flags = flags
    };

    push_interval(ctx->arena, &object->accesses, &object->access_count, &object->access_cap, interval);
}

static void add_escape(SROAContext* ctx, SROAObject* object, size_t offset, size_t size) {
    size_t end;

    if (size == 0) {
        size = object->slot->size - offset;
    }

    if (!range_end(offset, size, object->slot->size, &end)) {
        object->unknown_access = true;
        return;
    }

    SROAInterval interval = {
        .begin = offset,
        .end = end,
        .flags = SROA_ACCESS_OPAQUE
    };

    push_interval(ctx->arena, &object->escapes, &object->escape_count, &object->escape_cap, interval);
}

static void mark_unknown(SROAContext* ctx) {
    for (size_t i = 0; i < ctx->object_count; ++i) {
        ctx->objects[i].unknown_access = true;
    }
}

static SROAObject* object_from_ptr(SROAContext* ctx, const SROAPtrState* ptr, size_t* offset, size_t access_size) {
    if (!ptr || ptr->kind != SROA_PTR_KNOWN) {
        return NULL;
    }

    SROAObject* object = find_object_exact(ctx, ptr->base_offset);

    if (!object) {
        return NULL;
    }

    if (!range_end(ptr->offset, access_size, object->slot->size, offset)) {
        return NULL;
    }

    return object;
}

static bool operand_is_pointer_vreg(const IROperand* operand) {
    return operand && operand->kind == IR_OP_VREG;
}

static SROAPtrState ptr_unknown(void) {
    return (SROAPtrState){
        .kind = SROA_PTR_UNKNOWN,
        .base_offset = 0,
        .offset = 0,
        .extent = 0
    };
}

static SROAPtrState ptr_from_stack(int32_t base_offset, size_t extent) {
    return (SROAPtrState){
        .kind = SROA_PTR_KNOWN,
        .base_offset = base_offset,
        .offset = 0,
        .extent = extent
    };
}

static bool ptr_equal(const SROAPtrState* a, const SROAPtrState* b) {
    return a->kind == b->kind &&
           a->base_offset == b->base_offset &&
           a->offset == b->offset &&
           a->extent == b->extent;
}

static SROAPtrState combine_ptrs(const SROAPtrState* old_state, const SROAPtrState* new_state) {
    if (old_state->kind == SROA_PTR_UNKNOWN) {
        return *new_state;
    }

    if (new_state->kind == SROA_PTR_UNKNOWN) {
        return *old_state;
    }

    if (ptr_equal(old_state, new_state)) {
        return *old_state;
    }

    return (SROAPtrState){
        .kind = SROA_PTR_CONFLICT,
        .base_offset = 0,
        .offset = 0,
        .extent = 0
    };
}

static void ensure_ptr_count(SROAContext* ctx) {
    if (ctx->ptr_count >= (size_t)ctx->func->next_vreg_id) {
        return;
    }

    ctx->ptr_count = ctx->func->next_vreg_id;
    ctx->ptrs = ARENA_NEW_ARRAY_ZERO(ctx->arena, SROAPtrState, ctx->ptr_count);
}

static const SROAPtrState* get_ptr_state(const SROAContext* ctx, uint32_t vreg) {
    if (vreg >= ctx->ptr_count) {
        return NULL;
    }

    return &ctx->ptrs[vreg];
}

static void init_objects(SROAContext* ctx) {
    for (size_t i = 0; i < ctx->func->stack_slot_count; ++i) {
        IRStackSlot* slot = &ctx->func->stack_slots[i];

        if (slot->size == 0 || slot->is_spill) {
            continue;
        }

        SROAObject object = {
            .slot = slot,
            .accesses = NULL,
            .access_count = 0,
            .access_cap = 0,
            .escapes = NULL,
            .escape_count = 0,
            .escape_cap = 0,
            .slices = NULL,
            .slice_count = 0,
            .slice_cap = 0,
            .unknown_access = false
        };

        ARENA_DA_PUSH(ctx->arena, ctx->objects, ctx->object_count, ctx->object_cap, object);
    }
}

static bool ptr_propagation_pass(SROAContext* ctx) {
    bool changed = false;

    for (IRBlock* block = ctx->func->first_block; block != NULL; block = block->next_block) {
        for (IRInst* inst = block->first_inst; inst != NULL; inst = inst->next) {
            if (inst->dst.kind != IR_OP_VREG) {
                continue;
            }

            SROAPtrState next = ptr_unknown();

            if (inst->opcode == IR_ADDR && inst->src1.kind == IR_OP_STACK) {
                SROAObject* object = find_object_exact(ctx, inst->src1.stack_offset);

                if (object) {
                    size_t extent = inst->src1.byte_size;
                    if (extent == 0 || extent > object->slot->size) {
                        extent = object->slot->size;
                    }
                    next = ptr_from_stack(inst->src1.stack_offset, extent);
                }
            } else if ((inst->opcode == IR_MOV) && inst->src1.kind == IR_OP_VREG) {
                const SROAPtrState* source = get_ptr_state(ctx, inst->src1.vreg_id);
                if (source) {
                    next = *source;
                }
            } else if (inst->opcode == IR_ADD || inst->opcode == IR_SUB) {
                if (inst->src1.kind == IR_OP_VREG && inst->src2.kind == IR_OP_CONST) {
                    const SROAPtrState* source = get_ptr_state(ctx, inst->src1.vreg_id);

                    if (source && source->kind == SROA_PTR_KNOWN) {
                        SROAObject* object = find_object_exact(ctx, source->base_offset);
                        int64_t delta = inst->src2.int_val;

                        if (inst->opcode == IR_SUB) {
                            if (delta == INT64_MIN) {
                                delta = 0;
                                object = NULL;
                            } else {
                                delta = -delta;
                            }
                        }

                        if (object) {
                            int64_t new_offset = (int64_t)source->offset + delta;

                            if (new_offset >= 0 && (uint64_t)new_offset <= object->slot->size) {
                                size_t extent;

                                if (delta >= 0) {
                                    size_t consumed = (size_t)delta;
                                    extent = consumed >= source->extent ? 0 : source->extent - consumed;
                                } else {
                                    extent = object->slot->size - (size_t)new_offset;
                                }

                                next = (SROAPtrState){
                                    .kind = SROA_PTR_KNOWN,
                                    .base_offset = source->base_offset,
                                    .offset = (size_t)new_offset,
                                    .extent = extent
                                };
                            }
                        }
                    }
                }
            }

            SROAPtrState merged = combine_ptrs(&ctx->ptrs[inst->dst.vreg_id], &next);

            if (!ptr_equal(&ctx->ptrs[inst->dst.vreg_id], &merged)) {
                ctx->ptrs[inst->dst.vreg_id] = merged;
                changed = true;
            }
        }
    }

    return changed;
}

static void propagate_pointer_states(SROAContext* ctx) {
    bool changed = true;
    size_t limit = (size_t)ctx->func->next_vreg_id + 1;

    for (size_t i = 0; changed && i < limit; ++i) {
        changed = ptr_propagation_pass(ctx);
    }
}

static void record_stack_operand(SROAContext* ctx, const IROperand* operand, uint32_t flags) {
    if (!operand || operand->kind != IR_OP_STACK || operand->stack_offset >= 0) {
        return;
    }

    SROAObject* object = find_object_for_range(ctx, operand->stack_offset, operand->byte_size);

    if (!object) {
        mark_unknown(ctx);
        return;
    }

    size_t offset = (size_t)((int64_t)operand->stack_offset - (int64_t)object->slot->old_offset);
    add_access(ctx, object, offset, operand->byte_size, flags);
}

static void record_pointer_operand(SROAContext* ctx, const IROperand* operand, size_t access_size, uint32_t flags) {
    if (!operand_is_pointer_vreg(operand)) {
        return;
    }

    const SROAPtrState* ptr = get_ptr_state(ctx, operand->vreg_id);
    size_t offset = 0;
    SROAObject* object = object_from_ptr(ctx, ptr, &offset, access_size);

    if (!object) {
        mark_unknown(ctx);
        return;
    }

    add_access(ctx, object, offset, access_size, flags);
}

static void record_pointer_escape(SROAContext* ctx, const IROperand* operand) {
    if (!operand_is_pointer_vreg(operand)) {
        return;
    }

    const SROAPtrState* ptr = get_ptr_state(ctx, operand->vreg_id);

    if (!ptr || ptr->kind != SROA_PTR_KNOWN) {
        mark_unknown(ctx);
        return;
    }

    SROAObject* object = find_object_exact(ctx, ptr->base_offset);

    if (!object) {
        mark_unknown(ctx);
        return;
    }

    add_escape(ctx, object, ptr->offset, ptr->extent);
}

static bool const_memcpy_size(const IRInst* inst, size_t* size) {
    if (!inst || inst->src2.kind != IR_OP_CONST || inst->src2.int_val <= 0) {
        return false;
    }

    if ((uint64_t)inst->src2.int_val > SIZE_MAX) {
        return false;
    }

    *size = (size_t)inst->src2.int_val;
    return *size != 0;
}

static void analyze_instruction(SROAContext* ctx, IRInst* inst) {
    size_t copy_size = 0;

    switch (inst->opcode) {
        case IR_ADDR:
            break;

        case IR_LOAD:
            if (inst->src1.kind == IR_OP_STACK) {
                record_stack_operand(ctx, &inst->src1, SROA_ACCESS_READ);
            } else if (inst->src1.kind == IR_OP_VREG) {
                record_pointer_operand(ctx, &inst->src1, inst->dst.byte_size, SROA_ACCESS_READ);
            } else {
                mark_unknown(ctx);
            }
            break;

        case IR_STORE:
            if (inst->dst.kind == IR_OP_STACK) {
                record_stack_operand(ctx, &inst->dst, SROA_ACCESS_WRITE);
            } else if (inst->dst.kind == IR_OP_VREG) {
                record_pointer_operand(ctx, &inst->dst, inst->src1.byte_size, SROA_ACCESS_WRITE);
            } else {
                mark_unknown(ctx);
            }

            if (inst->src1.kind == IR_OP_VREG) {
                record_pointer_escape(ctx, &inst->src1);
            }
            break;

        case IR_ADD:
        case IR_SUB:
            if (inst->src1.kind == IR_OP_VREG) {
                const SROAPtrState* ptr = get_ptr_state(ctx, inst->src1.vreg_id);
                if (ptr && ptr->kind == SROA_PTR_KNOWN && inst->src2.kind != IR_OP_CONST) {
                    SROAObject* object = find_object_exact(ctx, ptr->base_offset);
                    if (object) {
                        object->unknown_access = true;
                    } else {
                        mark_unknown(ctx);
                    }
                }
            }
            break;

        case IR_MEMCPY:
            if (!const_memcpy_size(inst, &copy_size)) {
                mark_unknown(ctx);
                break;
            }

            if (inst->src1.kind == IR_OP_STACK) {
                record_stack_operand(ctx, &(IROperand){
                    .kind = IR_OP_STACK,
                    .byte_size = copy_size,
                    .is_signed = false,
                    .stack_offset = inst->src1.stack_offset
                }, SROA_ACCESS_COPY);
            } else if (inst->src1.kind == IR_OP_VREG) {
                record_pointer_operand(ctx, &inst->src1, copy_size, SROA_ACCESS_COPY);
            } else {
                mark_unknown(ctx);
            }

            if (inst->dst.kind == IR_OP_STACK) {
                record_stack_operand(ctx, &(IROperand){
                    .kind = IR_OP_STACK,
                    .byte_size = copy_size,
                    .is_signed = false,
                    .stack_offset = inst->dst.stack_offset
                }, SROA_ACCESS_COPY);
            } else if (inst->dst.kind == IR_OP_VREG) {
                record_pointer_operand(ctx, &inst->dst, copy_size, SROA_ACCESS_COPY);
            } else {
                mark_unknown(ctx);
            }
            break;

        case IR_CALL:
        case IR_CALL_PTR:
        case IR_TAIL_CALL:
        case IR_TAIL_CALL_PTR:
            record_pointer_escape(ctx, &inst->dst);
            record_pointer_escape(ctx, &inst->src1);
            record_pointer_escape(ctx, &inst->src2);

            for (size_t i = 0; i < inst->extra_arg_count; ++i) {
                IROperand* arg = &inst->extra_args[i];

                if (arg->kind == IR_OP_STACK) {
                    record_stack_operand(ctx, arg, SROA_ACCESS_READ);
                } else {
                    record_pointer_escape(ctx, arg);
                }
            }

            for (size_t i = 0; i < inst->asm_input_count; ++i) {
                IROperand* input = &inst->asm_inputs[i].val;

                if (input->kind == IR_OP_STACK) {
                    record_stack_operand(ctx, input, SROA_ACCESS_READ);
                } else {
                    record_pointer_escape(ctx, input);
                }
            }
            break;

        case IR_INLINE_ASM:
        case IR_VA_START:
        case IR_VA_ARG:
        case IR_VA_END:
        case IR_VA_COPY:
            mark_unknown(ctx);
            break;

        case IR_RET:
            record_pointer_escape(ctx, &inst->dst);
            record_pointer_escape(ctx, &inst->src1);
            record_pointer_escape(ctx, &inst->src2);
            record_stack_operand(ctx, &inst->dst, SROA_ACCESS_READ);
            record_stack_operand(ctx, &inst->src1, SROA_ACCESS_READ);
            record_stack_operand(ctx, &inst->src2, SROA_ACCESS_READ);
            break;

        case IR_MOV:
            record_stack_operand(ctx, &inst->dst, SROA_ACCESS_WRITE);
            record_stack_operand(ctx, &inst->src1, SROA_ACCESS_READ);
            break;

        default:
            if (inst->dst.kind == IR_OP_STACK || inst->src1.kind == IR_OP_STACK || inst->src2.kind == IR_OP_STACK) {
                record_stack_operand(ctx, &inst->dst, SROA_ACCESS_OPAQUE);
                record_stack_operand(ctx, &inst->src1, SROA_ACCESS_OPAQUE);
                record_stack_operand(ctx, &inst->src2, SROA_ACCESS_OPAQUE);
            }
            break;
    }
}

static void analyze_memory_accesses(SROAContext* ctx) {
    for (IRBlock* block = ctx->func->first_block; block != NULL; block = block->next_block) {
        for (IRInst* inst = block->first_inst; inst != NULL; inst = inst->next) {
            analyze_instruction(ctx, inst);
        }
    }
}

static void sort_intervals(SROAInterval* intervals, size_t count) {
    for (size_t i = 1; i < count; ++i) {
        SROAInterval value = intervals[i];
        size_t j = i;

        while (j > 0 &&
               (intervals[j - 1].begin > value.begin ||
                (intervals[j - 1].begin == value.begin && intervals[j - 1].end > value.end))) {
            intervals[j] = intervals[j - 1];
            --j;
        }

        intervals[j] = value;
    }
}

static bool exact_scalar_size(size_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

static size_t scalar_alignment(size_t size) {
    if (size >= 8) {
        return 8;
    }
    if (size >= 4) {
        return 4;
    }
    if (size >= 2) {
        return 2;
    }
    return 1;
}

static void build_slices(SROAContext* ctx, SROAObject* object) {
    if (object->unknown_access || object->access_count == 0) {
        return;
    }

    sort_intervals(object->accesses, object->access_count);
    sort_intervals(object->escapes, object->escape_count);

    for (size_t i = 0; i < object->access_count; ++i) {
        SROAInterval access = object->accesses[i];
        bool has_partial_overlap = false;
        bool has_escape = false;
        bool opaque = (access.flags & SROA_ACCESS_OPAQUE) != 0;

        for (size_t j = 0; j < object->access_count; ++j) {
            SROAInterval other = object->accesses[j];

            if (!ranges_overlap(access.begin, access.end, other.begin, other.end)) {
                continue;
            }

            if (access.begin != other.begin || access.end != other.end) {
                has_partial_overlap = true;
                break;
            }
        }

        for (size_t j = 0; j < object->escape_count; ++j) {
            SROAInterval escape = object->escapes[j];
            if (ranges_overlap(access.begin, access.end, escape.begin, escape.end)) {
                has_escape = true;
                break;
            }
        }

        if (has_partial_overlap || has_escape || opaque || !exact_scalar_size(access.end - access.begin)) {
            continue;
        }

        SROASlice slice = {
            .offset = access.begin,
            .size = access.end - access.begin,
            .access_flags = access.flags,
            .blocked = false,
            .new_slot = 0
        };

        for (size_t j = 0; j < object->slice_count; ++j) {
            SROASlice* existing = &object->slices[j];
            if (existing->offset == slice.offset && existing->size == slice.size) {
                existing->access_flags |= slice.access_flags;
                goto next_access;
            }
        }

        push_slice(ctx->arena, &object->slices, &object->slice_count, &object->slice_cap, slice);

next_access:
        (void)0;
    }
}

static SROASlice* find_slice(SROAObject* object, size_t offset, size_t size) {
    for (size_t i = 0; i < object->slice_count; ++i) {
        SROASlice* slice = &object->slices[i];
        if (slice->offset == offset && slice->size == size) {
            return slice;
        }
    }

    return NULL;
}

static void block_overlapping_slices(SROAObject* object, size_t begin, size_t end) {
    for (size_t i = 0; i < object->slice_count; ++i) {
        SROASlice* slice = &object->slices[i];
        if (ranges_overlap(slice->offset, slice->offset + slice->size, begin, end)) {
            slice->blocked = true;
        }
    }
}

static void finalize_candidates(SROAContext* ctx) {
    for (size_t i = 0; i < ctx->object_count; ++i) {
        SROAObject* object = &ctx->objects[i];

        for (size_t e = 0; e < object->escape_count; ++e) {
            block_overlapping_slices(object, object->escapes[e].begin, object->escapes[e].end);
        }

        for (size_t a = 0; a < object->access_count; ++a) {
            SROAInterval access = object->accesses[a];

            if ((access.flags & SROA_ACCESS_OPAQUE) != 0) {
                block_overlapping_slices(object, access.begin, access.end);
                continue;
            }

            SROASlice* slice = find_slice(object, access.begin, access.end - access.begin);
            if (!slice) {
                block_overlapping_slices(object, access.begin, access.end);
            }
        }

        size_t active_count = 0;

        for (size_t s = 0; s < object->slice_count; ++s) {
            SROASlice* slice = &object->slices[s];

            if (slice->blocked) {
                continue;
            }

            if (slice->size == 0 || !exact_scalar_size(slice->size)) {
                continue;
            }

            slice->new_slot = ir_func_alloc_stack_slot(
                ctx->func,
                slice->size,
                scalar_alignment(slice->size)
            );

            ++active_count;
        }

        if (active_count == 0) {
            object->slice_count = 0;
        }
    }
}

static bool pointer_is_split_target(SROAContext* ctx, const IROperand* operand, SROASlice** out_slice, size_t access_size) {
    if (!operand || operand->kind != IR_OP_VREG) {
        return false;
    }

    const SROAPtrState* ptr = get_ptr_state(ctx, operand->vreg_id);
    if (!ptr || ptr->kind != SROA_PTR_KNOWN) {
        return false;
    }

    SROAObject* object = find_object_exact(ctx, ptr->base_offset);
    if (!object) {
        return false;
    }

    SROASlice* slice = find_slice(object, ptr->offset, access_size);
    if (!slice || slice->blocked || slice->new_slot == 0) {
        return false;
    }

    *out_slice = slice;
    return true;
}

static bool stack_is_split_target(SROAContext* ctx, const IROperand* operand, SROASlice** out_slice) {
    if (!operand || operand->kind != IR_OP_STACK || operand->stack_offset >= 0) {
        return false;
    }

    SROAObject* object = find_object_for_range(ctx, operand->stack_offset, operand->byte_size);
    if (!object) {
        return false;
    }

    size_t offset = (size_t)((int64_t)operand->stack_offset - (int64_t)object->slot->old_offset);
    SROASlice* slice = find_slice(object, offset, operand->byte_size);

    if (!slice || slice->blocked || slice->new_slot == 0) {
        return false;
    }

    *out_slice = slice;
    return true;
}

static IROperand replacement_stack_operand(const IROperand* original, int32_t new_slot) {
    IROperand result = *original;
    result.stack_offset = new_slot;
    return result;
}

static void rewrite_instruction(SROAContext* ctx, IRInst* inst) {
    SROASlice* slice = NULL;

    switch (inst->opcode) {
        case IR_ADDR:
            break;

        case IR_LOAD:
            if (stack_is_split_target(ctx, &inst->src1, &slice)) {
                inst->opcode = IR_MOV;
                inst->src1 = replacement_stack_operand(&inst->src1, slice->new_slot);
                inst->src2 = ir_op_none();
            } else if (pointer_is_split_target(ctx, &inst->src1, &slice, inst->dst.byte_size)) {
                inst->opcode = IR_MOV;
                inst->src1 = ir_op_stack(slice->new_slot, slice->size, inst->dst.is_signed);
                inst->src2 = ir_op_none();
            }
            break;

        case IR_STORE:
            if (stack_is_split_target(ctx, &inst->dst, &slice)) {
                inst->dst = replacement_stack_operand(&inst->dst, slice->new_slot);
            } else if (pointer_is_split_target(ctx, &inst->dst, &slice, inst->src1.byte_size)) {
                inst->opcode = IR_MOV;
                inst->dst = ir_op_stack(slice->new_slot, slice->size, inst->src1.is_signed);
            }
            break;

        case IR_MOV:
            if (stack_is_split_target(ctx, &inst->dst, &slice)) {
                inst->dst = replacement_stack_operand(&inst->dst, slice->new_slot);
            }

            if (stack_is_split_target(ctx, &inst->src1, &slice)) {
                inst->src1 = replacement_stack_operand(&inst->src1, slice->new_slot);
            }
            break;

        case IR_MEMCPY: {
            size_t copy_size = 0;
            
            if (!const_memcpy_size(inst, &copy_size)) {
                break;
            }

            SROASlice* src_slice = NULL;
            SROASlice* dst_slice = NULL;

            bool src_split = stack_is_split_target(ctx, &inst->src1, &src_slice) && src_slice->size == copy_size;
            bool dst_split = stack_is_split_target(ctx, &inst->dst, &dst_slice) && dst_slice->size == copy_size;

            if (src_split && dst_split) {
                inst->opcode = IR_MOV;
                inst->src1 = ir_op_stack(src_slice->new_slot, src_slice->size, false);
                inst->dst = ir_op_stack(dst_slice->new_slot, dst_slice->size, false);
                inst->src2 = ir_op_none();
            } else if (src_split || dst_split) {
                if (src_split) {
                    inst->src1 = ir_op_stack(src_slice->new_slot, src_slice->size, false);
                }
                if (dst_split) {
                    inst->dst = ir_op_stack(dst_slice->new_slot, dst_slice->size, false);
                }
            }
            break;
        }

        default:
            break;
    }

    for (size_t i = 0; i < inst->extra_arg_count; ++i) {
        if (stack_is_split_target(ctx, &inst->extra_args[i], &slice)) {
            inst->extra_args[i] = replacement_stack_operand(&inst->extra_args[i], slice->new_slot);
        }
    }

    for (size_t i = 0; i < inst->asm_input_count; ++i) {
        if (stack_is_split_target(ctx, &inst->asm_inputs[i].val, &slice)) {
            inst->asm_inputs[i].val = replacement_stack_operand(&inst->asm_inputs[i].val, slice->new_slot);
        }
    }

    for (size_t i = 0; i < inst->asm_output_count; ++i) {
        if (stack_is_split_target(ctx, &inst->asm_outputs[i].val, &slice)) {
            inst->asm_outputs[i].val = replacement_stack_operand(&inst->asm_outputs[i].val, slice->new_slot);
        }
    }
}

static void transform(SROAContext* ctx) {
    for (IRBlock* block = ctx->func->first_block; block != NULL; block = block->next_block) {
        for (IRInst* inst = block->first_inst; inst != NULL; inst = inst->next) {
            rewrite_instruction(ctx, inst);
        }
    }

    ir_eliminate_nops(ctx->func);
}

void sroa_run_on_function(Arena* arena, IRFunction* func) {
    if (!arena || !func || !func->first_block || func->stack_slot_count == 0) {
        return;
    }

    SROAContext ctx = {
        .arena = arena,
        .func = func,
        .objects = NULL,
        .object_count = 0,
        .object_cap = 0,
        .ptrs = NULL,
        .ptr_count = 0,
    };

    init_objects(&ctx);

    ensure_ptr_count(&ctx);
    
    propagate_pointer_states(&ctx);
    
    analyze_memory_accesses(&ctx);

    if (ctx.object_count == 0) {
        return;
    }

    for (size_t i = 0; i < ctx.object_count; ++i) {
        build_slices(&ctx, &ctx.objects[i]);
    }

    finalize_candidates(&ctx);
    
    transform(&ctx);
}

void sroa_run_on_module(Arena* arena, IRModule* module) {
    if (!arena || !module) {
        return;
    }

    for (IRFunction* func = module->first_func; func != NULL; func = func->next) {
        sroa_run_on_function(arena, func);
    }
}