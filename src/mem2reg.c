#include "mem2reg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

typedef struct CFGBlock CFGBlock;

struct CFGBlock {
    IRBlock*   block;
    size_t     dense_id;
    size_t     rpo_idx;

    CFGBlock** preds;
    size_t     pred_count;
    size_t     pred_cap;

    CFGBlock** succs;
    size_t     succ_count;
    size_t     succ_cap;

    CFGBlock*  idom;

    CFGBlock** dom_children;
    size_t     dom_child_count;
    size_t     dom_child_cap;

    CFGBlock** df;
    size_t     df_count;
    size_t     df_cap;
};

typedef struct PromotedVar {
    int32_t    stack_offset;
    size_t     byte_size;
    bool       is_signed;
    uint32_t   var_id;

    CFGBlock** def_blocks;
    size_t     def_block_count;
    size_t     def_block_cap;
} PromotedVar;

typedef struct DefStackNode {
    IROperand            val;
    struct DefStackNode* next;
} DefStackNode;

typedef struct Mem2RegCtx {
    Arena*        arena;
    IRFunction*   func;

    CFGBlock**    blocks;
    size_t        block_count;

    CFGBlock**    rpo_blocks;
    size_t        rpo_count;

    PromotedVar*  vars;
    size_t        var_count;

    DefStackNode** def_stacks;

    IROperand*    vreg_subst;
    size_t        vreg_subst_cap;
} Mem2RegCtx;

static void cfg_add_edge(Arena* arena, CFGBlock* from, CFGBlock* to) {
    for (size_t i = 0; i < from->succ_count; ++i) {
        if (from->succs[i] == to) {
            return;
        }
    }

    ARENA_DA_PUSH(arena, from->succs, from->succ_count, from->succ_cap, to);
    ARENA_DA_PUSH(arena, to->preds, to->pred_count, to->pred_cap, from);
}

static void split_critical_edges(Arena* arena, IRFunction* func) {
    (void)arena;
    
    bool changed = true;

    while (changed) {
        changed = false;

        for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
            IRInst* term = b->last_inst;

            if (!term || term->opcode != IR_BR) {
                continue;
            }

            IRBlock* targets[2] = { term->src1.block, term->src2.block };

            for (size_t t = 0; t < 2; ++t) {
                IRBlock* target = targets[t];

                size_t pred_count = 0;

                for (IRBlock* p = func->first_block; p != NULL; p = p->next_block) {
                    IRInst* p_term = p->last_inst;

                    if (!p_term) continue;

                    if (p_term->opcode == IR_JMP && p_term->dst.block == target) {
                        pred_count++;
                    } else if (p_term->opcode == IR_BR && (p_term->src1.block == target || p_term->src2.block == target)) {
                        pred_count++;
                    }
                }

                if (pred_count > 1) {
                    IRBlock* split_bb = ir_block_create(func, "bb_crit_split");
                    ir_block_switch(func, split_bb);
                    ir_emit_inst(func, IR_JMP, ir_op_block(target), ir_op_none(), ir_op_none(), term->loc);

                    if (t == 0) {
                        term->src1 = ir_op_block(split_bb);
                    } else {
                        term->src2 = ir_op_block(split_bb);
                    }

                    changed = true;
                    break;
                }
            }

            if (changed) {
                break;
            }
        }
    }
}

static void rpo_dfs(CFGBlock* b, bool* visited, CFGBlock** post_order, size_t* po_count) {
    visited[b->dense_id] = true;

    for (size_t i = 0; i < b->succ_count; ++i) {
        CFGBlock* succ = b->succs[i];

        if (!visited[succ->dense_id]) {
            rpo_dfs(succ, visited, post_order, po_count);
        }
    }

    post_order[(*po_count)++] = b;
}

static CFGBlock* dom_intersect(CFGBlock* b1, CFGBlock* b2) {
    CFGBlock* finger1 = b1;
    CFGBlock* finger2 = b2;

    while (finger1 != finger2) {
        while (finger1->rpo_idx > finger2->rpo_idx) {
            finger1 = finger1->idom;
        }

        while (finger2->rpo_idx > finger1->rpo_idx) {
            finger2 = finger2->idom;
        }
    }

    return finger1;
}

