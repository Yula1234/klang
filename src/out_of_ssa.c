#include "out_of_ssa.h"

#include <assert.h>
#include <string.h>

typedef struct PendingCopy {
    IROperand dst;
    IROperand src;
    SourceLoc loc;
    bool      done;
} PendingCopy;

typedef struct EdgeSplit {
    IRBlock*  pred;
    IRBlock*  target;
    IRBlock*  split_bb;
} EdgeSplit;

static bool operand_equals(IROperand a, IROperand b) {
    if (a.kind != b.kind) {
        return false;
    }

    if (a.kind == IR_OP_VREG) {
        return a.vreg_id == b.vreg_id;
    }

    if (a.kind == IR_OP_REG) {
        return a.reg == b.reg;
    }

    if (a.kind == IR_OP_STACK) {
        return a.stack_offset == b.stack_offset;
    }

    if (a.kind == IR_OP_CONST) {
        return a.int_val == b.int_val;
    }

    return false;
}

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

static void emit_parallel_copies(Arena* arena, IRFunction* func, IRBlock* insert_block, PendingCopy* copies, size_t count) {
    size_t remaining = 0;

    for (size_t i = 0; i < count; ++i) {
        if (operand_equals(copies[i].dst, copies[i].src)) {
            copies[i].done = true;
        } else {
            copies[i].done = false;
            remaining++;
        }
    }

    while (remaining > 0) {
        bool progress = false;

        for (size_t i = 0; i < count; ++i) {
            if (copies[i].done) {
                continue;
            }

            bool dst_used_as_src = false;

            for (size_t j = 0; j < count; ++j) {
                if (!copies[j].done && i != j && operand_equals(copies[j].src, copies[i].dst)) {
                    dst_used_as_src = true;
                    break;
                }
            }

            if (!dst_used_as_src) {
                IRInst* mov = ARENA_NEW_ZERO(arena, IRInst);

                mov->opcode = IR_MOV;
                mov->dst    = copies[i].dst;
                mov->src1   = copies[i].src;
                mov->loc    = copies[i].loc;

                insert_inst_before_terminator(insert_block, mov);

                copies[i].done = true;
                remaining--;
                progress = true;
                break;
            }
        }

        if (!progress && remaining > 0) {
            size_t cycle_idx = (size_t)-1;

            for (size_t i = 0; i < count; ++i) {
                if (!copies[i].done) {
                    cycle_idx = i;
                    break;
                }
            }

            assert(cycle_idx != (size_t)-1);

            uint32_t tmp_vid = ir_vreg_alloc(func);
            IROperand tmp_op = ir_op_vreg(tmp_vid, copies[cycle_idx].dst.byte_size, copies[cycle_idx].dst.is_signed);

            IRInst* save_mov = ARENA_NEW_ZERO(arena, IRInst);

            save_mov->opcode = IR_MOV;
            save_mov->dst    = tmp_op;
            save_mov->src1   = copies[cycle_idx].dst;
            save_mov->loc    = copies[cycle_idx].loc;

            insert_inst_before_terminator(insert_block, save_mov);

            for (size_t j = 0; j < count; ++j) {
                if (!copies[j].done && operand_equals(copies[j].src, copies[cycle_idx].dst)) {
                    copies[j].src = tmp_op;
                }
            }

            IRInst* exec_mov = ARENA_NEW_ZERO(arena, IRInst);

            exec_mov->opcode = IR_MOV;
            exec_mov->dst    = copies[cycle_idx].dst;
            exec_mov->src1   = copies[cycle_idx].src;
            exec_mov->loc    = copies[cycle_idx].loc;

            insert_inst_before_terminator(insert_block, exec_mov);

            copies[cycle_idx].done = true;
            remaining--;
        }
    }
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

            PendingCopy* copies = ARENA_NEW_ARRAY(arena, PendingCopy, phi_count);
            size_t copy_count = 0;

            for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
                if (inst->opcode != IR_PHI) {
                    continue;
                }

                IROperand val = inst->extra_args[2 * p];

                if (val.kind != IR_OP_NONE) {
                    copies[copy_count].dst  = inst->dst;
                    copies[copy_count].src  = val;
                    copies[copy_count].loc  = inst->loc;
                    copies[copy_count].done = false;
                    copy_count++;
                }
            }

            emit_parallel_copies(arena, func, insert_block, copies, copy_count);
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