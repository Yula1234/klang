#include "licm.h"

#include <string.h>
#include <assert.h>

typedef struct LICMBlock LICMBlock;

struct LICMBlock {
    IRBlock*    block;
    size_t      dense_id;
    size_t      rpo_idx;

    LICMBlock** preds;
    size_t      pred_count;
    size_t      pred_cap;

    LICMBlock** succs;
    size_t      succ_count;
    size_t      succ_cap;

    LICMBlock*  idom;

    LICMBlock** dom_children;
    size_t      dom_child_count;
    size_t      dom_child_cap;
};

typedef struct NaturalLoop {
    LICMBlock*          header;
    LICMBlock*          preheader;
    LICMBlock**         blocks;
    size_t              block_count;
    size_t              block_cap;
    LICMBlock**         latches;
    size_t              latch_count;
    size_t              latch_cap;
    struct NaturalLoop* next;
} NaturalLoop;

typedef struct LICMContext {
    Arena*        arena;
    IRFunction*   func;

    LICMBlock**   blocks;
    size_t        block_count;

    LICMBlock**   rpo_blocks;
    size_t        rpo_count;

    NaturalLoop*  loops;
    size_t        vreg_cap;
} LICMContext;

static void add_cfg_edge(Arena* arena, LICMBlock* from, LICMBlock* to) {
    for (size_t i = 0; i < from->succ_count; ++i) {
        if (from->succs[i] == to) {
            return;
        }
    }

    ARENA_DA_PUSH(arena, from->succs, from->succ_count, from->succ_cap, to);
    ARENA_DA_PUSH(arena, to->preds, to->pred_count, to->pred_cap, from);
}

static void rpo_dfs(LICMBlock* b, bool* visited, LICMBlock** post_order, size_t* po_count) {
    visited[b->dense_id] = true;

    for (size_t i = 0; i < b->succ_count; ++i) {
        LICMBlock* succ = b->succs[i];

        if (!visited[succ->dense_id]) {
            rpo_dfs(succ, visited, post_order, po_count);
        }
    }

    post_order[(*po_count)++] = b;
}

static LICMBlock* dom_intersect(LICMBlock* b1, LICMBlock* b2) {
    LICMBlock* finger1 = b1;
    LICMBlock* finger2 = b2;

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

static bool dominates(const LICMBlock* a, const LICMBlock* b) {
    const LICMBlock* curr = b;

    while (curr != NULL) {
        if (curr == a) {
            return true;
        }

        if (curr->idom == curr) {
            break;
        }

        curr = curr->idom;
    }

    return false;
}

static void build_cfg_and_dom_tree(LICMContext* ctx) {
    IRFunction* func = ctx->func;
    size_t count = func->block_count;

    ctx->blocks = ARENA_NEW_ARRAY(ctx->arena, LICMBlock*, count);
    size_t idx = 0;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        LICMBlock* lb = ARENA_NEW_ZERO(ctx->arena, LICMBlock);

        lb->block    = b;
        lb->dense_id = idx;

        ctx->blocks[idx++] = lb;
    }

    ctx->block_count = count;

    for (size_t i = 0; i < count; ++i) {
        LICMBlock* lb = ctx->blocks[i];
        IRInst* term = lb->block->last_inst;

        if (!term) {
            if (lb->block->next_block) {
                for (size_t j = 0; j < count; ++j) {
                    if (ctx->blocks[j]->block == lb->block->next_block) {
                        add_cfg_edge(ctx->arena, lb, ctx->blocks[j]);
                        break;
                    }
                }
            }
            continue;
        }

        if (term->opcode == IR_JMP) {
            for (size_t j = 0; j < count; ++j) {
                if (ctx->blocks[j]->block == term->dst.block) {
                    add_cfg_edge(ctx->arena, lb, ctx->blocks[j]);
                    break;
                }
            }
        } else if (term->opcode == IR_BR) {
            for (size_t j = 0; j < count; ++j) {
                if (ctx->blocks[j]->block == term->src1.block || ctx->blocks[j]->block == term->src2.block) {
                    add_cfg_edge(ctx->arena, lb, ctx->blocks[j]);
                }
            }
        }
    }

    bool* visited = ARENA_NEW_ARRAY_ZERO(ctx->arena, bool, count);
    LICMBlock** post_order = ARENA_NEW_ARRAY(ctx->arena, LICMBlock*, count);
    size_t po_count = 0;

    rpo_dfs(ctx->blocks[0], visited, post_order, &po_count);

    ctx->rpo_blocks = ARENA_NEW_ARRAY(ctx->arena, LICMBlock*, po_count);
    ctx->rpo_count  = po_count;

    for (size_t i = 0; i < po_count; ++i) {
        ctx->rpo_blocks[i]          = post_order[po_count - 1 - i];
        ctx->rpo_blocks[i]->rpo_idx = i;
    }

    ctx->rpo_blocks[0]->idom = ctx->rpo_blocks[0];
    bool changed = true;

    while (changed) {
        changed = false;

        for (size_t i = 1; i < po_count; ++i) {
            LICMBlock* b = ctx->rpo_blocks[i];
            LICMBlock* new_idom = NULL;

            for (size_t p = 0; p < b->pred_count; ++p) {
                LICMBlock* pred = b->preds[p];

                if (pred->idom != NULL) {
                    if (!new_idom) {
                        new_idom = pred;
                    } else {
                        new_idom = dom_intersect(pred, new_idom);
                    }
                }
            }

            if (new_idom && b->idom != new_idom) {
                b->idom = new_idom;
                changed = true;
            }
        }
    }

    for (size_t i = 1; i < po_count; ++i) {
        LICMBlock* b = ctx->rpo_blocks[i];
        if (b->idom && b->idom != b) {
            ARENA_DA_PUSH(ctx->arena, b->idom->dom_children, b->idom->dom_child_count, b->idom->dom_child_cap, b);
        }
    }
}

