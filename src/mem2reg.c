#include "mem2reg.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "arena.h"

typedef struct CFGBlock CFGBlock;
typedef struct PromoteCandidate PromoteCandidate;
typedef struct PromotedVar PromotedVar;
typedef struct DefStackNode DefStackNode;
typedef struct Mem2RegContext Mem2RegContext;

typedef struct BitSet {
    uint64_t* words;
    size_t word_count;
} BitSet;

struct CFGBlock {
    IRBlock* block;
    size_t dense_id;
    size_t rpo_index;

    CFGBlock** preds;
    size_t pred_count;
    size_t pred_cap;

    CFGBlock** succs;
    size_t succ_count;
    size_t succ_cap;

    CFGBlock* idom;

    CFGBlock** dom_children;
    size_t dom_child_count;
    size_t dom_child_cap;

    CFGBlock** dom_frontier;
    size_t dom_frontier_count;
    size_t dom_frontier_cap;
};

struct PromoteCandidate {
    int32_t stack_offset;
    size_t byte_size;
    bool is_signed;
    bool escaped;
};

struct PromotedVar {
    int32_t stack_offset;
    size_t byte_size;
    bool is_signed;
    uint32_t id;

    CFGBlock** def_blocks;
    size_t def_count;
    size_t def_cap;

    CFGBlock** use_blocks;
    size_t use_count;
    size_t use_cap;

    bool local_only;
    bool use_before_def;
    CFGBlock* local_block;
};

struct DefStackNode {
    IROperand value;
    DefStackNode* next;
};

struct Mem2RegContext {
    Arena* arena;
    IRFunction* function;

    CFGBlock** blocks;
    size_t block_count;

    CFGBlock** rpo;
    size_t rpo_count;

    PromotedVar* vars;
    size_t var_count;
    size_t var_cap;

    DefStackNode** stacks;
    IROperand* substitutions;
    size_t substitution_cap;

    BitSet* block_use;
    BitSet* block_def;
    BitSet* live_in;
    BitSet* live_out;

    BitSet scratch_out;
    BitSet scratch_in;
};

static size_t bitset_word_count(size_t bit_count) {
    return (bit_count + 63u) >> 6;
}

static void bitset_init(Arena* arena, BitSet* set, size_t bit_count) {
    set->word_count = bitset_word_count(bit_count);
    set->words = set->word_count > 0
        ? ARENA_NEW_ARRAY_ZERO(arena, uint64_t, set->word_count)
        : NULL;
}

static bool bitset_test(const BitSet* set, size_t bit) {
    return (set->words[bit >> 6] & (UINT64_C(1) << (bit & 63u))) != 0;
}

static bool bitset_set(BitSet* set, size_t bit) {
    uint64_t* word = &set->words[bit >> 6];
    uint64_t mask = UINT64_C(1) << (bit & 63u);

    if ((*word & mask) != 0) {
        return false;
    }

    *word |= mask;
    return true;
}

static bool bitset_assign(BitSet* dst, const BitSet* src) {
    bool changed = false;

    for (size_t i = 0; i < dst->word_count; ++i) {
        if (dst->words[i] != src->words[i]) {
            dst->words[i] = src->words[i];
            changed = true;
        }
    }

    return changed;
}

static void bitset_clear_all(BitSet* set) {
    if (set->word_count != 0) {
        memset(set->words, 0, set->word_count * sizeof(*set->words));
    }
}

static bool bitset_copy_minus(BitSet* dst, const BitSet* src, const BitSet* minus) {
    bool changed = false;

    for (size_t i = 0; i < dst->word_count; ++i) {
        uint64_t value = src->words[i] & ~minus->words[i];

        if (dst->words[i] != value) {
            dst->words[i] = value;
            changed = true;
        }
    }

    return changed;
}

static bool bitset_union(BitSet* dst, const BitSet* src) {
    bool changed = false;

    for (size_t i = 0; i < dst->word_count; ++i) {
        uint64_t value = dst->words[i] | src->words[i];

        if (dst->words[i] != value) {
            dst->words[i] = value;
            changed = true;
        }
    }

    return changed;
}

static bool operand_is_stack(const IROperand* operand) {
    return operand->kind == IR_OP_STACK && operand->stack_offset < 0;
}

static size_t effective_byte_size(const IROperand* operand) {
    return operand->byte_size != 0 ? operand->byte_size : 8;
}

