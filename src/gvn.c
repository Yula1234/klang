#include "gvn.h"

#include <string.h>
#include <assert.h>

#define GVN_TABLE_SIZE 1024

typedef struct GVNDomBlock GVNDomBlock;

struct GVNDomBlock {
    IRBlock*      block;
    size_t        dense_id;
    size_t        rpo_idx;

    GVNDomBlock** preds;
    size_t        pred_count;
    size_t        pred_cap;

    GVNDomBlock** succs;
    size_t        succ_count;
    size_t        succ_cap;

    GVNDomBlock*  idom;

    GVNDomBlock** children;
    size_t        child_count;
    size_t        child_cap;
};

typedef struct GVNEntry {
    IROpcode         opcode;
    IROperand        src1;
    IROperand        src2;
    size_t           byte_size;
    bool             is_signed;
    uint32_t         leader_vreg;
    struct GVNEntry* next_in_bucket;
} GVNEntry;

typedef struct GVNContext {
    Arena*        arena;
    IRFunction*   func;

    GVNDomBlock** dom_blocks;
    size_t        block_count;

    GVNDomBlock** rpo_blocks;
    size_t        rpo_count;

    uint32_t*     leaders;
    size_t        vreg_cap;

    GVNEntry*     hash_table[GVN_TABLE_SIZE];
    GVNEntry**    entry_stack;
    size_t        stack_top;
    size_t        stack_cap;
} GVNContext;

static bool operand_equals(IROperand a, IROperand b) {
    if (a.kind != b.kind) {
        return false;
    }

    if (a.kind == IR_OP_VREG) {
        return a.vreg_id == b.vreg_id && a.byte_size == b.byte_size && a.is_signed == b.is_signed;
    }

    if (a.kind == IR_OP_CONST) {
        return a.int_val == b.int_val && a.byte_size == b.byte_size && a.is_signed == b.is_signed;
    }

    if (a.kind == IR_OP_STACK) {
        return a.stack_offset == b.stack_offset && a.byte_size == b.byte_size && a.is_signed == b.is_signed;
    }

    if (a.kind == IR_OP_GLOBAL) {
        return strview_equals(a.global_name, b.global_name);
    }

    if (a.kind == IR_OP_STR) {
        return a.str_id == b.str_id;
    }

    if (a.kind == IR_OP_NONE) {
        return true;
    }

    return false;
}

static IROperand resolve_leader(const GVNContext* ctx, IROperand op) {
    if (op.kind != IR_OP_VREG) {
        return op;
    }

    uint32_t curr = op.vreg_id;

    while (curr < ctx->vreg_cap && ctx->leaders[curr] != 0 && ctx->leaders[curr] != curr) {
        curr = ctx->leaders[curr];
    }

    op.vreg_id = curr;

    return op;
}

static bool is_commutative(IROpcode op) {
    switch (op) {
        case IR_ADD:
        case IR_MUL:
        case IR_AND:
        case IR_OR:
        case IR_XOR:
        case IR_CMP_EQ:
        case IR_CMP_NE:
            return true;

        default:
            return false;
    }
}

static void canonicalize_operands(IROpcode op, IROperand* s1, IROperand* s2) {
    if (!is_commutative(op)) {
        return;
    }

    if (s1->kind == IR_OP_CONST && s2->kind == IR_OP_VREG) {
        IROperand tmp = *s1;
        *s1 = *s2;
        *s2 = tmp;
        return;
    }

    if (s1->kind == IR_OP_VREG && s2->kind == IR_OP_VREG) {
        if (s1->vreg_id > s2->vreg_id) {
            IROperand tmp = *s1;
            *s1 = *s2;
            *s2 = tmp;
        }
    }
}

