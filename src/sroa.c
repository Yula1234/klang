#include "sroa.h"

#include <string.h>
#include <assert.h>

typedef struct SROAField {
    int32_t offset;
    size_t  size;
    int32_t new_slot;
} SROAField;

typedef struct SROASlot {
    int32_t    base_offset;
    size_t     total_size;
    bool       escaped;
    bool       can_split;
    SROAField* fields;
    size_t     field_count;
    size_t     field_cap;
} SROASlot;

typedef struct PtrOrigin {
    int32_t base_offset;
    int32_t offset;
    bool    valid;
} PtrOrigin;

typedef struct SROAContext {
    Arena*       arena;
    IRFunction*  func;
    SROASlot*    slots;
    size_t       slot_count;
    size_t       slot_cap;
    PtrOrigin*   vreg_ptrs;
    size_t       vreg_cap;
} SROAContext;

static SROASlot* find_or_add_slot(SROAContext* ctx, int32_t base_offset, size_t size) {
    for (size_t i = 0; i < ctx->slot_count; ++i) {
        if (ctx->slots[i].base_offset == base_offset) {
            if (size > ctx->slots[i].total_size) {
                ctx->slots[i].total_size = size;
            }
            return &ctx->slots[i];
        }
    }

    SROASlot slot = {
        .base_offset = base_offset,
        .total_size  = (size == 0) ? 8 : size,
        .escaped     = false,
        .can_split   = false,
        .fields      = NULL,
        .field_count = 0,
        .field_cap   = 0
    };

    ARENA_DA_PUSH(ctx->arena, ctx->slots, ctx->slot_count, ctx->slot_cap, slot);

    return &ctx->slots[ctx->slot_count - 1];
}

static SROASlot* get_slot_by_base(SROAContext* ctx, int32_t base_offset) {
    for (size_t i = 0; i < ctx->slot_count; ++i) {
        if (ctx->slots[i].base_offset == base_offset) {
            return &ctx->slots[i];
        }
    }

    return NULL;
}

static SROASlot* get_containing_slot(SROAContext* ctx, int32_t stack_offset) {
    for (size_t i = 0; i < ctx->slot_count; ++i) {
        int32_t base = ctx->slots[i].base_offset;
        int32_t end  = base + (int32_t)ctx->slots[i].total_size;

        if (stack_offset >= base && stack_offset < end) {
            return &ctx->slots[i];
        }
    }

    return NULL;
}

static void mark_slot_escaped(SROAContext* ctx, int32_t base_offset) {
    SROASlot* slot = get_slot_by_base(ctx, base_offset);

    if (slot != NULL) {
        slot->escaped = true;
    }
}

static void register_field_access(SROAContext* ctx, SROASlot* slot, int32_t rel_offset, size_t size) {
    if (size == 0) {
        size = 8;
    }

    for (size_t i = 0; i < slot->field_count; ++i) {
        if (slot->fields[i].offset == rel_offset) {
            if (size > slot->fields[i].size) {
                slot->fields[i].size = size;
            }
            return;
        }
    }

    SROAField f = {
        .offset   = rel_offset,
        .size     = size,
        .new_slot = 0
    };

    ARENA_DA_PUSH(ctx->arena, slot->fields, slot->field_count, slot->field_cap, f);
}

static void sort_fields(SROASlot* slot) {
    for (size_t i = 0; i < slot->field_count; ++i) {
        for (size_t j = i + 1; j < slot->field_count; ++j) {
            if (slot->fields[j].offset < slot->fields[i].offset) {
                SROAField tmp = slot->fields[i];
                slot->fields[i] = slot->fields[j];
                slot->fields[j] = tmp;
            }
        }
    }
}

static void insert_inst_after(IRBlock* block, IRInst* target, IRInst* new_inst) {
    new_inst->next = target->next;
    target->next   = new_inst;

    if (block->last_inst == target) {
        block->last_inst = new_inst;
    }

    block->inst_count++;
}