static bool is_scalar_size(size_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

static void cfg_add_edge(Arena* arena, CFGBlock* from, CFGBlock* to) {
    for (size_t i = 0; i < from->succ_count; ++i) {
        if (from->succs[i] == to) {
            return;
        }
    }

    ARENA_DA_PUSH(arena, from->succs, from->succ_count, from->succ_cap, to);
    ARENA_DA_PUSH(arena, to->preds, to->pred_count, to->pred_cap, from);
}

static size_t find_block_index(CFGBlock** blocks, size_t count, const IRBlock* target) {
    for (size_t i = 0; i < count; ++i) {
        if (blocks[i]->block == target) {
            return i;
        }
    }

    return SIZE_MAX;
}

static void split_critical_edges(Arena* arena, IRFunction* function) {
    bool changed;

    do {
        changed = false;

        for (IRBlock* block = function->first_block; block != NULL; block = block->next_block) {
            IRInst* terminator = block->last_inst;

            if (terminator == NULL || terminator->opcode != IR_BR) {
                continue;
            }

            IRBlock* targets[2] = {
                terminator->src1.kind == IR_OP_BLOCK ? terminator->src1.block : NULL,
                terminator->src2.kind == IR_OP_BLOCK ? terminator->src2.block : NULL
            };

            for (size_t t = 0; t < 2; ++t) {
                IRBlock* target = targets[t];
                size_t predecessor_count = 0;

                if (target == NULL) {
                    continue;
                }

                for (IRBlock* predecessor = function->first_block;
                     predecessor != NULL;
                     predecessor = predecessor->next_block) {
                    IRInst* predecessor_term = predecessor->last_inst;

                    if (predecessor_term == NULL) {
                        continue;
                    }

                    if (predecessor_term->opcode == IR_JMP &&
                        predecessor_term->dst.kind == IR_OP_BLOCK &&
                        predecessor_term->dst.block == target) {
                        predecessor_count++;
                    } else if (predecessor_term->opcode == IR_BR &&
                               ((predecessor_term->src1.kind == IR_OP_BLOCK && predecessor_term->src1.block == target) ||
                                (predecessor_term->src2.kind == IR_OP_BLOCK && predecessor_term->src2.block == target))) {
                        predecessor_count++;
                    }
                }

                if (predecessor_count <= 1) {
                    continue;
                }

                IRBlock* split = ir_block_create(function, "bb_mem2reg_split");
                ir_block_switch(function, split);
                ir_emit_inst(function, IR_JMP, ir_op_block(target), ir_op_none(), ir_op_none(), terminator->loc);

                if (t == 0) {
                    terminator->src1 = ir_op_block(split);
                } else {
                    terminator->src2 = ir_op_block(split);
                }

                changed = true;
                break;
            }

            if (changed) {
                break;
            }
        }
    } while (changed);

    (void)arena;
}

static void build_cfg(Mem2RegContext* ctx) {
    size_t block_count = ctx->function->block_count;
    ctx->block_count = block_count;
    ctx->blocks = ARENA_NEW_ARRAY(ctx->arena, CFGBlock*, block_count);

    size_t index = 0;
    for (IRBlock* block = ctx->function->first_block; block != NULL; block = block->next_block) {
        CFGBlock* cfg = ARENA_NEW_ZERO(ctx->arena, CFGBlock);
        cfg->block = block;
        cfg->dense_id = index;
        ctx->blocks[index++] = cfg;
    }

    for (size_t i = 0; i < block_count; ++i) {
        CFGBlock* cfg = ctx->blocks[i];
        IRInst* terminator = cfg->block->last_inst;

        if (terminator != NULL &&
            terminator->opcode == IR_JMP &&
            terminator->dst.kind == IR_OP_BLOCK) {
            size_t successor = find_block_index(ctx->blocks, block_count, terminator->dst.block);
            if (successor != SIZE_MAX) {
                cfg_add_edge(ctx->arena, cfg, ctx->blocks[successor]);
            }
        } else if (terminator != NULL && terminator->opcode == IR_BR) {
            if (terminator->src1.kind == IR_OP_BLOCK) {
                size_t successor = find_block_index(ctx->blocks, block_count, terminator->src1.block);
                if (successor != SIZE_MAX) {
                    cfg_add_edge(ctx->arena, cfg, ctx->blocks[successor]);
                }
            }

            if (terminator->src2.kind == IR_OP_BLOCK) {
                size_t successor = find_block_index(ctx->blocks, block_count, terminator->src2.block);
                if (successor != SIZE_MAX) {
                    cfg_add_edge(ctx->arena, cfg, ctx->blocks[successor]);
                }
            }
        } else if (terminator != NULL &&
                   (terminator->opcode == IR_RET ||
                    terminator->opcode == IR_TAIL_CALL ||
                    terminator->opcode == IR_TAIL_CALL_PTR)) {
            continue;
        } else if (cfg->block->next_block != NULL) {
            size_t successor = find_block_index(ctx->blocks, block_count, cfg->block->next_block);
            if (successor != SIZE_MAX) {
                cfg_add_edge(ctx->arena, cfg, ctx->blocks[successor]);
            }
        }
    }
}

static void rpo_dfs(CFGBlock* block, bool* visited, CFGBlock** postorder, size_t* count) {
    if (visited[block->dense_id]) {
        return;
    }

    visited[block->dense_id] = true;

    for (size_t i = 0; i < block->succ_count; ++i) {
        rpo_dfs(block->succs[i], visited, postorder, count);
    }

    postorder[(*count)++] = block;
}

static void compute_rpo(Mem2RegContext* ctx) {
    bool* visited = ARENA_NEW_ARRAY_ZERO(ctx->arena, bool, ctx->block_count);
    CFGBlock** postorder = ARENA_NEW_ARRAY(ctx->arena, CFGBlock*, ctx->block_count);
    size_t postorder_count = 0;

    if (ctx->block_count != 0) {
        rpo_dfs(ctx->blocks[0], visited, postorder, &postorder_count);
    }

    ctx->rpo_count = postorder_count;
    ctx->rpo = ARENA_NEW_ARRAY(ctx->arena, CFGBlock*, postorder_count);

    for (size_t i = 0; i < postorder_count; ++i) {
        CFGBlock* block = postorder[postorder_count - i - 1];
        block->rpo_index = i;
        ctx->rpo[i] = block;
    }
}

static CFGBlock* dom_intersect(CFGBlock* left, CFGBlock* right) {
    CFGBlock* a = left;
    CFGBlock* b = right;

    while (a != b) {
        while (a->rpo_index > b->rpo_index) {
            a = a->idom;
        }

        while (b->rpo_index > a->rpo_index) {
            b = b->idom;
        }
    }

    return a;
}

static void compute_dominators(Mem2RegContext* ctx) {
    if (ctx->rpo_count == 0) {
        return;
    }

    CFGBlock* entry = ctx->rpo[0];
    entry->idom = entry;

    bool changed;
    do {
        changed = false;

        for (size_t i = 1; i < ctx->rpo_count; ++i) {
            CFGBlock* block = ctx->rpo[i];
            CFGBlock* new_idom = NULL;

            for (size_t p = 0; p < block->pred_count; ++p) {
                CFGBlock* predecessor = block->preds[p];

                if (predecessor->idom == NULL) {
                    continue;
                }

                if (new_idom == NULL) {
                    new_idom = predecessor;
                } else {
                    new_idom = dom_intersect(predecessor, new_idom);
                }
            }

            if (new_idom != NULL && block->idom != new_idom) {
                block->idom = new_idom;
                changed = true;
            }
        }
    } while (changed);

    for (size_t i = 1; i < ctx->rpo_count; ++i) {
        CFGBlock* block = ctx->rpo[i];
        assert(block->idom != NULL);
        assert(block->idom != block);
        ARENA_DA_PUSH(ctx->arena,
                      block->idom->dom_children,
                      block->idom->dom_child_count,
                      block->idom->dom_child_cap,
                      block);
    }
}

static void compute_dominance_frontiers(Mem2RegContext* ctx) {
    for (size_t i = 0; i < ctx->rpo_count; ++i) {
        CFGBlock* block = ctx->rpo[i];

        size_t reachable_pred_count = 0;
        for (size_t p = 0; p < block->pred_count; ++p) {
            if (block->preds[p]->idom != NULL) {
                reachable_pred_count++;
            }
        }

        if (reachable_pred_count < 2) {
            continue;
        }

        for (size_t p = 0; p < block->pred_count; ++p) {
            CFGBlock* runner = block->preds[p];

            if (runner->idom == NULL) {
                continue;
            }

            while (runner != block->idom) {
                bool present = false;

                for (size_t d = 0; d < runner->dom_frontier_count; ++d) {
                    if (runner->dom_frontier[d] == block) {
                        present = true;
                        break;
                    }
                }

                if (!present) {
                    ARENA_DA_PUSH(ctx->arena,
                                  runner->dom_frontier,
                                  runner->dom_frontier_count,
                                  runner->dom_frontier_cap,
                                  block);
                }

                if (runner == runner->idom) {
                    break;
                }

                runner = runner->idom;
            }
        }
    }
}

static int32_t candidate_index(const PromoteCandidate* candidates,
                               size_t candidate_count,
                               int32_t stack_offset) {
    for (size_t i = 0; i < candidate_count; ++i) {
        if (candidates[i].stack_offset == stack_offset) {
            return (int32_t)i;
        }
    }

    return -1;
}

static PromoteCandidate* candidate_get_or_add(Arena* arena,
                                              PromoteCandidate** candidates,
                                              size_t* candidate_count,
                                              size_t* candidate_cap,
                                              const IROperand* operand) {
    size_t size = effective_byte_size(operand);
    int32_t index = candidate_index(*candidates, *candidate_count, operand->stack_offset);

    if (index >= 0) {
        PromoteCandidate* candidate = &(*candidates)[(size_t)index];

        if (size > candidate->byte_size) {
            candidate->byte_size = size;
        }

        return candidate;
    }

    PromoteCandidate candidate = {
        .stack_offset = operand->stack_offset,
        .byte_size = size,
        .is_signed = operand->is_signed,
        .escaped = false
    };

    ARENA_DA_PUSH(arena, *candidates, *candidate_count, *candidate_cap, candidate);
    return &(*candidates)[*candidate_count - 1];
}

static bool ranges_overlap(int32_t a_offset, size_t a_size,
                           int32_t b_offset, size_t b_size) {
    int64_t a_begin = a_offset;
    int64_t a_end = a_begin + (int64_t)a_size;
    int64_t b_begin = b_offset;
    int64_t b_end = b_begin + (int64_t)b_size;

    return a_begin < b_end && b_begin < a_end;
}

static void mark_range_escaped(PromoteCandidate* candidates,
                               size_t candidate_count,
                               int32_t offset,
                               size_t size) {
    size = size != 0 ? size : 8;

    for (size_t i = 0; i < candidate_count; ++i) {
        if (ranges_overlap(candidates[i].stack_offset,
                           candidates[i].byte_size,
                           offset,
                           size)) {
            candidates[i].escaped = true;
        }
    }
}

static void collect_candidates(Mem2RegContext* ctx,
                               PromoteCandidate** candidates,
                               size_t* candidate_count,
                               size_t* candidate_cap) {
    for (IRBlock* block = ctx->function->first_block;
         block != NULL;
         block = block->next_block) {
        for (IRInst* inst = block->first_inst; inst != NULL; inst = inst->next) {
            if (operand_is_stack(&inst->dst)) {
                (void)candidate_get_or_add(ctx->arena,
                                           candidates,
                                           candidate_count,
                                           candidate_cap,
                                           &inst->dst);
            }

            if (operand_is_stack(&inst->src1)) {
                (void)candidate_get_or_add(ctx->arena,
                                           candidates,
                                           candidate_count,
                                           candidate_cap,
                                           &inst->src1);
            }

            if (operand_is_stack(&inst->src2)) {
                (void)candidate_get_or_add(ctx->arena,
                                           candidates,
                                           candidate_count,
                                           candidate_cap,
                                           &inst->src2);
            }

            for (size_t i = 0; i < inst->extra_arg_count; ++i) {
                if (operand_is_stack(&inst->extra_args[i])) {
                    (void)candidate_get_or_add(ctx->arena,
                                               candidates,
                                               candidate_count,
                                               candidate_cap,
                                               &inst->extra_args[i]);
                }
            }
        }
    }

    for (IRBlock* block = ctx->function->first_block;
         block != NULL;
         block = block->next_block) {
        for (IRInst* inst = block->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_ADDR && operand_is_stack(&inst->src1)) {
                mark_range_escaped(*candidates,
                                   *candidate_count,
                                   inst->src1.stack_offset,
                                   effective_byte_size(&inst->src1));
            }

            if (inst->opcode == IR_MEMCPY) {
                size_t copy_size = 8;

                if (inst->src2.kind == IR_OP_CONST && inst->src2.int_val > 0) {
                    copy_size = (size_t)inst->src2.int_val;
                } else {
                    if (operand_is_stack(&inst->dst)) {
                        mark_range_escaped(*candidates,
                                           *candidate_count,
                                           inst->dst.stack_offset,
                                           0);
                    }

                    if (operand_is_stack(&inst->src1)) {
                        mark_range_escaped(*candidates,
                                           *candidate_count,
                                           inst->src1.stack_offset,
                                           0);
                    }
                    continue;
                }

                if (operand_is_stack(&inst->dst)) {
                    mark_range_escaped(*candidates,
                                       *candidate_count,
                                       inst->dst.stack_offset,
                                       copy_size);
                }

                if (operand_is_stack(&inst->src1)) {
                    mark_range_escaped(*candidates,
                                       *candidate_count,
                                       inst->src1.stack_offset,
                                       copy_size);
                }
            }

            if (inst->opcode == IR_STORE && inst->src1.kind == IR_OP_VREG) {
                /* A pointer-valued vreg stored to memory escapes the pointed
                 * stack object. The exact source range is recovered later by
                 * walking pointer-def chains; for unknown pointer arithmetic,
                 * the whole corresponding candidate is conservatively escaped. */
                for (IRBlock* scan_block = ctx->function->first_block;
                     scan_block != NULL;
                     scan_block = scan_block->next_block) {
                    for (IRInst* scan = scan_block->first_inst; scan != NULL; scan = scan->next) {
                        if (scan->dst.kind == IR_OP_VREG &&
                            scan->dst.vreg_id == inst->src1.vreg_id &&
                            scan->opcode == IR_ADDR &&
                            operand_is_stack(&scan->src1)) {
                            mark_range_escaped(*candidates,
                                               *candidate_count,
                                               scan->src1.stack_offset,
                                               effective_byte_size(&scan->src1));
                        }
                    }
                }
            }
        }
    }
}