static bool loop_contains_block(const NaturalLoop* loop, const LICMBlock* block) {
    for (size_t i = 0; i < loop->block_count; ++i) {
        if (loop->blocks[i] == block) {
            return true;
        }
    }

    return false;
}

static void discover_loop_body(LICMContext* ctx, NaturalLoop* loop, LICMBlock* latch) {
    ARENA_DA_PUSH(ctx->arena, loop->latches, loop->latch_count, loop->latch_cap, latch);

    if (!loop_contains_block(loop, loop->header)) {
        ARENA_DA_PUSH(ctx->arena, loop->blocks, loop->block_count, loop->block_cap, loop->header);
    }

    if (!loop_contains_block(loop, latch)) {
        ARENA_DA_PUSH(ctx->arena, loop->blocks, loop->block_count, loop->block_cap, latch);
    }

    LICMBlock** work_stack = ARENA_NEW_ARRAY(ctx->arena, LICMBlock*, ctx->block_count);
    size_t stack_size = 0;

    work_stack[stack_size++] = latch;

    while (stack_size > 0) {
        LICMBlock* curr = work_stack[--stack_size];

        for (size_t p = 0; p < curr->pred_count; ++p) {
            LICMBlock* pred = curr->preds[p];

            if (pred != loop->header && !loop_contains_block(loop, pred)) {
                ARENA_DA_PUSH(ctx->arena, loop->blocks, loop->block_count, loop->block_cap, pred);
                work_stack[stack_size++] = pred;
            }
        }
    }
}

static void find_natural_loops(LICMContext* ctx) {
    for (size_t i = 0; i < ctx->rpo_count; ++i) {
        LICMBlock* b = ctx->rpo_blocks[i];

        for (size_t s = 0; s < b->succ_count; ++s) {
            LICMBlock* succ = b->succs[s];

            if (dominates(succ, b)) {
                NaturalLoop* loop = NULL;

                for (NaturalLoop* l = ctx->loops; l != NULL; l = l->next) {
                    if (l->header == succ) {
                        loop = l;
                        break;
                    }
                }

                if (!loop) {
                    loop = ARENA_NEW_ZERO(ctx->arena, NaturalLoop);

                    loop->header    = succ;
                    loop->preheader = NULL;
                    loop->next      = ctx->loops;

                    ctx->loops = loop;
                }

                discover_loop_body(ctx, loop, b);
            }
        }
    }
}

