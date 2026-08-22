#include "cfg_opt.h"

#include <string.h>
#include <assert.h>

typedef struct CFGInfo {
    size_t   pred_count;
    IRBlock* single_pred;
    size_t   succ_count;
    IRBlock* succs[2];
    bool     reachable;
} CFGInfo;

static void analyze_cfg(Arena* arena, IRFunction* func, CFGInfo* info, size_t block_count) {
    memset(info, 0, sizeof(CFGInfo) * block_count);

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        if (b->id >= block_count) {
            continue;
        }

        IRInst* term = b->last_inst;

        if (!term) {
            if (b->next_block && b->next_block->id < block_count) {
                info[b->id].succs[info[b->id].succ_count++] = b->next_block;
                info[b->next_block->id].pred_count++;
                info[b->next_block->id].single_pred = b;
            }
            continue;
        }

        if (term->opcode == IR_JMP && term->dst.kind == IR_OP_BLOCK && term->dst.block) {
            IRBlock* target = term->dst.block;

            if (target->id < block_count) {
                info[b->id].succs[info[b->id].succ_count++] = target;
                info[target->id].pred_count++;
                info[target->id].single_pred = (info[target->id].pred_count == 1) ? b : NULL;
            }
        } else if (term->opcode == IR_BR) {
            IRBlock* t1 = (term->src1.kind == IR_OP_BLOCK) ? term->src1.block : NULL;
            IRBlock* t2 = (term->src2.kind == IR_OP_BLOCK) ? term->src2.block : NULL;

            if (t1 && t1->id < block_count) {
                info[b->id].succs[info[b->id].succ_count++] = t1;
                info[t1->id].pred_count++;
                info[t1->id].single_pred = (info[t1->id].pred_count == 1) ? b : NULL;
            }

            if (t2 && t2 != t1 && t2->id < block_count) {
                info[b->id].succs[info[b->id].succ_count++] = t2;
                info[t2->id].pred_count++;
                info[t2->id].single_pred = (info[t2->id].pred_count == 1) ? b : NULL;
            }
        }
    }

    if (func->first_block && func->first_block->id < block_count) {
        IRBlock** queue = ARENA_NEW_ARRAY(arena, IRBlock*, block_count);
        size_t head = 0;
        size_t tail = 0;

        queue[tail++] = func->first_block;
        info[func->first_block->id].reachable = true;

        while (head < tail) {
            IRBlock* curr = queue[head++];

            for (size_t s = 0; s < info[curr->id].succ_count; ++s) {
                IRBlock* succ = info[curr->id].succs[s];

                if (!info[succ->id].reachable) {
                    info[succ->id].reachable = true;
                    queue[tail++] = succ;
                }
            }
        }
    }
}

static void update_phi_incoming_block(IRBlock* target, IRBlock* old_block, IRBlock* new_block) {
    for (IRInst* inst = target->first_inst; inst != NULL && inst->opcode == IR_PHI; inst = inst->next) {
        for (size_t a = 0; a < inst->extra_arg_count; a += 2) {
            if (inst->extra_args[a + 1].block == old_block) {
                inst->extra_args[a + 1].block = new_block;
            }
        }
    }
}

static bool is_empty_trampoline(const IRBlock* b) {
    if (!b || !b->first_inst) {
        return false;
    }

    size_t non_nop_count = 0;
    IRInst* last = NULL;

    for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
        if (inst->opcode == IR_PHI) {
            return false;
        }

        if (inst->opcode != IR_NOP) {
            non_nop_count++;
            last = inst;
        }
    }

    return (non_nop_count == 1 && last != NULL && last->opcode == IR_JMP && last->dst.kind == IR_OP_BLOCK);
}