static int32_t promoted_var_index(const Mem2RegContext* ctx, int32_t stack_offset) {
    for (size_t i = 0; i < ctx->var_count; ++i) {
        if (ctx->vars[i].stack_offset == stack_offset) {
            return (int32_t)i;
        }
    }

    return -1;
}

static void record_unique_block(Arena* arena,
                                CFGBlock*** blocks,
                                size_t* count,
                                size_t* cap,
                                CFGBlock* block) {
    for (size_t i = 0; i < *count; ++i) {
        if ((*blocks)[i] == block) {
            return;
        }
    }

    ARENA_DA_PUSH(arena, *blocks, *count, *cap, block);
}

static bool instruction_reads_promoted_stack(const IRInst* inst, int32_t stack_offset) {
    if (inst->opcode == IR_MOV &&
        operand_is_stack(&inst->src1) &&
        inst->src1.stack_offset == stack_offset) {
        return true;
    }

    return false;
}

static bool instruction_defines_promoted_stack(const IRInst* inst, int32_t stack_offset) {
    if ((inst->opcode == IR_MOV || inst->opcode == IR_PARAM) &&
        operand_is_stack(&inst->dst) &&
        inst->dst.stack_offset == stack_offset) {
        return true;
    }

    return false;
}

static void build_var_def_use(Mem2RegContext* ctx) {
    for (size_t v = 0; v < ctx->var_count; ++v) {
        PromotedVar* var = &ctx->vars[v];

        for (size_t i = 0; i < ctx->rpo_count; ++i) {
            CFGBlock* block = ctx->rpo[i];
            bool seen_def = false;

            for (IRInst* inst = block->block->first_inst; inst != NULL; inst = inst->next) {
                if (instruction_reads_promoted_stack(inst, var->stack_offset) && !seen_def) {
                    record_unique_block(ctx->arena,
                                        &var->use_blocks,
                                        &var->use_count,
                                        &var->use_cap,
                                        block);
                    var->use_before_def = true;
                }

                if (instruction_defines_promoted_stack(inst, var->stack_offset)) {
                    record_unique_block(ctx->arena,
                                        &var->def_blocks,
                                        &var->def_count,
                                        &var->def_cap,
                                        block);
                    seen_def = true;
                }
            }
        }

        var->local_only = var->def_count == 1 &&
                          var->use_count == 1 &&
                          !var->use_before_def;
        var->local_block = var->local_only ? var->def_blocks[0] : NULL;

        if (!var->local_only) {
            continue;
        }

        for (size_t i = 0; i < var->use_count; ++i) {
            if (var->use_blocks[i] != var->local_block) {
                var->local_only = false;
                var->local_block = NULL;
                break;
            }
        }
    }
}