static void create_loop_preheader(LICMContext* ctx, NaturalLoop* loop) {
    IRFunction* func = ctx->func;
    LICMBlock* header = loop->header;

    size_t outside_count = 0;

    for (size_t p = 0; p < header->pred_count; ++p) {
        if (!loop_contains_block(loop, header->preds[p])) {
            outside_count++;
        }
    }

    if (outside_count == 1) {
        for (size_t p = 0; p < header->pred_count; ++p) {
            LICMBlock* pred = header->preds[p];

            if (!loop_contains_block(loop, pred) && pred->succ_count == 1) {
                loop->preheader = pred;
                return;
            }
        }
    }

    IRBlock* pre_bb = ir_block_create(func, "bb_loop_preheader");
    ir_block_switch(func, pre_bb);
    ir_emit_inst(func, IR_JMP, ir_op_block(header->block), ir_op_none(), ir_op_none(), (SourceLoc){0});

    LICMBlock* pre_lb = ARENA_NEW_ZERO(ctx->arena, LICMBlock);
    pre_lb->block    = pre_bb;
    pre_lb->dense_id = ctx->block_count++;

    for (size_t p = 0; p < header->pred_count; ++p) {
        LICMBlock* pred = header->preds[p];

        if (!loop_contains_block(loop, pred)) {
            IRInst* term = pred->block->last_inst;

            if (term != NULL) {
                if (term->opcode == IR_JMP && term->dst.block == header->block) {
                    term->dst = ir_op_block(pre_bb);
                } else if (term->opcode == IR_BR) {
                    if (term->src1.block == header->block) {
                        term->src1 = ir_op_block(pre_bb);
                    }
                    if (term->src2.block == header->block) {
                        term->src2 = ir_op_block(pre_bb);
                    }
                }
            }

            for (IRInst* inst = header->block->first_inst; inst != NULL && inst->opcode == IR_PHI; inst = inst->next) {
                for (size_t a = 0; a < inst->extra_arg_count; a += 2) {
                    if (inst->extra_args[a + 1].block == pred->block) {
                        inst->extra_args[a + 1] = ir_op_block(pre_bb);
                    }
                }
            }
        }
    }

    loop->preheader = pre_lb;
}

static bool is_licm_candidate(IROpcode op) {
    switch (op) {
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_AND:
        case IR_OR:
        case IR_XOR:
        case IR_SHL:
        case IR_SHR:
        case IR_NEG:
        case IR_NOT:
        case IR_CMP_EQ:
        case IR_CMP_NE:
        case IR_CMP_LT:
        case IR_CMP_LE:
        case IR_CMP_GT:
        case IR_CMP_GE:
        case IR_MOV:
        case IR_ADDR:
        case IR_GLOBAL_STR:
            return true;

        case IR_DIV:
        case IR_MOD:
            return true;

        default:
            return false;
    }
}

static bool is_operand_loop_invariant(IROperand op, const bool* defined_in_loop, const bool* invariant_vregs, size_t vreg_cap) {
    if (op.kind == IR_OP_CONST || op.kind == IR_OP_GLOBAL || op.kind == IR_OP_STR || op.kind == IR_OP_NONE) {
        return true;
    }

    if (op.kind == IR_OP_VREG) {
        if (op.vreg_id >= vreg_cap) {
            return false;
        }

        if (!defined_in_loop[op.vreg_id]) {
            return true;
        }

        return invariant_vregs[op.vreg_id];
    }

    return false;
}

static void insert_before_terminator(IRBlock* block, IRInst* inst) {
    IRInst* prev = NULL;
    IRInst* curr = block->first_inst;

    while (curr && curr->next && curr->opcode != IR_JMP && curr->opcode != IR_BR && curr->opcode != IR_RET) {
        prev = curr;
        curr = curr->next;
    }

    if (curr && (curr->opcode == IR_JMP || curr->opcode == IR_BR || curr->opcode == IR_RET)) {
        if (prev) {
            inst->next = curr;
            prev->next = inst;
        } else {
            inst->next         = block->first_inst;
            block->first_inst  = inst;
        }
    } else {
        if (block->last_inst) {
            block->last_inst->next = inst;
            block->last_inst       = inst;
        } else {
            block->first_inst = inst;
            block->last_inst  = inst;
        }
    }

    block->inst_count++;
}