static void compute_dominance_frontiers(Mem2RegCtx* ctx) {
    for (size_t i = 0; i < ctx->rpo_count; ++i) {
        CFGBlock* b = ctx->rpo_blocks[i];

        if (b->pred_count >= 2) {
            for (size_t p = 0; p < b->pred_count; ++p) {
                CFGBlock* runner = b->preds[p];

                while (runner && runner != b->idom) {
                    bool exists = false;

                    for (size_t d = 0; d < runner->df_count; ++d) {
                        if (runner->df[d] == b) {
                            exists = true;
                            break;
                        }
                    }

                    if (!exists) {
                        ARENA_DA_PUSH(ctx->arena, runner->df, runner->df_count, runner->df_cap, b);
                    }

                    if (runner == runner->idom) {
                        break;
                    }

                    runner = runner->idom;
                }
            }
        }
    }
}

static void def_stack_push(Mem2RegCtx* ctx, uint32_t var_id, IROperand val) {
    DefStackNode* node = ARENA_NEW(ctx->arena, DefStackNode);

    node->val  = val;
    node->next = ctx->def_stacks[var_id];

    ctx->def_stacks[var_id] = node;
}

static IROperand def_stack_top(Mem2RegCtx* ctx, uint32_t var_id) {
    if (ctx->def_stacks[var_id] != NULL) {
        return ctx->def_stacks[var_id]->val;
    }

    PromotedVar* v = &ctx->vars[var_id];
    return ir_op_const(0, v->byte_size, v->is_signed);
}

static void def_stack_pop(Mem2RegCtx* ctx, uint32_t var_id) {
    assert(ctx->def_stacks[var_id] != NULL);
    ctx->def_stacks[var_id] = ctx->def_stacks[var_id]->next;
}

static IROperand resolve_operand(const Mem2RegCtx* ctx, IROperand op) {
    while (op.kind == IR_OP_VREG && op.vreg_id < ctx->vreg_subst_cap) {
        IROperand subst = ctx->vreg_subst[op.vreg_id];

        if (subst.kind == IR_OP_NONE) {
            break;
        }

        op = subst;
    }

    return op;
}

static int32_t find_promoted_var(const Mem2RegCtx* ctx, int32_t stack_offset) {
    for (size_t i = 0; i < ctx->var_count; ++i) {
        if (ctx->vars[i].stack_offset == stack_offset) {
            return (int32_t)i;
        }
    }

    return -1;
}