static void build_liveness(Mem2RegContext* ctx) {
    size_t block_count = ctx->block_count;
    size_t var_count = ctx->var_count;

    ctx->block_use = ARENA_NEW_ARRAY(ctx->arena, BitSet, block_count);
    ctx->block_def = ARENA_NEW_ARRAY(ctx->arena, BitSet, block_count);
    ctx->live_in = ARENA_NEW_ARRAY(ctx->arena, BitSet, block_count);
    ctx->live_out = ARENA_NEW_ARRAY(ctx->arena, BitSet, block_count);

    for (size_t b = 0; b < block_count; ++b) {
        bitset_init(ctx->arena, &ctx->block_use[b], var_count);
        bitset_init(ctx->arena, &ctx->block_def[b], var_count);
        bitset_init(ctx->arena, &ctx->live_in[b], var_count);
        bitset_init(ctx->arena, &ctx->live_out[b], var_count);
    }

    bitset_init(ctx->arena, &ctx->scratch_out, var_count);
    bitset_init(ctx->arena, &ctx->scratch_in, var_count);

    for (size_t i = 0; i < ctx->rpo_count; ++i) {
        CFGBlock* block = ctx->rpo[i];
        BitSet* use = &ctx->block_use[block->dense_id];
        BitSet* def = &ctx->block_def[block->dense_id];

        for (IRInst* inst = block->block->first_inst; inst != NULL; inst = inst->next) {
            bool reads_stack = false;
            bool defines_stack = false;
            IROperand read_operand = ir_op_none();
            IROperand write_operand = ir_op_none();

            if (inst->opcode == IR_MOV) {
                if (operand_is_stack(&inst->src1)) {
                    reads_stack = true;
                    read_operand = inst->src1;
                }
                if (operand_is_stack(&inst->dst)) {
                    defines_stack = true;
                    write_operand = inst->dst;
                }
            } else if (inst->opcode == IR_PARAM && operand_is_stack(&inst->dst)) {
                defines_stack = true;
                write_operand = inst->dst;
            } else if (inst->opcode == IR_LOAD && operand_is_stack(&inst->src1)) {
                reads_stack = true;
                read_operand = inst->src1;
            } else if (inst->opcode == IR_STORE && operand_is_stack(&inst->dst)) {
                defines_stack = true;
                write_operand = inst->dst;
            }

            if (reads_stack) {
                int32_t var_id = promoted_var_index(ctx, read_operand.stack_offset);
                if (var_id >= 0 && !bitset_test(def, (size_t)var_id)) {
                    bitset_set(use, (size_t)var_id);
                }
            }

            if (defines_stack) {
                int32_t var_id = promoted_var_index(ctx, write_operand.stack_offset);
                if (var_id >= 0) {
                    bitset_set(def, (size_t)var_id);
                }
            }
        }
    }

    size_t* queue = NULL;
    size_t queue_count = 0;
    size_t queue_cap = 0;
    bool* queued = ARENA_NEW_ARRAY_ZERO(ctx->arena, bool, block_count);
    size_t head = 0;

    for (size_t i = 0; i < ctx->rpo_count; ++i) {
        size_t id = ctx->rpo[ctx->rpo_count - i - 1]->dense_id;
        ARENA_DA_PUSH(ctx->arena, queue, queue_count, queue_cap, id);
        queued[id] = true;
    }

    while (head < queue_count) {
        size_t block_id = queue[head++];
        queued[block_id] = false;
        CFGBlock* block = ctx->blocks[block_id];

        bitset_clear_all(&ctx->scratch_out);
        for (size_t s = 0; s < block->succ_count; ++s) {
            bitset_union(&ctx->scratch_out, &ctx->live_in[block->succs[s]->dense_id]);
        }

        bool out_changed = bitset_assign(&ctx->live_out[block_id], &ctx->scratch_out);

        bitset_clear_all(&ctx->scratch_in);
        bitset_copy_minus(&ctx->scratch_in,
                          &ctx->live_out[block_id],
                          &ctx->block_def[block_id]);
        bitset_union(&ctx->scratch_in, &ctx->block_use[block_id]);

        bool in_changed = bitset_assign(&ctx->live_in[block_id], &ctx->scratch_in);

        if (out_changed || in_changed) {
            for (size_t p = 0; p < block->pred_count; ++p) {
                size_t pred_id = block->preds[p]->dense_id;

                if (!queued[pred_id]) {
                    ARENA_DA_PUSH(ctx->arena, queue, queue_count, queue_cap, pred_id);
                    queued[pred_id] = true;
                }
            }
        }
    }
}