static inline bool inst_dst_is_read(IROpcode op) {
    return op == IR_STORE || op == IR_MEMCPY || op == IR_BR || op == IR_RET;
}

static void optimize_loop_licm(LICMContext* ctx, NaturalLoop* loop) {
    if (!loop->preheader) {
        return;
    }

    size_t vreg_cap = ctx->vreg_cap;
    bool* defined_in_loop = ARENA_NEW_ARRAY_ZERO(ctx->arena, bool, vreg_cap);
    bool* invariant_vregs = ARENA_NEW_ARRAY_ZERO(ctx->arena, bool, vreg_cap);

    for (size_t b = 0; b < loop->block_count; ++b) {
        IRBlock* ib = loop->blocks[b]->block;

        for (IRInst* inst = ib->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP || inst_dst_is_read(inst->opcode)) {
                continue;
            }

            if (inst->dst.kind == IR_OP_VREG && inst->dst.vreg_id < vreg_cap) {
                defined_in_loop[inst->dst.vreg_id] = true;
            }
        }
    }

    bool changed = true;

    while (changed) {
        changed = false;

        for (size_t b = 0; b < loop->block_count; ++b) {
            IRBlock* ib = loop->blocks[b]->block;

            for (IRInst* inst = ib->first_inst; inst != NULL; inst = inst->next) {
                if (inst->opcode == IR_NOP || inst->opcode == IR_PHI || !is_licm_candidate(inst->opcode)) {
                    continue;
                }

                if (inst_dst_is_read(inst->opcode) || inst->dst.kind != IR_OP_VREG || inst->dst.vreg_id >= vreg_cap) {
                    continue;
                }

                if (invariant_vregs[inst->dst.vreg_id]) {
                    continue;
                }

                if ((inst->opcode == IR_DIV || inst->opcode == IR_MOD) && (inst->src2.kind != IR_OP_CONST || inst->src2.int_val == 0)) {
                    continue;
                }

                bool s1_inv = is_operand_loop_invariant(inst->src1, defined_in_loop, invariant_vregs, vreg_cap);
                bool s2_inv = is_operand_loop_invariant(inst->src2, defined_in_loop, invariant_vregs, vreg_cap);

                if (s1_inv && s2_inv) {
                    invariant_vregs[inst->dst.vreg_id] = true;
                    changed = true;
                }
            }
        }
    }

    for (size_t b = 0; b < loop->block_count; ++b) {
        IRBlock* ib = loop->blocks[b]->block;

        for (IRInst* inst = ib->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP || inst->opcode == IR_PHI || !is_licm_candidate(inst->opcode)) {
                continue;
            }

            if (inst_dst_is_read(inst->opcode) || inst->dst.kind != IR_OP_VREG || inst->dst.vreg_id >= vreg_cap) {
                continue;
            }

            if (invariant_vregs[inst->dst.vreg_id]) {
                IRInst* hoisted = ARENA_NEW(ctx->arena, IRInst);
                *hoisted = *inst;
                hoisted->next = NULL;

                insert_before_terminator(loop->preheader->block, hoisted);

                inst->opcode = IR_NOP;
            }
        }
    }
}

void licm_run_on_function(Arena* arena, IRFunction* func) {
    if (!func || !func->first_block || func->first_block == func->last_block) {
        return;
    }

    size_t vreg_cap = func->next_vreg_id + 1024;

    LICMContext ctx = {
        .arena       = arena,
        .func        = func,
        .blocks      = NULL,
        .block_count = 0,
        .rpo_blocks  = NULL,
        .rpo_count   = 0,
        .loops       = NULL,
        .vreg_cap    = vreg_cap
    };

    build_cfg_and_dom_tree(&ctx);
    find_natural_loops(&ctx);

    for (NaturalLoop* l = ctx.loops; l != NULL; l = l->next) {
        create_loop_preheader(&ctx, l);
    }

    for (NaturalLoop* l = ctx.loops; l != NULL; l = l->next) {
        optimize_loop_licm(&ctx, l);
    }

    ir_eliminate_nops(func);
}

void licm_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        licm_run_on_function(arena, f);
    }
}