static void sroa_analyze(SROAContext* ctx) {
    IRFunction* func = ctx->func;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_ADDR && inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                find_or_add_slot(ctx, inst->src1.stack_offset, inst->src1.byte_size);
            }

            if (inst->opcode == IR_MEMCPY && inst->src2.kind == IR_OP_CONST) {
                if (inst->dst.kind == IR_OP_STACK && inst->dst.stack_offset < 0) {
                    find_or_add_slot(ctx, inst->dst.stack_offset, (size_t)inst->src2.int_val);
                }

                if (inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                    find_or_add_slot(ctx, inst->src1.stack_offset, (size_t)inst->src2.int_val);
                }
            }
        }
    }

    bool ptr_changed = true;

    while (ptr_changed) {
        ptr_changed = false;

        for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
            for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
                if (inst->opcode == IR_ADDR && inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                    uint32_t vid = inst->dst.vreg_id;

                    if (vid < ctx->vreg_cap && !ctx->vreg_ptrs[vid].valid) {
                        ctx->vreg_ptrs[vid].base_offset = inst->src1.stack_offset;
                        ctx->vreg_ptrs[vid].offset      = 0;
                        ctx->vreg_ptrs[vid].valid       = true;
                        ptr_changed = true;
                    }
                }

                if (inst->opcode == IR_ADD && inst->src1.kind == IR_OP_VREG && inst->src2.kind == IR_OP_CONST) {
                    uint32_t in_vid  = inst->src1.vreg_id;
                    uint32_t out_vid = inst->dst.vreg_id;

                    if (in_vid < ctx->vreg_cap && ctx->vreg_ptrs[in_vid].valid && out_vid < ctx->vreg_cap) {
                        if (!ctx->vreg_ptrs[out_vid].valid) {
                            ctx->vreg_ptrs[out_vid].base_offset = ctx->vreg_ptrs[in_vid].base_offset;
                            ctx->vreg_ptrs[out_vid].offset      = ctx->vreg_ptrs[in_vid].offset + (int32_t)inst->src2.int_val;
                            ctx->vreg_ptrs[out_vid].valid       = true;
                            ptr_changed = true;
                        }
                    }
                }

                if (inst->opcode == IR_MOV && inst->src1.kind == IR_OP_VREG && inst->dst.kind == IR_OP_VREG) {
                    uint32_t in_vid  = inst->src1.vreg_id;
                    uint32_t out_vid = inst->dst.vreg_id;

                    if (in_vid < ctx->vreg_cap && ctx->vreg_ptrs[in_vid].valid && out_vid < ctx->vreg_cap) {
                        if (!ctx->vreg_ptrs[out_vid].valid) {
                            ctx->vreg_ptrs[out_vid] = ctx->vreg_ptrs[in_vid];
                            ptr_changed = true;
                        }
                    }
                }
            }
        }
    }

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_CALL || inst->opcode == IR_CALL_PTR || inst->opcode == IR_INLINE_ASM) {
                for (size_t a = 0; a < inst->extra_arg_count; ++a) {
                    if (inst->extra_args[a].kind == IR_OP_VREG) {
                        uint32_t vid = inst->extra_args[a].vreg_id;

                        if (vid < ctx->vreg_cap && ctx->vreg_ptrs[vid].valid) {
                            mark_slot_escaped(ctx, ctx->vreg_ptrs[vid].base_offset);
                        }
                    }
                }

                for (size_t a = 0; a < inst->asm_input_count; ++a) {
                    if (inst->asm_inputs[a].val.kind == IR_OP_VREG) {
                        uint32_t vid = inst->asm_inputs[a].val.vreg_id;

                        if (vid < ctx->vreg_cap && ctx->vreg_ptrs[vid].valid) {
                            mark_slot_escaped(ctx, ctx->vreg_ptrs[vid].base_offset);
                        }
                    }
                }
            }

            if (inst->opcode == IR_RET && inst->dst.kind == IR_OP_VREG) {
                uint32_t vid = inst->dst.vreg_id;

                if (vid < ctx->vreg_cap && ctx->vreg_ptrs[vid].valid) {
                    mark_slot_escaped(ctx, ctx->vreg_ptrs[vid].base_offset);
                }
            }

            if (inst->opcode == IR_STORE && inst->src1.kind == IR_OP_VREG) {
                uint32_t vid = inst->src1.vreg_id;

                if (vid < ctx->vreg_cap && ctx->vreg_ptrs[vid].valid) {
                    mark_slot_escaped(ctx, ctx->vreg_ptrs[vid].base_offset);
                }
            }

            if (inst->opcode == IR_ADD && inst->src1.kind == IR_OP_VREG && inst->src2.kind != IR_OP_CONST) {
                uint32_t vid = inst->src1.vreg_id;

                if (vid < ctx->vreg_cap && ctx->vreg_ptrs[vid].valid) {
                    mark_slot_escaped(ctx, ctx->vreg_ptrs[vid].base_offset);
                }
            }

            if (inst->opcode == IR_LOAD && inst->src1.kind == IR_OP_VREG) {
                uint32_t vid = inst->src1.vreg_id;

                if (vid < ctx->vreg_cap && ctx->vreg_ptrs[vid].valid) {
                    SROASlot* slot = get_slot_by_base(ctx, ctx->vreg_ptrs[vid].base_offset);

                    if (slot != NULL) {
                        register_field_access(ctx, slot, ctx->vreg_ptrs[vid].offset, inst->dst.byte_size);
                    }
                }
            }

            if (inst->opcode == IR_STORE && inst->dst.kind == IR_OP_VREG) {
                uint32_t vid = inst->dst.vreg_id;

                if (vid < ctx->vreg_cap && ctx->vreg_ptrs[vid].valid) {
                    SROASlot* slot = get_slot_by_base(ctx, ctx->vreg_ptrs[vid].base_offset);

                    if (slot != NULL) {
                        register_field_access(ctx, slot, ctx->vreg_ptrs[vid].offset, inst->src1.byte_size);
                    }
                }
            }

            if (inst->dst.kind == IR_OP_STACK && inst->dst.stack_offset < 0) {
                SROASlot* slot = get_containing_slot(ctx, inst->dst.stack_offset);

                if (slot != NULL) {
                    register_field_access(ctx, slot, inst->dst.stack_offset - slot->base_offset, inst->dst.byte_size);
                }
            }

            if (inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                SROASlot* slot = get_containing_slot(ctx, inst->src1.stack_offset);

                if (slot != NULL) {
                    register_field_access(ctx, slot, inst->src1.stack_offset - slot->base_offset, inst->src1.byte_size);
                }
            }
        }
    }

    for (size_t i = 0; i < ctx->slot_count; ++i) {
        SROASlot* slot = &ctx->slots[i];

        if (slot->escaped || slot->total_size <= 8 || slot->field_count == 0) {
            slot->can_split = false;
            continue;
        }

        sort_fields(slot);

        bool valid_partition = true;

        for (size_t f = 0; f < slot->field_count; ++f) {
            if (slot->fields[f].size != 1 && slot->fields[f].size != 2 &&
                slot->fields[f].size != 4 && slot->fields[f].size != 8) {
                valid_partition = false;
                break;
            }

            if (f + 1 < slot->field_count) {
                if (slot->fields[f].offset + (int32_t)slot->fields[f].size > slot->fields[f + 1].offset) {
                    valid_partition = false;
                    break;
                }
            }
        }

        if (valid_partition) {
            size_t covered_bytes = 0;

            for (size_t f = 0; f < slot->field_count; ++f) {
                covered_bytes += slot->fields[f].size;
            }

            if (covered_bytes != slot->total_size) {
                valid_partition = false;
            }
        }

        if (valid_partition) {
            slot->can_split = true;

            for (size_t f = 0; f < slot->field_count; ++f) {
                size_t sz    = slot->fields[f].size;
                size_t align = (sz >= 8) ? 8 : (sz >= 4 ? 4 : (sz >= 2 ? 2 : 1));

                slot->fields[f].new_slot = ir_func_alloc_stack_slot(func, sz, align);
            }
        }
    }
}