static void add_entry_definition(Mem2RegContext* ctx) {
    if (ctx->rpo_count == 0) {
        return;
    }

    CFGBlock* entry = ctx->rpo[0];

    for (size_t v = 0; v < ctx->var_count; ++v) {
        record_unique_block(ctx->arena,
                            &ctx->vars[v].def_blocks,
                            &ctx->vars[v].def_count,
                            &ctx->vars[v].def_cap,
                            entry);
    }
}

static bool phi_needed_for_var(const Mem2RegContext* ctx,
                               size_t var_id,
                               CFGBlock* block) {
    return bitset_test(&ctx->live_in[block->dense_id], var_id);
}

static void insert_phi(Mem2RegContext* ctx, size_t var_id, CFGBlock* block) {
    PromotedVar* var = &ctx->vars[var_id];
    uint32_t phi_vreg = ir_vreg_alloc(ctx->function);
    IRInst* phi = ARENA_NEW_ZERO(ctx->arena, IRInst);

    phi->opcode = IR_PHI;
    phi->dst = ir_op_vreg(phi_vreg, var->byte_size, var->is_signed);
    phi->src1 = ir_op_stack(var->stack_offset, var->byte_size, var->is_signed);
    phi->src2 = ir_op_none();
    phi->loc = block->block->first_inst != NULL
        ? block->block->first_inst->loc
        : (SourceLoc){0};
    phi->extra_arg_count = block->pred_count * 2;
    phi->extra_args = ARENA_NEW_ARRAY_ZERO(ctx->arena, IROperand, phi->extra_arg_count);

    phi->next = block->block->first_inst;
    block->block->first_inst = phi;

    if (block->block->last_inst == NULL) {
        block->block->last_inst = phi;
    }

    block->block->inst_count++;
}