static bool simplify_trampolines(IRFunction* func, CFGInfo* info, size_t block_count) {
    bool changed = false;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        if (b == func->first_block || !info[b->id].reachable) {
            continue;
        }

        if (is_empty_trampoline(b)) {
            IRBlock* target = b->last_inst->dst.block;

            if (target && target != b && target->id < block_count) {
                for (IRBlock* p = func->first_block; p != NULL; p = p->next_block) {
                    if (p == b || !info[p->id].reachable) {
                        continue;
                    }

                    IRInst* term = p->last_inst;

                    if (!term) {
                        continue;
                    }

                    if (term->opcode == IR_JMP && term->dst.block == b) {
                        term->dst.block = target;
                        update_phi_incoming_block(target, b, p);
                        changed = true;
                    } else if (term->opcode == IR_BR) {
                        if (term->src1.block == b) {
                            term->src1.block = target;
                            update_phi_incoming_block(target, b, p);
                            changed = true;
                        }

                        if (term->src2.block == b) {
                            term->src2.block = target;
                            update_phi_incoming_block(target, b, p);
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    return changed;
}

static bool merge_sequential_blocks(IRFunction* func, CFGInfo* info, size_t block_count) {
    bool changed = false;

    for (IRBlock* a = func->first_block; a != NULL; a = a->next_block) {
        if (!info[a->id].reachable) {
            continue;
        }

        IRInst* term = a->last_inst;

        if (term != NULL && term->opcode == IR_JMP && term->dst.kind == IR_OP_BLOCK) {
            IRBlock* b = term->dst.block;

            if (b && b != a && b != func->first_block && b->id < block_count) {
                if (info[b->id].pred_count == 1 && info[b->id].single_pred == a && info[b->id].reachable) {
                    for (IRInst* inst = b->first_inst; inst != NULL && inst->opcode == IR_PHI; inst = inst->next) {
                        if (inst->extra_arg_count >= 2) {
                            inst->opcode          = IR_MOV;
                            inst->src1            = inst->extra_args[0];
                            inst->src2            = ir_op_none();
                            inst->extra_args      = NULL;
                            inst->extra_arg_count = 0;
                        }
                    }

                    IRInst* prev = NULL;
                    IRInst* curr = a->first_inst;

                    while (curr && curr != term) {
                        prev = curr;
                        curr = curr->next;
                    }

                    if (prev) {
                        prev->next = b->first_inst;
                    } else {
                        a->first_inst = b->first_inst;
                    }

                    if (b->last_inst) {
                        a->last_inst = b->last_inst;
                    } else {
                        a->last_inst = prev;
                    }

                    a->inst_count = (a->inst_count - 1) + b->inst_count;
                    a->is_terminated = b->is_terminated;

                    for (IRBlock* s = func->first_block; s != NULL; s = s->next_block) {
                        update_phi_incoming_block(s, b, a);
                    }

                    b->first_inst = NULL;
                    b->last_inst  = NULL;
                    b->inst_count = 0;
                    info[b->id].reachable = false;

                    changed = true;
                }
            }
        }
    }

    return changed;
}

static bool simplify_branch_targets(IRFunction* func) {
    bool changed = false;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        IRInst* term = b->last_inst;

        if (term != NULL && term->opcode == IR_BR) {
            if (term->src1.kind == IR_OP_BLOCK && term->src2.kind == IR_OP_BLOCK && term->src1.block == term->src2.block) {
                term->opcode = IR_JMP;
                term->dst    = term->src1;
                term->src1   = ir_op_none();
                term->src2   = ir_op_none();
                changed      = true;
            }
        }
    }

    return changed;
}

static void rotate_loops_in_function(IRFunction* func) {
    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        IRInst* term = b->last_inst;

        if (term != NULL && term->opcode == IR_JMP && term->dst.kind == IR_OP_BLOCK) {
            IRBlock* target = term->dst.block;

            if (target && target->last_inst && target->last_inst->opcode == IR_BR) {
                IRInst* br = target->last_inst;

                if (br->src1.block == b || br->src2.block == b) {
                    if (b->next_block != target && target != func->first_block) {
                        IRBlock* prev_target = NULL;

                        for (IRBlock* curr = func->first_block; curr != NULL; curr = curr->next_block) {
                            if (curr->next_block == target) {
                                prev_target = curr;
                                break;
                            }
                        }

                        if (prev_target && target->next_block) {
                            prev_target->next_block = target->next_block;

                            target->next_block = b->next_block;
                            b->next_block      = target;

                            if (func->last_block == b) {
                                func->last_block = target;
                            }
                        }
                    }
                }
            }
        }
    }
}

static void remove_unreachable_blocks(IRFunction* func, const CFGInfo* info, size_t block_count) {
    IRBlock* prev = NULL;
    IRBlock* curr = func->first_block;

    while (curr != NULL) {
        if (curr->id < block_count && !info[curr->id].reachable && curr != func->first_block) {
            for (IRInst* inst = curr->first_inst; inst != NULL; inst = inst->next) {
                inst->opcode = IR_NOP;
            }

            if (prev) {
                prev->next_block = curr->next_block;
            } else {
                func->first_block = curr->next_block;
            }

            if (curr == func->last_block) {
                func->last_block = prev;
            }

            func->block_count--;
            curr = curr->next_block;
        } else {
            prev = curr;
            curr = curr->next_block;
        }
    }
}

void cfg_opt_run_on_function(Arena* arena, IRFunction* func) {
    if (!func || !func->first_block || func->first_block == func->last_block) {
        return;
    }

    uint32_t reindex_id = 0;
    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        b->id = reindex_id++;
    }
    func->next_block_id = reindex_id;

    size_t block_count = func->next_block_id + 64;
    CFGInfo* info = ARENA_NEW_ARRAY_ZERO(arena, CFGInfo, block_count);

    bool changed = true;
    size_t iter = 0;

    while (changed && iter < 16) {
        changed = false;
        iter++;

        analyze_cfg(arena, func, info, block_count);

        if (simplify_branch_targets(func)) {
            changed = true;
        }

        if (simplify_trampolines(func, info, block_count)) {
            changed = true;
        }

        analyze_cfg(arena, func, info, block_count);

        if (merge_sequential_blocks(func, info, block_count)) {
            changed = true;
        }
    }

    rotate_loops_in_function(func);

    analyze_cfg(arena, func, info, block_count);
    remove_unreachable_blocks(func, info, block_count);

    ir_eliminate_nops(func);
}

void cfg_opt_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        cfg_opt_run_on_function(arena, f);
    }
}