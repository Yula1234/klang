#include "out_of_ssa.h"

#include <assert.h>

typedef struct PhiCopy {
    IROperand dst;
    IROperand src;
    IROperand tmp;
    SourceLoc loc;
} PhiCopy;

typedef struct EdgeSplit {
    IRBlock*  pred;
    IRBlock*  target;
    IRBlock*  split_bb;
} EdgeSplit;

static IRBlock* get_or_create_split_block(Arena* arena, IRFunction* func, IRBlock* pred, IRBlock* target, EdgeSplit** splits, size_t* split_count, size_t* split_cap) {
    for (size_t i = 0; i < *split_count; ++i) {
        if ((*splits)[i].pred == pred && (*splits)[i].target == target) {
            return (*splits)[i].split_bb;
        }
    }

    bool needs_split = false;
    IRInst* term = pred->last_inst;

    if (term != NULL && term->opcode == IR_BR) {
        needs_split = true;
    }

    if (!needs_split) {
        return pred;
    }

    IRBlock* split_bb = ir_block_create(func, "bb_phi_split");
    ir_block_switch(func, split_bb);
    ir_emit_inst(func, IR_JMP, ir_op_block(target), ir_op_none(), ir_op_none(), term->loc);

    if (term->opcode == IR_BR) {
        if (term->src1.block == target) {
            term->src1 = ir_op_block(split_bb);
        }
        if (term->src2.block == target) {
            term->src2 = ir_op_block(split_bb);
        }
    }

    EdgeSplit split = {
        .pred     = pred,
        .target   = target,
        .split_bb = split_bb
    };

    ARENA_DA_PUSH(arena, *splits, *split_count, *split_cap, split);

    return split_bb;
}

static void insert_inst_before_terminator(IRBlock* block, IRInst* new_inst) {
    assert(block != NULL && new_inst != NULL);

    IRInst* prev = NULL;
    IRInst* curr = block->first_inst;

    while (curr && curr->next && curr->opcode != IR_JMP && curr->opcode != IR_BR && curr->opcode != IR_RET) {
        prev = curr;
        curr = curr->next;
    }

    if (curr && (curr->opcode == IR_JMP || curr->opcode == IR_BR || curr->opcode == IR_RET)) {
        if (prev) {
            new_inst->next = curr;
            prev->next     = new_inst;
        } else {
            new_inst->next    = block->first_inst;
            block->first_inst = new_inst;
        }
    } else {
        if (block->last_inst) {
            block->last_inst->next = new_inst;
            block->last_inst       = new_inst;
        } else {
            block->first_inst = new_inst;
            block->last_inst  = new_inst;
        }
    }

    block->inst_count++;
}

void out_of_ssa_run_on_function(Arena* arena, IRFunction* func) {
    if (!func || !func->first_block) {
        return;
    }

    EdgeSplit* splits = NULL;
    size_t split_count = 0;
    size_t split_cap = 0;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        size_t phi_count = 0;
        IRInst* first_phi = NULL;

        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_PHI) {
                if (!first_phi) {
                    first_phi = inst;
                }
                phi_count++;
            }
        }

        if (phi_count == 0 || !first_phi) {
            continue;
        }

        size_t pred_count = first_phi->extra_arg_count / 2;

        for (size_t p = 0; p < pred_count; ++p) {
            IRBlock* pred_block = first_phi->extra_args[2 * p + 1].block;
            if (!pred_block) {
                continue;
            }

            IRBlock* insert_block = get_or_create_split_block(arena, func, pred_block, b, &splits, &split_count, &split_cap);

            PhiCopy* copies = ARENA_NEW_ARRAY(arena, PhiCopy, phi_count);
            size_t valid_copies = 0;

            for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
                if (inst->opcode != IR_PHI) {
                    continue;
                }

                IROperand val = inst->extra_args[2 * p];

                if (val.kind != IR_OP_NONE) {
                    uint32_t tmp_vreg = ir_vreg_alloc(func);

                    copies[valid_copies].dst = inst->dst;
                    copies[valid_copies].src = val;
                    copies[valid_copies].tmp = ir_op_vreg(tmp_vreg, inst->dst.byte_size, inst->dst.is_signed);
                    copies[valid_copies].loc = inst->loc;

                    valid_copies++;
                }
            }

            for (size_t c = 0; c < valid_copies; ++c) {
                IRInst* move_to_tmp = ARENA_NEW_ZERO(arena, IRInst);

                move_to_tmp->opcode = IR_MOV;
                move_to_tmp->dst    = copies[c].tmp;
                move_to_tmp->src1   = copies[c].src;
                move_to_tmp->loc    = copies[c].loc;

                insert_inst_before_terminator(insert_block, move_to_tmp);
            }

            for (size_t c = 0; c < valid_copies; ++c) {
                IRInst* move_to_dst = ARENA_NEW_ZERO(arena, IRInst);

                move_to_dst->opcode = IR_MOV;
                move_to_dst->dst    = copies[c].dst;
                move_to_dst->src1   = copies[c].tmp;
                move_to_dst->loc    = copies[c].loc;

                insert_inst_before_terminator(insert_block, move_to_dst);
            }
        }

        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_PHI) {
                inst->opcode = IR_NOP;
            }
        }
    }

    ir_eliminate_nops(func);
}

void out_of_ssa_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        out_of_ssa_run_on_function(arena, f);
    }
}