static void place_pruned_phis(Mem2RegContext* ctx) {
    for (size_t var_id = 0; var_id < ctx->var_count; ++var_id) {
        PromotedVar* var = &ctx->vars[var_id];

        if (var->local_only) {
            continue;
        }

        bool* has_phi = ARENA_NEW_ARRAY_ZERO(ctx->arena, bool, ctx->block_count);
        bool* queued = ARENA_NEW_ARRAY_ZERO(ctx->arena, bool, ctx->block_count);
        CFGBlock** queue = NULL;
        size_t queue_count = 0;
        size_t queue_cap = 0;
        size_t head = 0;

        for (size_t i = 0; i < var->def_count; ++i) {
            CFGBlock* def_block = var->def_blocks[i];

            if (!queued[def_block->dense_id]) {
                queued[def_block->dense_id] = true;
                ARENA_DA_PUSH(ctx->arena, queue, queue_count, queue_cap, def_block);
            }
        }

        while (head < queue_count) {
            CFGBlock* x = queue[head++];

            for (size_t i = 0; i < x->dom_frontier_count; ++i) {
                CFGBlock* y = x->dom_frontier[i];

                if (has_phi[y->dense_id] || !phi_needed_for_var(ctx, var_id, y)) {
                    continue;
                }

                insert_phi(ctx, var_id, y);
                has_phi[y->dense_id] = true;

                if (!queued[y->dense_id]) {
                    queued[y->dense_id] = true;
                    ARENA_DA_PUSH(ctx->arena, queue, queue_count, queue_cap, y);
                }
            }
        }
    }
}

static void stack_push(Mem2RegContext* ctx, size_t var_id, IROperand value) {
    DefStackNode* node = ARENA_NEW(ctx->arena, DefStackNode);
    node->value = value;
    node->next = ctx->stacks[var_id];
    ctx->stacks[var_id] = node;
}

static IROperand stack_peek(const Mem2RegContext* ctx, size_t var_id) {
    if (ctx->stacks[var_id] != NULL) {
        return ctx->stacks[var_id]->value;
    }

    const PromotedVar* var = &ctx->vars[var_id];
    return ir_op_const(0, var->byte_size, var->is_signed);
}

static void stack_pop(Mem2RegContext* ctx, size_t var_id) {
    assert(ctx->stacks[var_id] != NULL);
    ctx->stacks[var_id] = ctx->stacks[var_id]->next;
}

static IROperand resolve_substitution(const Mem2RegContext* ctx, IROperand operand) {
    size_t steps = 0;

    while (operand.kind == IR_OP_VREG && operand.vreg_id < ctx->substitution_cap) {
        IROperand replacement = ctx->substitutions[operand.vreg_id];

        if (replacement.kind == IR_OP_NONE) {
            break;
        }

        operand = replacement;
        if (++steps > ctx->substitution_cap) {
            break;
        }
    }

    return operand;
}

static void initialize_substitution_table(Mem2RegContext* ctx) {
    ctx->substitution_cap = ctx->function->next_vreg_id + 1024;
    ctx->substitutions = ARENA_NEW_ARRAY_ZERO(ctx->arena,
                                              IROperand,
                                              ctx->substitution_cap);
}

static int32_t phi_var_id(const Mem2RegContext* ctx, const IRInst* inst) {
    if (inst->opcode != IR_PHI || !operand_is_stack(&inst->src1)) {
        return -1;
    }

    return promoted_var_index(ctx, inst->src1.stack_offset);
}