static uint64_t hash_operand(IROperand op) {
    uint64_t h = ((uint64_t)op.kind * 31ULL) ^ ((uint64_t)op.byte_size << 4);

    if (op.kind == IR_OP_VREG) {
        h ^= ((uint64_t)op.vreg_id + 0x9e3779b97f4a7c15ULL);
    } else if (op.kind == IR_OP_CONST) {
        h ^= ((uint64_t)op.int_val + 0x9e3779b97f4a7c15ULL);
    } else if (op.kind == IR_OP_STACK) {
        h ^= ((uint64_t)op.stack_offset + 0x9e3779b97f4a7c15ULL);
    } else if (op.kind == IR_OP_GLOBAL) {
        for (size_t i = 0; i < op.global_name.len; ++i) {
            h = h * 33ULL + (uint64_t)op.global_name.data[i];
        }
    } else if (op.kind == IR_OP_STR) {
        h ^= ((uint64_t)op.str_id + 0x9e3779b97f4a7c15ULL);
    }

    return h;
}

static uint64_t hash_expr(IROpcode op, IROperand s1, IROperand s2, size_t sz, bool is_signed) {
    uint64_t h = ((uint64_t)op * 1000003ULL) ^ ((uint64_t)sz * 10007ULL) ^ (is_signed ? 1ULL : 0ULL);

    h = (h * 33ULL) ^ hash_operand(s1);
    h = (h * 33ULL) ^ hash_operand(s2);

    return h;
}

static bool is_cse_candidate(IROpcode op) {
    switch (op) {
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_DIV:
        case IR_MOD:
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
        case IR_ADDR:
        case IR_GLOBAL_STR:
            return true;

        default:
            return false;
    }
}

static uint32_t lookup_expression(const GVNContext* ctx, IROpcode op, IROperand s1, IROperand s2, size_t sz, bool is_signed) {
    uint64_t h = hash_expr(op, s1, s2, sz, is_signed);
    size_t bucket = (size_t)(h % GVN_TABLE_SIZE);

    for (GVNEntry* e = ctx->hash_table[bucket]; e != NULL; e = e->next_in_bucket) {
        if (e->opcode == op && e->byte_size == sz && e->is_signed == is_signed) {
            if (operand_equals(e->src1, s1) && operand_equals(e->src2, s2)) {
                return e->leader_vreg;
            }
        }
    }

    return 0;
}

static void insert_expression(GVNContext* ctx, IROpcode op, IROperand s1, IROperand s2, size_t sz, bool is_signed, uint32_t leader) {
    uint64_t h = hash_expr(op, s1, s2, sz, is_signed);
    size_t bucket = (size_t)(h % GVN_TABLE_SIZE);

    GVNEntry* entry = ARENA_NEW(ctx->arena, GVNEntry);

    entry->opcode         = op;
    entry->src1           = s1;
    entry->src2           = s2;
    entry->byte_size      = sz;
    entry->is_signed      = is_signed;
    entry->leader_vreg    = leader;
    entry->next_in_bucket = ctx->hash_table[bucket];

    ctx->hash_table[bucket] = entry;

    ARENA_DA_PUSH(ctx->arena, ctx->entry_stack, ctx->stack_top, ctx->stack_cap, entry);
}

static void unwind_stack(GVNContext* ctx, size_t mark) {
    while (ctx->stack_top > mark) {
        GVNEntry* entry = ctx->entry_stack[--ctx->stack_top];

        uint64_t h = hash_expr(entry->opcode, entry->src1, entry->src2, entry->byte_size, entry->is_signed);
        size_t bucket = (size_t)(h % GVN_TABLE_SIZE);

        if (ctx->hash_table[bucket] == entry) {
            ctx->hash_table[bucket] = entry->next_in_bucket;
        } else {
            GVNEntry* prev = ctx->hash_table[bucket];

            while (prev && prev->next_in_bucket != entry) {
                prev = prev->next_in_bucket;
            }

            if (prev) {
                prev->next_in_bucket = entry->next_in_bucket;
            }
        }
    }
}