static SROAField* get_matching_field(SROASlot* slot, int32_t rel_offset, size_t size) {
    if (!slot || !slot->can_split) {
        return NULL;
    }

    for (size_t f = 0; f < slot->field_count; ++f) {
        if (slot->fields[f].offset == rel_offset) {
            if (size == 0 || slot->fields[f].size == size) {
                return &slot->fields[f];
            }
        }
    }

    return NULL;
}

static void sroa_transform(SROAContext* ctx) {
    IRFunction* func = ctx->func;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP) {
                continue;
            }

            if (inst->opcode == IR_ADDR && inst->src1.kind == IR_OP_STACK) {
                SROASlot* slot = get_slot_by_base(ctx, inst->src1.stack_offset);

                if (slot != NULL && slot->can_split) {
                    inst->opcode = IR_NOP;
                    continue;
                }
            }

            if (inst->opcode == IR_ADD && inst->src1.kind == IR_OP_VREG && inst->src2.kind == IR_OP_CONST) {
                uint32_t vid = inst->src1.vreg_id;

                if (vid < ctx->vreg_cap && ctx->vreg_ptrs[vid].valid) {
                    SROASlot* slot = get_slot_by_base(ctx, ctx->vreg_ptrs[vid].base_offset);

                    if (slot != NULL && slot->can_split) {
                        inst->opcode = IR_NOP;
                        continue;
                    }
                }
            }

            if (inst->opcode == IR_LOAD && inst->src1.kind == IR_OP_VREG) {
                uint32_t vid = inst->src1.vreg_id;

                if (vid < ctx->vreg_cap && ctx->vreg_ptrs[vid].valid) {
                    SROASlot* slot = get_slot_by_base(ctx, ctx->vreg_ptrs[vid].base_offset);
                    SROAField* f   = get_matching_field(slot, ctx->vreg_ptrs[vid].offset, inst->dst.byte_size);

                    if (f != NULL) {
                        inst->opcode = IR_MOV;
                        inst->src1   = ir_op_stack(f->new_slot, f->size, inst->dst.is_signed);
                        inst->src2   = ir_op_none();
                        continue;
                    }
                }
            }

            if (inst->opcode == IR_STORE && inst->dst.kind == IR_OP_VREG) {
                uint32_t vid = inst->dst.vreg_id;

                if (vid < ctx->vreg_cap && ctx->vreg_ptrs[vid].valid) {
                    SROASlot* slot = get_slot_by_base(ctx, ctx->vreg_ptrs[vid].base_offset);
                    SROAField* f   = get_matching_field(slot, ctx->vreg_ptrs[vid].offset, inst->src1.byte_size);

                    if (f != NULL) {
                        inst->opcode = IR_MOV;
                        inst->dst    = ir_op_stack(f->new_slot, f->size, inst->src1.is_signed);
                        continue;
                    }
                }
            }

            if (inst->opcode == IR_MOV) {
                if (inst->dst.kind == IR_OP_STACK && inst->dst.stack_offset < 0) {
                    SROASlot* slot = get_containing_slot(ctx, inst->dst.stack_offset);

                    if (slot != NULL && slot->can_split) {
                        SROAField* f = get_matching_field(slot, inst->dst.stack_offset - slot->base_offset, inst->dst.byte_size);

                        if (f != NULL) {
                            inst->dst.stack_offset = f->new_slot;
                        }
                    }
                }

                if (inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                    SROASlot* slot = get_containing_slot(ctx, inst->src1.stack_offset);

                    if (slot != NULL && slot->can_split) {
                        SROAField* f = get_matching_field(slot, inst->src1.stack_offset - slot->base_offset, inst->src1.byte_size);

                        if (f != NULL) {
                            inst->src1.stack_offset = f->new_slot;
                        }
                    }
                }
            }

            if (inst->opcode == IR_MEMCPY) {
                SROASlot* src_slot = NULL;
                SROASlot* dst_slot = NULL;
                int32_t   src_off  = 0;
                int32_t   dst_off  = 0;

                if (inst->src1.kind == IR_OP_STACK) {
                    src_slot = get_slot_by_base(ctx, inst->src1.stack_offset);
                    src_off  = 0;
                } else if (inst->src1.kind == IR_OP_VREG && inst->src1.vreg_id < ctx->vreg_cap && ctx->vreg_ptrs[inst->src1.vreg_id].valid) {
                    src_slot = get_slot_by_base(ctx, ctx->vreg_ptrs[inst->src1.vreg_id].base_offset);
                    src_off  = ctx->vreg_ptrs[inst->src1.vreg_id].offset;
                }

                if (inst->dst.kind == IR_OP_STACK) {
                    dst_slot = get_slot_by_base(ctx, inst->dst.stack_offset);
                    dst_off  = 0;
                } else if (inst->dst.kind == IR_OP_VREG && inst->dst.vreg_id < ctx->vreg_cap && ctx->vreg_ptrs[inst->dst.vreg_id].valid) {
                    dst_slot = get_slot_by_base(ctx, ctx->vreg_ptrs[inst->dst.vreg_id].base_offset);
                    dst_off  = ctx->vreg_ptrs[inst->dst.vreg_id].offset;
                }

                if (src_slot != NULL && src_slot->can_split && dst_slot != NULL && dst_slot->can_split) {
                    IRInst* insert_point = inst;

                    for (size_t f = 0; f < src_slot->field_count; ++f) {
                        SROAField* sf = &src_slot->fields[f];
                        SROAField* df = get_matching_field(dst_slot, dst_off + sf->offset - src_off, sf->size);

                        if (df != NULL) {
                            IRInst* mov = ARENA_NEW_ZERO(ctx->arena, IRInst);

                            mov->opcode = IR_MOV;
                            mov->dst    = ir_op_stack(df->new_slot, df->size, false);
                            mov->src1   = ir_op_stack(sf->new_slot, sf->size, false);
                            mov->loc    = inst->loc;

                            insert_inst_after(b, insert_point, mov);
                            insert_point = mov;
                        }
                    }

                    inst->opcode = IR_NOP;
                    continue;
                }

                if (src_slot != NULL && src_slot->can_split) {
                    IRInst* insert_point = inst;

                    for (size_t f = 0; f < src_slot->field_count; ++f) {
                        SROAField* sf = &src_slot->fields[f];
                        IROperand  target_addr = inst->dst;

                        if (sf->offset != 0) {
                            uint32_t addr_v = ir_vreg_alloc(func);
                            IRInst* add_inst = ARENA_NEW_ZERO(ctx->arena, IRInst);

                            add_inst->opcode = IR_ADD;
                            add_inst->dst    = ir_op_vreg(addr_v, 8, false);
                            add_inst->src1   = inst->dst;
                            add_inst->src2   = ir_op_const((int64_t)sf->offset, 8, false);
                            add_inst->loc    = inst->loc;

                            insert_inst_after(b, insert_point, add_inst);
                            insert_point = add_inst;
                            target_addr  = ir_op_vreg(addr_v, 8, false);
                        }

                        IRInst* store_inst = ARENA_NEW_ZERO(ctx->arena, IRInst);

                        store_inst->opcode = IR_STORE;
                        store_inst->dst    = target_addr;
                        store_inst->src1   = ir_op_stack(sf->new_slot, sf->size, false);
                        store_inst->loc    = inst->loc;

                        insert_inst_after(b, insert_point, store_inst);
                        insert_point = store_inst;
                    }

                    inst->opcode = IR_NOP;
                    continue;
                }

                if (dst_slot != NULL && dst_slot->can_split) {
                    IRInst* insert_point = inst;

                    for (size_t f = 0; f < dst_slot->field_count; ++f) {
                        SROAField* df = &dst_slot->fields[f];
                        IROperand  source_addr = inst->src1;

                        if (df->offset != 0) {
                            uint32_t addr_v = ir_vreg_alloc(func);
                            IRInst* add_inst = ARENA_NEW_ZERO(ctx->arena, IRInst);

                            add_inst->opcode = IR_ADD;
                            add_inst->dst    = ir_op_vreg(addr_v, 8, false);
                            add_inst->src1   = inst->src1;
                            add_inst->src2   = ir_op_const((int64_t)df->offset, 8, false);
                            add_inst->loc    = inst->loc;

                            insert_inst_after(b, insert_point, add_inst);
                            insert_point = add_inst;
                            source_addr  = ir_op_vreg(addr_v, 8, false);
                        }

                        uint32_t val_v = ir_vreg_alloc(func);
                        IRInst* load_inst = ARENA_NEW_ZERO(ctx->arena, IRInst);

                        load_inst->opcode = IR_LOAD;
                        load_inst->dst    = ir_op_vreg(val_v, df->size, false);
                        load_inst->src1   = source_addr;
                        load_inst->loc    = inst->loc;

                        insert_inst_after(b, insert_point, load_inst);
                        insert_point = load_inst;

                        IRInst* mov_inst = ARENA_NEW_ZERO(ctx->arena, IRInst);

                        mov_inst->opcode = IR_MOV;
                        mov_inst->dst    = ir_op_stack(df->new_slot, df->size, false);
                        mov_inst->src1   = ir_op_vreg(val_v, df->size, false);
                        mov_inst->loc    = inst->loc;

                        insert_inst_after(b, insert_point, mov_inst);
                        insert_point = mov_inst;
                    }

                    inst->opcode = IR_NOP;
                    continue;
                }
            }
        }
    }

    ir_eliminate_nops(func);
}

void sroa_run_on_function(Arena* arena, IRFunction* func) {
    if (!func || !func->first_block) {
        return;
    }

    size_t vreg_cap = func->next_vreg_id + 1024;

    SROAContext ctx = {
        .arena      = arena,
        .func       = func,
        .slots      = NULL,
        .slot_count = 0,
        .slot_cap   = 0,
        .vreg_ptrs  = ARENA_NEW_ARRAY_ZERO(arena, PtrOrigin, vreg_cap),
        .vreg_cap   = vreg_cap
    };

    sroa_analyze(&ctx);
    sroa_transform(&ctx);
}

void sroa_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        sroa_run_on_function(arena, f);
    }
}