static void set_phi_incoming(IRInst* phi, size_t predecessor_index, IROperand value, IRBlock* predecessor) {
    assert(phi->opcode == IR_PHI);
    assert(predecessor_index * 2 + 1 < phi->extra_arg_count);

    phi->extra_args[predecessor_index * 2] = value;
    phi->extra_args[predecessor_index * 2 + 1] = ir_op_block(predecessor);
}

static IROperand load_value_from_promoted_stack(Mem2RegContext* ctx, size_t var_id, IRInst* inst) {
    IROperand value = stack_peek(ctx, var_id);
    value.byte_size = inst->dst.byte_size != 0
        ? inst->dst.byte_size
        : value.byte_size;
    value.is_signed = inst->dst.is_signed;
    return resolve_substitution(ctx, value);
}

static IROperand resolve_promoted_stack_read(const Mem2RegContext* ctx, IROperand operand) {
    if (!operand_is_stack(&operand)) {
        return resolve_substitution(ctx, operand);
    }

    int32_t var_id = promoted_var_index(ctx, operand.stack_offset);
    if (var_id < 0) {
        return resolve_substitution(ctx, operand);
    }

    IROperand value = stack_peek(ctx, (size_t)var_id);
    if (operand.byte_size != 0) {
        value.byte_size = operand.byte_size;
        value.is_signed = operand.is_signed;
    }

    return resolve_substitution(ctx, value);
}

static void rename_local_block(Mem2RegContext* ctx, PromotedVar* var) {
    CFGBlock* block = var->local_block;
    IROperand current = ir_op_const(0, var->byte_size, var->is_signed);
    bool defined = false;

    for (IRInst* inst = block->block->first_inst; inst != NULL; ) {
        IRInst* next = inst->next;

        if (inst->opcode == IR_PHI && phi_var_id(ctx, inst) == (int32_t)var->id) {
            current = inst->dst;
            defined = true;
            inst = next;
            continue;
        }

        if (instruction_defines_promoted_stack(inst, var->stack_offset)) {
            current = inst->src1;
            defined = true;
            inst->opcode = IR_NOP;
        } else if (instruction_reads_promoted_stack(inst, var->stack_offset)) {
            assert(defined || var->def_count == 1);
            if (inst->dst.kind == IR_OP_VREG && inst->dst.vreg_id < ctx->substitution_cap) {
                ctx->substitutions[inst->dst.vreg_id] = current;
                inst->opcode = IR_NOP;
            }
        }

        inst = next;
    }
}

static void rename_block(Mem2RegContext* ctx, CFGBlock* block) {
    size_t* pushed = ARENA_NEW_ARRAY_ZERO(ctx->arena, size_t, ctx->var_count);

    for (IRInst* inst = block->block->first_inst;
         inst != NULL && inst->opcode == IR_PHI;
         inst = inst->next) {
        int32_t var_id = phi_var_id(ctx, inst);

        if (var_id >= 0) {
            stack_push(ctx, (size_t)var_id, inst->dst);
            pushed[(size_t)var_id]++;
        }
    }

    for (IRInst* inst = block->block->first_inst; inst != NULL; inst = inst->next) {
        if (inst->opcode == IR_PHI) {
            continue;
        }

        if (inst->opcode == IR_PARAM && operand_is_stack(&inst->dst)) {
            int32_t var_id = promoted_var_index(ctx, inst->dst.stack_offset);

            if (var_id >= 0) {
                PromotedVar* var = &ctx->vars[(size_t)var_id];
                uint32_t vreg = ir_vreg_alloc(ctx->function);
                IROperand value = ir_op_vreg(vreg, var->byte_size, var->is_signed);

                inst->dst = value;
                stack_push(ctx, (size_t)var_id, value);
                pushed[(size_t)var_id]++;
            }
            continue;
        }

        if ((inst->opcode == IR_MOV || inst->opcode == IR_LOAD) &&
            operand_is_stack(&inst->src1) &&
            inst->dst.kind == IR_OP_VREG) {
            int32_t var_id = promoted_var_index(ctx, inst->src1.stack_offset);

            if (var_id >= 0) {
                ctx->substitutions[inst->dst.vreg_id] = load_value_from_promoted_stack(ctx, (size_t)var_id, inst);
                inst->opcode = IR_NOP;
                continue;
            }
        }

        if ((inst->opcode == IR_MOV || inst->opcode == IR_STORE) && operand_is_stack(&inst->dst)) {
            int32_t var_id = promoted_var_index(ctx, inst->dst.stack_offset);

            if (var_id >= 0) {
                stack_push(ctx,
                           (size_t)var_id,
                           resolve_substitution(ctx, inst->src1));
                pushed[(size_t)var_id]++;
                inst->opcode = IR_NOP;
                continue;
            }
        }

        if (inst->dst.kind == IR_OP_VREG) {
            inst->dst = resolve_substitution(ctx, inst->dst);
        } else if (inst->opcode != IR_PARAM && inst->opcode != IR_STORE) {
            inst->dst = resolve_promoted_stack_read(ctx, inst->dst);
        }

        inst->src1 = resolve_promoted_stack_read(ctx, inst->src1);
        inst->src2 = resolve_promoted_stack_read(ctx, inst->src2);

        for (size_t i = 0; i < inst->extra_arg_count; ++i) {
            inst->extra_args[i] = resolve_promoted_stack_read(ctx, inst->extra_args[i]);
        }

        for (size_t i = 0; i < inst->asm_input_count; ++i) {
            inst->asm_inputs[i].val = resolve_substitution(ctx, inst->asm_inputs[i].val);
        }

        for (size_t i = 0; i < inst->asm_output_count; ++i) {
            inst->asm_outputs[i].val = resolve_substitution(ctx, inst->asm_outputs[i].val);
        }
    }

    for (size_t successor_index = 0; successor_index < block->succ_count; ++successor_index) {
        CFGBlock* successor = block->succs[successor_index];
        size_t predecessor_index = SIZE_MAX;

        for (size_t p = 0; p < successor->pred_count; ++p) {
            if (successor->preds[p] == block) {
                predecessor_index = p;
                break;
            }
        }

        assert(predecessor_index != SIZE_MAX);

        for (IRInst* inst = successor->block->first_inst;
             inst != NULL && inst->opcode == IR_PHI;
             inst = inst->next) {
            int32_t var_id = phi_var_id(ctx, inst);

            if (var_id >= 0) {
                IROperand value = resolve_substitution(ctx, stack_peek(ctx, (size_t)var_id));
                set_phi_incoming(inst, predecessor_index, value, block->block);
            }
        }
    }

    for (size_t child = 0; child < block->dom_child_count; ++child) {
        rename_block(ctx, block->dom_children[child]);
    }

    for (size_t var_id = 0; var_id < ctx->var_count; ++var_id) {
        while (pushed[var_id] != 0) {
            stack_pop(ctx, var_id);
            pushed[var_id]--;
        }
    }
}