static void add_cfg_edge(Arena* arena, GVNDomBlock* from, GVNDomBlock* to) {
    for (size_t i = 0; i < from->succ_count; ++i) {
        if (from->succs[i] == to) {
            return;
        }
    }

    ARENA_DA_PUSH(arena, from->succs, from->succ_count, from->succ_cap, to);
    ARENA_DA_PUSH(arena, to->preds, to->pred_count, to->pred_cap, from);
}

static void rpo_dfs(GVNDomBlock* b, bool* visited, GVNDomBlock** post_order, size_t* po_count) {
    visited[b->dense_id] = true;

    for (size_t i = 0; i < b->succ_count; ++i) {
        GVNDomBlock* succ = b->succs[i];

        if (!visited[succ->dense_id]) {
            rpo_dfs(succ, visited, post_order, po_count);
        }
    }

    post_order[(*po_count)++] = b;
}

static GVNDomBlock* dom_intersect(GVNDomBlock* b1, GVNDomBlock* b2) {
    GVNDomBlock* finger1 = b1;
    GVNDomBlock* finger2 = b2;

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

static void build_dom_tree(GVNContext* ctx) {
    IRFunction* func = ctx->func;
    size_t count = func->block_count;

    ctx->dom_blocks = ARENA_NEW_ARRAY(ctx->arena, GVNDomBlock*, count);
    size_t idx = 0;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        GVNDomBlock* db = ARENA_NEW_ZERO(ctx->arena, GVNDomBlock);

        db->block    = b;
        db->dense_id = idx;

        ctx->dom_blocks[idx++] = db;
    }

    ctx->block_count = count;

    for (size_t i = 0; i < count; ++i) {
        GVNDomBlock* db = ctx->dom_blocks[i];
        IRInst* term = db->block->last_inst;

        if (!term) {
            if (db->block->next_block) {
                for (size_t j = 0; j < count; ++j) {
                    if (ctx->dom_blocks[j]->block == db->block->next_block) {
                        add_cfg_edge(ctx->arena, db, ctx->dom_blocks[j]);
                        break;
                    }
                }
            }
            continue;
        }

        if (term->opcode == IR_JMP) {
            for (size_t j = 0; j < count; ++j) {
                if (ctx->dom_blocks[j]->block == term->dst.block) {
                    add_cfg_edge(ctx->arena, db, ctx->dom_blocks[j]);
                    break;
                }
            }
        } else if (term->opcode == IR_BR) {
            for (size_t j = 0; j < count; ++j) {
                if (ctx->dom_blocks[j]->block == term->src1.block || ctx->dom_blocks[j]->block == term->src2.block) {
                    add_cfg_edge(ctx->arena, db, ctx->dom_blocks[j]);
                }
            }
        }
    }

    bool* visited = ARENA_NEW_ARRAY_ZERO(ctx->arena, bool, count);
    GVNDomBlock** post_order = ARENA_NEW_ARRAY(ctx->arena, GVNDomBlock*, count);
    size_t po_count = 0;

    rpo_dfs(ctx->dom_blocks[0], visited, post_order, &po_count);

    ctx->rpo_blocks = ARENA_NEW_ARRAY(ctx->arena, GVNDomBlock*, po_count);
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
            GVNDomBlock* b = ctx->rpo_blocks[i];
            GVNDomBlock* new_idom = NULL;

            for (size_t p = 0; p < b->pred_count; ++p) {
                GVNDomBlock* pred = b->preds[p];

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
        GVNDomBlock* b = ctx->rpo_blocks[i];
        if (b->idom && b->idom != b) {
            ARENA_DA_PUSH(ctx->arena, b->idom->children, b->idom->child_count, b->idom->child_cap, b);
        }
    }
}