static void rename_block_ssa(Mem2RegCtx* ctx, CFGBlock* b) {
    size_t* pushes = ARENA_NEW_ARRAY_ZERO(ctx->arena, size_t, ctx->var_count);

    for (IRInst* inst = b->block->first_inst; inst != NULL; inst = inst->next) {
        if (inst->opcode != IR_PHI) {
            continue;
        }

        int32_t var_id = find_promoted_var(ctx, inst->src1.stack_offset);

        if (var_id >= 0) {
            def_stack_push(ctx, (uint32_t)var_id, inst->dst);
            pushes[var_id]++;
        }
    }

    for (IRInst* inst = b->block->first_inst; inst != NULL; inst = inst->next) {
        if (inst->opcode == IR_PHI) {
            continue;
        }

        if (inst->opcode == IR_PARAM && inst->dst.kind == IR_OP_STACK) {
            int32_t var_id = find_promoted_var(ctx, inst->dst.stack_offset);

            if (var_id >= 0) {
                PromotedVar* v = &ctx->vars[var_id];
                uint32_t vreg  = ir_vreg_alloc(ctx->func);
                IROperand new_op = ir_op_vreg(vreg, v->byte_size, v->is_signed);

                inst->dst = new_op;

                def_stack_push(ctx, (uint32_t)var_id, new_op);
                pushes[var_id]++;
            }
        } else if (inst->opcode == IR_MOV && inst->src1.kind == IR_OP_STACK && inst->dst.kind == IR_OP_VREG) {
            int32_t var_id = find_promoted_var(ctx, inst->src1.stack_offset);

            if (var_id >= 0) {
                IROperand val = def_stack_top(ctx, (uint32_t)var_id);
                val = resolve_operand(ctx, val);

                if (inst->dst.vreg_id < ctx->vreg_subst_cap) {
                    ctx->vreg_subst[inst->dst.vreg_id] = val;
                }

                inst->opcode = IR_NOP;
            }
        } else if (inst->opcode == IR_MOV && inst->dst.kind == IR_OP_STACK) {
            int32_t var_id = find_promoted_var(ctx, inst->dst.stack_offset);

            if (var_id >= 0) {
                IROperand val = resolve_operand(ctx, inst->src1);

                def_stack_push(ctx, (uint32_t)var_id, val);
                pushes[var_id]++;

                inst->opcode = IR_NOP;
            }
        } else {
            if (inst->opcode == IR_STORE || inst->opcode == IR_MEMCPY || inst->opcode == IR_BR || inst->opcode == IR_RET) {
                inst->dst = resolve_operand(ctx, inst->dst);
            }

            inst->src1 = resolve_operand(ctx, inst->src1);
            inst->src2 = resolve_operand(ctx, inst->src2);

            for (size_t k = 0; k < inst->extra_arg_count; ++k) {
                inst->extra_args[k] = resolve_operand(ctx, inst->extra_args[k]);
            }
        }
    }

    for (size_t s = 0; s < b->succ_count; ++s) {
        CFGBlock* succ = b->succs[s];

        size_t pred_idx = 0;

        for (size_t p = 0; p < succ->pred_count; ++p) {
            if (succ->preds[p] == b) {
                pred_idx = p;
                break;
            }
        }

        for (IRInst* inst = succ->block->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode != IR_PHI) {
                break;
            }

            int32_t var_id = find_promoted_var(ctx, inst->src1.stack_offset);

            if (var_id >= 0) {
                IROperand val = def_stack_top(ctx, (uint32_t)var_id);
                val = resolve_operand(ctx, val);

                inst->extra_args[2 * pred_idx]     = val;
                inst->extra_args[2 * pred_idx + 1] = ir_op_block(b->block);
            }
        }
    }

    for (size_t c = 0; c < b->dom_child_count; ++c) {
        rename_block_ssa(ctx, b->dom_children[c]);
    }

    for (size_t v = 0; v < ctx->var_count; ++v) {
        while (pushes[v] > 0) {
            def_stack_pop(ctx, (uint32_t)v);
            pushes[v]--;
        }
    }
}

static void eliminate_dead_nops(IRFunction* func) {
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

static void lower_phi_nodes_out_of_ssa(Arena* arena, IRFunction* func) {
    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode != IR_PHI) {
                break;
            }

            for (size_t i = 0; i < inst->extra_arg_count; i += 2) {
                IROperand val        = inst->extra_args[i];
                IRBlock*  pred_block = inst->extra_args[i + 1].block;

                if (!pred_block || val.kind == IR_OP_NONE) {
                    continue;
                }

                IRInst* copy_inst = ARENA_NEW_ZERO(arena, IRInst);

                copy_inst->opcode          = IR_MOV;
                copy_inst->dst             = inst->dst;
                copy_inst->src1            = val;
                copy_inst->src2            = ir_op_none();
                copy_inst->loc             = inst->loc;
                copy_inst->next            = NULL;
                copy_inst->extra_args      = NULL;
                copy_inst->extra_arg_count = 0;

                IRInst* prev = NULL;
                IRInst* curr = pred_block->first_inst;

                while (curr && curr->next && curr->opcode != IR_JMP && curr->opcode != IR_BR && curr->opcode != IR_RET) {
                    prev = curr;
                    curr = curr->next;
                }

                if (curr && (curr->opcode == IR_JMP || curr->opcode == IR_BR || curr->opcode == IR_RET)) {
                    if (prev) {
                        copy_inst->next = curr;
                        prev->next      = copy_inst;
                    } else {
                        copy_inst->next        = pred_block->first_inst;
                        pred_block->first_inst = copy_inst;
                    }
                } else {
                    if (pred_block->last_inst) {
                        pred_block->last_inst->next = copy_inst;
                        pred_block->last_inst       = copy_inst;
                    } else {
                        pred_block->first_inst = copy_inst;
                        pred_block->last_inst  = copy_inst;
                    }
                }

                pred_block->inst_count++;
            }

            inst->opcode = IR_NOP;
        }
    }

    eliminate_dead_nops(func);
}