static void eliminate_promoted_stack_objects(Mem2RegContext* ctx) {
    for (IRBlock* block = ctx->function->first_block;
         block != NULL;
         block = block->next_block) {
        for (IRInst* inst = block->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_PHI) {
                continue;
            }

            if (inst->dst.kind == IR_OP_VREG) {
                inst->dst = resolve_substitution(ctx, inst->dst);
            }
            inst->src1 = resolve_substitution(ctx, inst->src1);
            inst->src2 = resolve_substitution(ctx, inst->src2);

            for (size_t i = 0; i < inst->extra_arg_count; ++i) {
                inst->extra_args[i] = resolve_substitution(ctx, inst->extra_args[i]);
            }
        }
    }
}

void mem2reg_run_on_function(Arena* arena, IRFunction* func) {
    if (arena == NULL || func == NULL || func->first_block == NULL) {
        return;
    }

    split_critical_edges(arena, func);

    Mem2RegContext ctx = {
        .arena = arena,
        .function = func,
        .blocks = NULL,
        .block_count = 0,
        .rpo = NULL,
        .rpo_count = 0,
        .vars = NULL,
        .var_count = 0,
        .var_cap = 0,
        .stacks = NULL,
        .substitutions = NULL,
        .substitution_cap = 0,
        .block_use = NULL,
        .block_def = NULL,
        .live_in = NULL,
        .live_out = NULL
    };

    build_cfg(&ctx);
    compute_rpo(&ctx);

    if (ctx.rpo_count == 0) {
        return;
    }

    PromoteCandidate* candidates = NULL;
    size_t candidate_count = 0;
    size_t candidate_cap = 0;
    collect_candidates(&ctx, &candidates, &candidate_count, &candidate_cap);

    for (size_t i = 0; i < candidate_count; ++i) {
        PromoteCandidate* candidate = &candidates[i];

        if (candidate->escaped || !is_scalar_size(candidate->byte_size)) {
            continue;
        }

        PromotedVar var = {
            .stack_offset = candidate->stack_offset,
            .byte_size = candidate->byte_size,
            .is_signed = candidate->is_signed,
            .id = (uint32_t)ctx.var_count,
            .def_blocks = NULL,
            .def_count = 0,
            .def_cap = 0,
            .use_blocks = NULL,
            .use_count = 0,
            .use_cap = 0,
            .local_only = false,
            .use_before_def = false,
            .local_block = NULL
        };

        ARENA_DA_PUSH(arena, ctx.vars, ctx.var_count, ctx.var_cap, var);
    }

    if (ctx.var_count == 0) {
        return;
    }

    build_var_def_use(&ctx);
    build_liveness(&ctx);

    add_entry_definition(&ctx);
    compute_dominators(&ctx);
    compute_dominance_frontiers(&ctx);

    initialize_substitution_table(&ctx);
    ctx.stacks = ARENA_NEW_ARRAY_ZERO(arena, DefStackNode*, ctx.var_count);

    for (size_t v = 0; v < ctx.var_count; ++v) {
        if (ctx.vars[v].local_only) {
            rename_local_block(&ctx, &ctx.vars[v]);
        }
    }

    place_pruned_phis(&ctx);

    if (ctx.rpo_count != 0) {
        rename_block(&ctx, ctx.rpo[0]);
    }

    eliminate_promoted_stack_objects(&ctx);
    ir_eliminate_nops(func);
}

void mem2reg_run_on_module(Arena* arena, IRModule* module) {
    if (arena == NULL || module == NULL) {
        return;
    }

    for (IRFunction* function = module->first_func;
         function != NULL;
         function = function->next) {
        mem2reg_run_on_function(arena, function);
    }
}