static void gvn_dom_walk(GVNContext* ctx, GVNDomBlock* db) {
    size_t mark = ctx->stack_top;

    for (IRInst* inst = db->block->first_inst; inst != NULL; inst = inst->next) {
        if (inst->opcode == IR_NOP) {
            continue;
        }

        inst->src1 = resolve_leader(ctx, inst->src1);
        inst->src2 = resolve_leader(ctx, inst->src2);

        if (inst->opcode == IR_BR || inst->opcode == IR_RET || inst->opcode == IR_STORE || inst->opcode == IR_MEMCPY) {
            inst->dst = resolve_leader(ctx, inst->dst);
        }

        if (inst->opcode == IR_PHI) {
            for (size_t i = 0; i < inst->extra_arg_count; i += 2) {
                inst->extra_args[i] = resolve_leader(ctx, inst->extra_args[i]);
            }
        }

        if (inst->dst.kind != IR_OP_VREG || !is_cse_candidate(inst->opcode)) {
            continue;
        }

        IROperand s1 = inst->src1;
        IROperand s2 = inst->src2;

        canonicalize_operands(inst->opcode, &s1, &s2);

        uint32_t leader = lookup_expression(ctx, inst->opcode, s1, s2, inst->dst.byte_size, inst->dst.is_signed);

        if (leader != 0 && leader != inst->dst.vreg_id) {
            ctx->leaders[inst->dst.vreg_id] = leader;

            inst->opcode          = IR_MOV;
            inst->src1            = ir_op_vreg(leader, inst->dst.byte_size, inst->dst.is_signed);
            inst->src2            = ir_op_none();
            inst->extra_args      = NULL;
            inst->extra_arg_count = 0;
        } else {
            insert_expression(ctx, inst->opcode, s1, s2, inst->dst.byte_size, inst->dst.is_signed, inst->dst.vreg_id);
        }
    }

    for (size_t c = 0; c < db->child_count; ++c) {
        gvn_dom_walk(ctx, db->children[c]);
    }

    unwind_stack(ctx, mark);
}

static void apply_gvn_leaders(GVNContext* ctx) {
    IRFunction* func = ctx->func;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP) {
                continue;
            }

            inst->src1 = resolve_leader(ctx, inst->src1);
            inst->src2 = resolve_leader(ctx, inst->src2);

            if (inst->opcode == IR_BR || inst->opcode == IR_RET || inst->opcode == IR_STORE || inst->opcode == IR_MEMCPY) {
                inst->dst = resolve_leader(ctx, inst->dst);
            }

            if (inst->opcode == IR_PHI) {
                for (size_t i = 0; i < inst->extra_arg_count; i += 2) {
                    inst->extra_args[i] = resolve_leader(ctx, inst->extra_args[i]);
                }
            }

            for (size_t i = 0; i < inst->extra_arg_count; ++i) {
                if (inst->opcode != IR_PHI) {
                    inst->extra_args[i] = resolve_leader(ctx, inst->extra_args[i]);
                }
            }

            for (size_t i = 0; i < inst->asm_input_count; ++i) {
                inst->asm_inputs[i].val = resolve_leader(ctx, inst->asm_inputs[i].val);
            }
        }
    }
}

void gvn_run_on_function(Arena* arena, IRFunction* func) {
    if (!func || !func->first_block) {
        return;
    }

    size_t vreg_cap = func->next_vreg_id + 1024;

    GVNContext ctx = {
        .arena       = arena,
        .func        = func,
        .dom_blocks  = NULL,
        .block_count = 0,
        .rpo_blocks  = NULL,
        .rpo_count   = 0,
        .leaders     = ARENA_NEW_ARRAY_ZERO(arena, uint32_t, vreg_cap),
        .vreg_cap    = vreg_cap,
        .entry_stack = NULL,
        .stack_top   = 0,
        .stack_cap   = 0
    };

    memset(ctx.hash_table, 0, sizeof(ctx.hash_table));

    build_dom_tree(&ctx);

    if (ctx.rpo_count > 0) {
        gvn_dom_walk(&ctx, ctx.rpo_blocks[0]);
    }

    apply_gvn_leaders(&ctx);
}

void gvn_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        gvn_run_on_function(arena, f);
    }
}