static void mark_slot_range_escaped(int32_t base_offset, size_t byte_size,
                                    const int32_t* candidate_slots, bool* slot_escaped, size_t slot_count) {
    if (byte_size == 0) {
        byte_size = 8;
    }

    int32_t end_offset = base_offset + (int32_t)byte_size;

    for (size_t s = 0; s < slot_count; ++s) {
        if (candidate_slots[s] >= base_offset && candidate_slots[s] < end_offset) {
            slot_escaped[s] = true;
        }
    }
}

void mem2reg_run_on_function(Arena* arena, IRFunction* func) {
    if (!func || !func->first_block || func->first_block == func->last_block) {
        return;
    }

    split_critical_edges(arena, func);

    size_t slot_cap = 64;
    size_t slot_count = 0;
    int32_t* candidate_slots = ARENA_NEW_ARRAY(arena, int32_t, slot_cap);
    size_t*  slot_sizes      = ARENA_NEW_ARRAY(arena, size_t, slot_cap);
    bool*    slot_signed     = ARENA_NEW_ARRAY(arena, bool, slot_cap);
    bool*    slot_escaped    = ARENA_NEW_ARRAY_ZERO(arena, bool, slot_cap);

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                int32_t off = inst->src1.stack_offset;
                size_t idx = (size_t)-1;

                for (size_t s = 0; s < slot_count; ++s) {
                    if (candidate_slots[s] == off) {
                        idx = s;
                        break;
                    }
                }

                if (idx == (size_t)-1) {
                    idx = slot_count;
                    ARENA_DA_PUSH(arena, candidate_slots, slot_count, slot_cap, off);
                    slot_sizes[idx]  = inst->src1.byte_size;
                    slot_signed[idx] = inst->src1.is_signed;
                }
            }

            if (inst->dst.kind == IR_OP_STACK && inst->dst.stack_offset < 0) {
                int32_t off = inst->dst.stack_offset;
                size_t idx = (size_t)-1;

                for (size_t s = 0; s < slot_count; ++s) {
                    if (candidate_slots[s] == off) {
                        idx = s;
                        break;
                    }
                }

                if (idx == (size_t)-1) {
                    idx = slot_count;
                    ARENA_DA_PUSH(arena, candidate_slots, slot_count, slot_cap, off);
                    slot_sizes[idx]  = inst->dst.byte_size;
                    slot_signed[idx] = inst->dst.is_signed;
                }
            }
        }
    }

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_ADDR && inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                mark_slot_range_escaped(inst->src1.stack_offset, inst->src1.byte_size,
                                        candidate_slots, slot_escaped, slot_count);
            }

            if (inst->opcode == IR_MEMCPY) {
                size_t cpy_size = (inst->src2.kind == IR_OP_CONST) ? (size_t)inst->src2.int_val : 8;

                if (inst->dst.kind == IR_OP_STACK && inst->dst.stack_offset < 0) {
                    mark_slot_range_escaped(inst->dst.stack_offset, cpy_size,
                                            candidate_slots, slot_escaped, slot_count);
                }
                if (inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                    mark_slot_range_escaped(inst->src1.stack_offset, cpy_size,
                                            candidate_slots, slot_escaped, slot_count);
                }
            }
        }
    }

    size_t promo_count = 0;

    for (size_t s = 0; s < slot_count; ++s) {
        if (!slot_escaped[s] && slot_sizes[s] > 0 && slot_sizes[s] <= 8) {
            promo_count++;
        }
    }

    if (promo_count == 0) {
        return;
    }

    PromotedVar* vars = ARENA_NEW_ARRAY(arena, PromotedVar, promo_count);
    size_t v_idx = 0;

    for (size_t s = 0; s < slot_count; ++s) {
        if (!slot_escaped[s] && slot_sizes[s] > 0 && slot_sizes[s] <= 8) {
            vars[v_idx].stack_offset    = candidate_slots[s];
            vars[v_idx].byte_size       = slot_sizes[s];
            vars[v_idx].is_signed       = slot_signed[s];
            vars[v_idx].var_id          = (uint32_t)v_idx;
            vars[v_idx].def_blocks      = NULL;
            vars[v_idx].def_block_count = 0;
            vars[v_idx].def_block_cap   = 0;
            v_idx++;
        }
    }

    size_t block_count = func->block_count;
    CFGBlock** cfg_blocks = ARENA_NEW_ARRAY(arena, CFGBlock*, block_count);
    size_t b_idx = 0;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        CFGBlock* cb = ARENA_NEW_ZERO(arena, CFGBlock);

        cb->block           = b;
        cb->dense_id        = b_idx;
        cb->rpo_idx         = 0;
        cb->preds           = NULL;
        cb->pred_count      = 0;
        cb->pred_cap        = 0;
        cb->succs           = NULL;
        cb->succ_count      = 0;
        cb->succ_cap        = 0;
        cb->idom            = NULL;
        cb->dom_children    = NULL;
        cb->dom_child_count = 0;
        cb->dom_child_cap   = 0;
        cb->df              = NULL;
        cb->df_count        = 0;
        cb->df_cap          = 0;

        cfg_blocks[b_idx++] = cb;
    }

    for (size_t i = 0; i < block_count; ++i) {
        CFGBlock* cb = cfg_blocks[i];
        IRInst* term = cb->block->last_inst;

        if (!term) {
            if (cb->block->next_block) {
                for (size_t j = 0; j < block_count; ++j) {
                    if (cfg_blocks[j]->block == cb->block->next_block) {
                        cfg_add_edge(arena, cb, cfg_blocks[j]);
                        break;
                    }
                }
            }
            continue;
        }

        if (term->opcode == IR_JMP) {
            for (size_t j = 0; j < block_count; ++j) {
                if (cfg_blocks[j]->block == term->dst.block) {
                    cfg_add_edge(arena, cb, cfg_blocks[j]);
                    break;
                }
            }
        } else if (term->opcode == IR_BR) {
            for (size_t j = 0; j < block_count; ++j) {
                if (cfg_blocks[j]->block == term->src1.block || cfg_blocks[j]->block == term->src2.block) {
                    cfg_add_edge(arena, cb, cfg_blocks[j]);
                }
            }
        }
    }

    bool* visited = ARENA_NEW_ARRAY_ZERO(arena, bool, block_count);
    CFGBlock** post_order = ARENA_NEW_ARRAY(arena, CFGBlock*, block_count);
    size_t po_count = 0;

    rpo_dfs(cfg_blocks[0], visited, post_order, &po_count);

    CFGBlock** rpo_blocks = ARENA_NEW_ARRAY(arena, CFGBlock*, po_count);

    for (size_t i = 0; i < po_count; ++i) {
        rpo_blocks[i]          = post_order[po_count - 1 - i];
        rpo_blocks[i]->rpo_idx = i;
    }

    rpo_blocks[0]->idom = rpo_blocks[0];
    bool dom_changed    = true;

    while (dom_changed) {
        dom_changed = false;

        for (size_t i = 1; i < po_count; ++i) {
            CFGBlock* b = rpo_blocks[i];
            CFGBlock* new_idom = NULL;

            for (size_t p = 0; p < b->pred_count; ++p) {
                CFGBlock* pred = b->preds[p];

                if (pred->idom != NULL) {
                    if (!new_idom) {
                        new_idom = pred;
                    } else {
                        new_idom = dom_intersect(pred, new_idom);
                    }
                }
            }

            if (new_idom && b->idom != new_idom) {
                b->idom     = new_idom;
                dom_changed = true;
            }
        }
    }

    for (size_t i = 1; i < po_count; ++i) {
        CFGBlock* b = rpo_blocks[i];
        if (b->idom && b->idom != b) {
            ARENA_DA_PUSH(arena, b->idom->dom_children, b->idom->dom_child_count, b->idom->dom_child_cap, b);
        }
    }

    Mem2RegCtx ctx = {
        .arena          = arena,
        .func           = func,
        .blocks         = cfg_blocks,
        .block_count    = block_count,
        .rpo_blocks     = rpo_blocks,
        .rpo_count      = po_count,
        .vars           = vars,
        .var_count      = promo_count,
        .def_stacks     = ARENA_NEW_ARRAY_ZERO(arena, DefStackNode*, promo_count),
        .vreg_subst     = ARENA_NEW_ARRAY_ZERO(arena, IROperand, func->next_vreg_id + 1024),
        .vreg_subst_cap = func->next_vreg_id + 1024
    };

    compute_dominance_frontiers(&ctx);

    for (size_t v = 0; v < promo_count; ++v) {
        for (size_t i = 0; i < block_count; ++i) {
            CFGBlock* cb = cfg_blocks[i];

            for (IRInst* inst = cb->block->first_inst; inst != NULL; inst = inst->next) {
                if ((inst->opcode == IR_MOV || inst->opcode == IR_PARAM) &&
                    inst->dst.kind == IR_OP_STACK &&
                    inst->dst.stack_offset == vars[v].stack_offset) {

                    ARENA_DA_PUSH(arena, vars[v].def_blocks, vars[v].def_block_count, vars[v].def_block_cap, cb);
                    break;
                }
            }
        }
    }

    for (size_t v = 0; v < promo_count; ++v) {
        CFGBlock** work_queue = ARENA_NEW_ARRAY(arena, CFGBlock*, block_count * 2);
        size_t w_head = 0;
        size_t w_tail = 0;

        bool* has_phi = ARENA_NEW_ARRAY_ZERO(arena, bool, block_count);
        bool* in_work = ARENA_NEW_ARRAY_ZERO(arena, bool, block_count);

        for (size_t d = 0; d < vars[v].def_block_count; ++d) {
            work_queue[w_tail++] = vars[v].def_blocks[d];
            in_work[vars[v].def_blocks[d]->dense_id] = true;
        }

        while (w_head < w_tail) {
            CFGBlock* x = work_queue[w_head++];

            for (size_t f = 0; f < x->df_count; ++f) {
                CFGBlock* y = x->df[f];

                if (!has_phi[y->dense_id]) {
                    has_phi[y->dense_id] = true;

                    uint32_t phi_vreg = ir_vreg_alloc(func);
                    IRInst* phi_inst  = ARENA_NEW_ZERO(arena, IRInst);

                    phi_inst->opcode          = IR_PHI;
                    phi_inst->dst             = ir_op_vreg(phi_vreg, vars[v].byte_size, vars[v].is_signed);
                    phi_inst->src1            = ir_op_stack(vars[v].stack_offset, vars[v].byte_size, vars[v].is_signed);
                    phi_inst->src2            = ir_op_none();
                    phi_inst->loc             = y->block->first_inst ? y->block->first_inst->loc : (SourceLoc){0};
                    phi_inst->extra_arg_count = 2 * y->pred_count;
                    phi_inst->extra_args      = ARENA_NEW_ARRAY_ZERO(arena, IROperand, phi_inst->extra_arg_count);

                    phi_inst->next        = y->block->first_inst;
                    y->block->first_inst  = phi_inst;

                    if (!y->block->last_inst) {
                        y->block->last_inst = phi_inst;
                    }

                    y->block->inst_count++;

                    if (!in_work[y->dense_id]) {
                        in_work[y->dense_id] = true;
                        work_queue[w_tail++] = y;
                    }
                }
            }
        }
    }

    if (func->next_vreg_id >= ctx.vreg_subst_cap) {
        ctx.vreg_subst_cap = func->next_vreg_id + 1024;
        ctx.vreg_subst     = ARENA_NEW_ARRAY_ZERO(arena, IROperand, ctx.vreg_subst_cap);
    }

    rename_block_ssa(&ctx, rpo_blocks[0]);

    eliminate_dead_nops(func);

    lower_phi_nodes_out_of_ssa(arena, func);
}

void mem2reg_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        mem2reg_run_on_function(arena, f);
    }
}