#include "gvn.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define GVN_BUCKET_COUNT 4096
#define GVN_INVALID_VN UINT32_MAX
#define GVN_INVALID_VREG UINT32_MAX

typedef enum GVNKeyKind {
    GVN_KEY_EXPR = 1,
    GVN_KEY_PHI,
    GVN_KEY_LOAD
} GVNKeyKind;

typedef struct GVNExprKey {
    GVNKeyKind kind;
    IROpcode opcode;
    uint32_t lhs;
    uint32_t rhs;
    uint32_t aux;
    size_t byte_size;
    bool is_signed;
} GVNExprKey;

typedef struct GVNEntry {
    GVNExprKey key;
    uint32_t value_number;
    struct GVNEntry* next;
} GVNEntry;

typedef struct GVNLeader {
    uint32_t vreg;
    uint32_t value_number;
    struct GVNLeader* next;
} GVNLeader;

typedef struct GVNMemKey {
    IROperand base;
    size_t byte_size;
    bool is_signed;
} GVNMemKey;

typedef struct GVNMemEntry {
    GVNMemKey key;
    uint32_t value_number;
    uint32_t vreg;
    struct GVNMemEntry* next;
} GVNMemEntry;

typedef struct GVNBlock {
    IRBlock* ir;
    uint32_t id;
    size_t rpo_index;

    struct GVNBlock** preds;
    size_t pred_count;
    size_t pred_cap;

    struct GVNBlock** succs;
    size_t succ_count;
    size_t succ_cap;

    struct GVNBlock* idom;
    struct GVNBlock** children;
    size_t child_count;
    size_t child_cap;
} GVNBlock;

typedef struct GVNContext {
    Arena* arena;
    IRFunction* func;

    GVNBlock** blocks;
    size_t block_cap;
    size_t block_count;

    GVNBlock** rpo;
    size_t rpo_count;

    uint32_t* value_number;
    uint32_t next_value_number;

    GVNEntry* expr_buckets[GVN_BUCKET_COUNT];
    GVNMemEntry* mem_buckets[GVN_BUCKET_COUNT];

    GVNLeader** leader_buckets;
    size_t leader_bucket_count;
    GVNLeader** leader_stack;
    size_t leader_stack_count;
    size_t leader_stack_cap;

    GVNEntry** expr_stack;
    size_t expr_stack_count;
    size_t expr_stack_cap;

    GVNMemEntry** mem_stack;
    size_t mem_stack_count;
    size_t mem_stack_cap;
} GVNContext;

static uint64_t gvn_mix64(uint64_t x) {
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return x;
}

static uint64_t gvn_hash_bytes(const void* data, size_t len) {
    const uint8_t* p = data;
    uint64_t h = UINT64_C(1469598103934665603);

    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }

    return h;
}

static uint64_t gvn_hash_operand(IROperand op) {
    uint64_t h = UINT64_C(0x9e3779b97f4a7c15);

    h ^= gvn_mix64((uint64_t)op.kind + UINT64_C(0x100));
    h ^= gvn_mix64((uint64_t)op.byte_size + UINT64_C(0x200));
    h ^= gvn_mix64(op.is_signed ? UINT64_C(0x300) : UINT64_C(0x400));

    switch (op.kind) {
        case IR_OP_VREG:
            h ^= gvn_mix64((uint64_t)op.vreg_id);
            break;

        case IR_OP_CONST:
            h ^= gvn_mix64((uint64_t)op.int_val);
            break;

        case IR_OP_STACK:
            h ^= gvn_mix64((uint64_t)(uint32_t)op.stack_offset);
            break;

        case IR_OP_GLOBAL:
            h ^= gvn_hash_bytes(
                op.global_name.data,
                op.global_name.len
            );
            break;

        case IR_OP_STR:
            h ^= gvn_mix64((uint64_t)op.str_id);
            break;

        default:
            break;
    }

    return h;
}

static bool gvn_operand_equal(IROperand a, IROperand b) {
    if (
        a.kind != b.kind ||
        a.byte_size != b.byte_size ||
        a.is_signed != b.is_signed
    ) {
        return false;
    }

    switch (a.kind) {
        case IR_OP_NONE:
            return true;

        case IR_OP_VREG:
            return a.vreg_id == b.vreg_id;

        case IR_OP_CONST:
            return a.int_val == b.int_val;

        case IR_OP_STACK:
            return a.stack_offset == b.stack_offset;

        case IR_OP_GLOBAL:
            return strview_equals(
                a.global_name,
                b.global_name
            );

        case IR_OP_STR:
            return a.str_id == b.str_id;

        case IR_OP_BLOCK:
            return a.block == b.block;

        case IR_OP_REG:
            return a.reg == b.reg;

        default:
            return false;
    }
}

static bool gvn_key_equal(
    const GVNExprKey* a,
    const GVNExprKey* b
) {
    return
        a->kind == b->kind &&
        a->opcode == b->opcode &&
        a->lhs == b->lhs &&
        a->rhs == b->rhs &&
        a->aux == b->aux &&
        a->byte_size == b->byte_size &&
        a->is_signed == b->is_signed;
}

static uint64_t gvn_hash_key(
    const GVNExprKey* key
) {
    uint64_t h = UINT64_C(0x243f6a8885a308d3);

    h ^= gvn_mix64((uint64_t)key->kind);
    h ^= gvn_mix64((uint64_t)key->opcode << 1);
    h ^= gvn_mix64((uint64_t)key->lhs << 7);
    h ^= gvn_mix64((uint64_t)key->rhs << 17);
    h ^= gvn_mix64((uint64_t)key->aux << 29);
    h ^= gvn_mix64((uint64_t)key->byte_size << 37);
    h ^= gvn_mix64(key->is_signed ? 1 : 0);

    return h;
}

static uint64_t gvn_hash_mem_key(
    const GVNMemKey* key
) {
    uint64_t h = gvn_hash_operand(key->base);
    h ^= gvn_mix64((uint64_t)key->byte_size);
    h ^= gvn_mix64(key->is_signed ? 1 : 0);

    return h;
}

static bool gvn_mem_key_equal(
    const GVNMemKey* a,
    const GVNMemKey* b
) {
    return
        a->byte_size == b->byte_size &&
        a->is_signed == b->is_signed &&
        gvn_operand_equal(a->base, b->base);
}

static bool gvn_is_commutative(IROpcode opcode) {
    switch (opcode) {
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

static bool gvn_is_pure(IROpcode opcode) {
    switch (opcode) {
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
        case IR_ADDR:
        case IR_GLOBAL_STR:
            return true;

        default:
            return false;
    }
}

static bool gvn_is_memory_read(const IRInst* inst) {
    if (inst->opcode == IR_LOAD) {
        return true;
    }

    if (
        inst->opcode == IR_MOV &&
        inst->dst.kind == IR_OP_VREG &&
        (
            inst->src1.kind == IR_OP_STACK ||
            inst->src1.kind == IR_OP_GLOBAL
        )
    ) {
        return true;
    }

    return false;
}

static bool gvn_is_memory_write(const IRInst* inst) {
    if (inst->opcode == IR_STORE) {
        return true;
    }

    if (
        inst->opcode == IR_MOV &&
        (
            inst->dst.kind == IR_OP_STACK ||
            inst->dst.kind == IR_OP_GLOBAL
        )
    ) {
        return true;
    }

    return false;
}

static bool gvn_is_memory_clobber(const IRInst* inst) {
    switch (inst->opcode) {
        case IR_MEMCPY:
        case IR_CALL:
        case IR_CALL_PTR:
        case IR_TAIL_CALL:
        case IR_TAIL_CALL_PTR:
        case IR_INLINE_ASM:
        case IR_VA_START:
        case IR_VA_END:
        case IR_VA_COPY:
            return true;

        default:
            return false;
    }
}

static GVNBlock* gvn_find_block(
    const GVNContext* ctx,
    const IRBlock* block
) {
    if (
        block == NULL ||
        block->id >= ctx->block_cap
    ) {
        return NULL;
    }

    return ctx->blocks[block->id];
}

static void gvn_add_edge(
    GVNContext* ctx,
    GVNBlock* pred,
    GVNBlock* succ
) {
    if (pred == NULL || succ == NULL) {
        return;
    }

    for (size_t i = 0; i < pred->succ_count; ++i) {
        if (pred->succs[i] == succ) {
            return;
        }
    }

    ARENA_DA_PUSH(
        ctx->arena,
        pred->succs,
        pred->succ_count,
        pred->succ_cap,
        succ
    );

    ARENA_DA_PUSH(
        ctx->arena,
        succ->preds,
        succ->pred_count,
        succ->pred_cap,
        pred
    );
}

static void gvn_build_cfg(GVNContext* ctx) {
    for (size_t i = 0; i < ctx->block_cap; ++i) {
        GVNBlock* block = ctx->blocks[i];

        if (block == NULL) {
            continue;
        }

        IRInst* term = block->ir->last_inst;

        if (term == NULL) {
            if (block->ir->next_block != NULL) {
                gvn_add_edge(
                    ctx,
                    block,
                    gvn_find_block(
                        ctx,
                        block->ir->next_block
                    )
                );
            }

            continue;
        }

        switch (term->opcode) {
            case IR_JMP:
                if (term->dst.kind == IR_OP_BLOCK) {
                    gvn_add_edge(
                        ctx,
                        block,
                        gvn_find_block(
                            ctx,
                            term->dst.block
                        )
                    );
                }
                break;

            case IR_BR:
                if (term->src1.kind == IR_OP_BLOCK) {
                    gvn_add_edge(
                        ctx,
                        block,
                        gvn_find_block(
                            ctx,
                            term->src1.block
                        )
                    );
                }

                if (term->src2.kind == IR_OP_BLOCK) {
                    gvn_add_edge(
                        ctx,
                        block,
                        gvn_find_block(
                            ctx,
                            term->src2.block
                        )
                    );
                }
                break;

            case IR_RET:
            case IR_TAIL_CALL:
            case IR_TAIL_CALL_PTR:
                break;

            default:
                if (block->ir->next_block != NULL) {
                    gvn_add_edge(
                        ctx,
                        block,
                        gvn_find_block(
                            ctx,
                            block->ir->next_block
                        )
                    );
                }
                break;
        }
    }
}

static void gvn_rpo_dfs(
    GVNBlock* block,
    bool* visited,
    GVNBlock** postorder,
    size_t* count
) {
    if (visited[block->id]) {
        return;
    }

    visited[block->id] = true;

    for (size_t i = 0; i < block->succ_count; ++i) {
        gvn_rpo_dfs(
            block->succs[i],
            visited,
            postorder,
            count
        );
    }

    postorder[(*count)++] = block;
}

static GVNBlock* gvn_intersect(
    GVNBlock* lhs,
    GVNBlock* rhs
) {
    GVNBlock* a = lhs;
    GVNBlock* b = rhs;

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

static void gvn_build_dominators(
    GVNContext* ctx
) {
    GVNBlock* entry =
        gvn_find_block(
            ctx,
            ctx->func->first_block
        );

    if (entry == NULL) {
        return;
    }

    bool* visited =
        ARENA_NEW_ARRAY_ZERO(
            ctx->arena,
            bool,
            ctx->block_cap
        );

    GVNBlock** postorder =
        ARENA_NEW_ARRAY(
            ctx->arena,
            GVNBlock*,
            ctx->block_count
        );

    size_t post_count = 0;

    gvn_rpo_dfs(
        entry,
        visited,
        postorder,
        &post_count
    );

    ctx->rpo =
        ARENA_NEW_ARRAY(
            ctx->arena,
            GVNBlock*,
            post_count
        );

    ctx->rpo_count = post_count;

    for (size_t i = 0; i < post_count; ++i) {
        GVNBlock* block =
            postorder[
                post_count - 1 - i
            ];

        block->rpo_index = i;
        ctx->rpo[i] = block;
    }

    ctx->rpo[0]->idom =
        ctx->rpo[0];

    bool changed = true;

    while (changed) {
        changed = false;

        for (size_t i = 1; i < post_count; ++i) {
            GVNBlock* block =
                ctx->rpo[i];

            GVNBlock* new_idom = NULL;

            for (
                size_t p = 0;
                p < block->pred_count;
                ++p
            ) {
                GVNBlock* pred =
                    block->preds[p];

                if (pred->idom == NULL) {
                    continue;
                }

                if (new_idom == NULL) {
                    new_idom = pred;
                } else {
                    new_idom =
                        gvn_intersect(
                            pred,
                            new_idom
                        );
                }
            }

            if (
                new_idom != NULL &&
                block->idom != new_idom
            ) {
                block->idom = new_idom;
                changed = true;
            }
        }
    }

    for (size_t i = 1; i < post_count; ++i) {
        GVNBlock* block = ctx->rpo[i];

        if (
            block->idom != NULL &&
            block->idom != block
        ) {
            ARENA_DA_PUSH(
                ctx->arena,
                block->idom->children,
                block->idom->child_count,
                block->idom->child_cap,
                block
            );
        }
    }
}

static GVNEntry* gvn_lookup_expr(const GVNContext* ctx, const GVNExprKey* key);
static GVNEntry* gvn_insert_expr(GVNContext* ctx, const GVNExprKey* key, uint32_t value_number);
static uint32_t gvn_new_value_number(GVNContext* ctx);

static uint32_t gvn_operand_value_number(
    GVNContext* ctx,
    IROperand operand
) {
    uint32_t existing_vn = GVN_INVALID_VN;

    if (operand.kind == IR_OP_VREG) {
        if (operand.vreg_id < ctx->func->next_vreg_id) {
            return ctx->value_number[
                operand.vreg_id
            ];
        }

        return GVN_INVALID_VN;
    }

    GVNExprKey key = {
        .kind = GVN_KEY_EXPR,
        .opcode = IR_NOP,
        .lhs = 0,
        .rhs = 0,
        .aux = 0,
        .byte_size = operand.byte_size,
        .is_signed = operand.is_signed
    };

    switch (operand.kind) {
        case IR_OP_CONST:
            key.lhs = (uint32_t)(uint64_t)operand.int_val;
            key.rhs = (uint32_t)((uint64_t)operand.int_val >> 32);
            key.aux = 1;
            break;

        case IR_OP_STACK:
            key.lhs = (uint32_t)operand.stack_offset;
            key.rhs = 0;
            key.aux = 2;
            break;

        case IR_OP_GLOBAL:
            key.lhs = (uint32_t)gvn_hash_bytes(
                operand.global_name.data,
                operand.global_name.len
            );
            key.rhs = (uint32_t)operand.global_name.len;
            key.aux = 3;
            break;

        case IR_OP_STR:
            key.lhs = operand.str_id;
            key.aux = 4;
            break;

        default:
            return GVN_INVALID_VN;
    }

    GVNEntry* entry =
        gvn_lookup_expr(ctx, &key);

    if (entry != NULL) {
        return entry->value_number;
    }

    existing_vn = gvn_new_value_number(ctx);

    gvn_insert_expr(
        ctx,
        &key,
        existing_vn
    );

    return existing_vn;
}

static GVNEntry* gvn_lookup_expr(
    const GVNContext* ctx,
    const GVNExprKey* key
) {
    size_t bucket =
        (size_t)(
            gvn_hash_key(key) %
            GVN_BUCKET_COUNT
        );

    for (
        GVNEntry* entry =
            ctx->expr_buckets[bucket];
        entry != NULL;
        entry = entry->next
    ) {
        if (gvn_key_equal(&entry->key, key)) {
            return entry;
        }
    }

    return NULL;
}

static GVNEntry* gvn_insert_expr(
    GVNContext* ctx,
    const GVNExprKey* key,
    uint32_t value_number
) {
    size_t bucket =
        (size_t)(
            gvn_hash_key(key) %
            GVN_BUCKET_COUNT
        );

    GVNEntry* entry =
        ARENA_NEW(ctx->arena, GVNEntry);

    entry->key = *key;
    entry->value_number =
        value_number;
    entry->next =
        ctx->expr_buckets[bucket];

    ctx->expr_buckets[bucket] = entry;

    ARENA_DA_PUSH(
        ctx->arena,
        ctx->expr_stack,
        ctx->expr_stack_count,
        ctx->expr_stack_cap,
        entry
    );

    return entry;
}

static GVNMemEntry* gvn_lookup_mem(
    const GVNContext* ctx,
    const GVNMemKey* key
) {
    size_t bucket =
        (size_t)(
            gvn_hash_mem_key(key) %
            GVN_BUCKET_COUNT
        );

    for (
        GVNMemEntry* entry =
            ctx->mem_buckets[bucket];
        entry != NULL;
        entry = entry->next
    ) {
        if (gvn_mem_key_equal(&entry->key, key)) {
            return entry;
        }
    }

    return NULL;
}

static GVNMemEntry* gvn_insert_mem(
    GVNContext* ctx,
    const GVNMemKey* key,
    uint32_t value_number,
    uint32_t vreg
) {
    size_t bucket =
        (size_t)(
            gvn_hash_mem_key(key) %
            GVN_BUCKET_COUNT
        );

    GVNMemEntry* entry =
        ARENA_NEW(ctx->arena, GVNMemEntry);

    entry->key = *key;
    entry->value_number =
        value_number;
    entry->vreg = vreg;
    entry->next =
        ctx->mem_buckets[bucket];

    ctx->mem_buckets[bucket] = entry;

    ARENA_DA_PUSH(
        ctx->arena,
        ctx->mem_stack,
        ctx->mem_stack_count,
        ctx->mem_stack_cap,
        entry
    );

    return entry;
}

static void gvn_remove_expr(
    GVNContext* ctx,
    GVNEntry* target
) {
    size_t bucket =
        (size_t)(
            gvn_hash_key(&target->key) %
            GVN_BUCKET_COUNT
        );

    GVNEntry** link =
        &ctx->expr_buckets[bucket];

    while (*link != NULL) {
        if (*link == target) {
            *link = target->next;
            return;
        }

        link = &(*link)->next;
    }
}

static void gvn_remove_mem(
    GVNContext* ctx,
    GVNMemEntry* target
) {
    size_t bucket =
        (size_t)(
            gvn_hash_mem_key(&target->key) %
            GVN_BUCKET_COUNT
        );

    GVNMemEntry** link =
        &ctx->mem_buckets[bucket];

    while (*link != NULL) {
        if (*link == target) {
            *link = target->next;
            return;
        }

        link = &(*link)->next;
    }
}

static void gvn_remove_leader(
    GVNContext* ctx,
    GVNLeader* target
) {
    size_t bucket =
        (size_t)(
            gvn_mix64(
                target->value_number
            ) %
            ctx->leader_bucket_count
        );

    GVNLeader** link =
        &ctx->leader_buckets[bucket];

    while (*link != NULL) {
        if (*link == target) {
            *link = target->next;
            return;
        }

        link = &(*link)->next;
    }
}


static void gvn_unwind(
    GVNContext* ctx,
    size_t expr_mark,
    size_t mem_mark,
    size_t leader_mark
) {
    while (
        ctx->leader_stack_count >
        leader_mark
    ) {
        GVNLeader* leader =
            ctx->leader_stack[
                --ctx->leader_stack_count
            ];

        gvn_remove_leader(
            ctx,
            leader
        );
    }

    while (
        ctx->expr_stack_count >
        expr_mark
    ) {
        GVNEntry* entry =
            ctx->expr_stack[
                --ctx->expr_stack_count
            ];

        gvn_remove_expr(
            ctx,
            entry
        );
    }

    while (
        ctx->mem_stack_count >
        mem_mark
    ) {
        GVNMemEntry* entry =
            ctx->mem_stack[
                --ctx->mem_stack_count
            ];

        gvn_remove_mem(
            ctx,
            entry
        );
    }
}

static uint32_t gvn_new_value_number(
    GVNContext* ctx
) {
    assert(
        ctx->next_value_number !=
        GVN_INVALID_VN
    );

    return ctx->next_value_number++;
}

static void gvn_set_vreg_value(
    GVNContext* ctx,
    uint32_t vreg,
    uint32_t value_number
) {
    if (
        vreg < ctx->func->next_vreg_id
    ) {
        ctx->value_number[vreg] =
            value_number;
    }
}

static uint32_t gvn_get_leader(
    const GVNContext* ctx,
    uint32_t value_number
) {
    if (
        value_number == GVN_INVALID_VN
    ) {
        return GVN_INVALID_VREG;
    }

    size_t bucket =
        (size_t)(
            gvn_mix64(value_number) %
            ctx->leader_bucket_count
        );

    for (
        GVNLeader* leader =
            ctx->leader_buckets[bucket];
        leader != NULL;
        leader = leader->next
    ) {
        if (
            leader->value_number ==
            value_number
        ) {
            return leader->vreg;
        }
    }

    return GVN_INVALID_VREG;
}

static void gvn_set_leader(
    GVNContext* ctx,
    uint32_t value_number,
    uint32_t vreg
) {
    if (
        value_number == GVN_INVALID_VN ||
        vreg == GVN_INVALID_VREG
    ) {
        return;
    }

    size_t bucket =
        (size_t)(
            gvn_mix64(value_number) %
            ctx->leader_bucket_count
        );

    GVNLeader* leader =
        ARENA_NEW(ctx->arena, GVNLeader);

    leader->value_number =
        value_number;
    leader->vreg = vreg;
    leader->next =
        ctx->leader_buckets[bucket];

    ctx->leader_buckets[bucket] = leader;

    ARENA_DA_PUSH(
        ctx->arena,
        ctx->leader_stack,
        ctx->leader_stack_count,
        ctx->leader_stack_cap,
        leader
    );
}

static uint32_t gvn_make_expr_number(
    GVNContext* ctx,
    IROpcode opcode,
    uint32_t lhs,
    uint32_t rhs,
    uint32_t aux,
    size_t byte_size,
    bool is_signed
) {
    if (
        lhs == GVN_INVALID_VN ||
        (
            rhs == GVN_INVALID_VN &&
            opcode != IR_NEG &&
            opcode != IR_NOT &&
            opcode != IR_ADDR &&
            opcode != IR_GLOBAL_STR
        )
    ) {
        return GVN_INVALID_VN;
    }

    if (
        gvn_is_commutative(opcode) &&
        lhs > rhs
    ) {
        uint32_t tmp = lhs;
        lhs = rhs;
        rhs = tmp;
    }

    GVNExprKey key = {
        .kind = GVN_KEY_EXPR,
        .opcode = opcode,
        .lhs = lhs,
        .rhs = rhs,
        .aux = aux,
        .byte_size = byte_size,
        .is_signed = is_signed
    };

    GVNEntry* existing =
        gvn_lookup_expr(
            ctx,
            &key
        );

    if (existing != NULL) {
        return existing->value_number;
    }

    uint32_t value_number =
        gvn_new_value_number(ctx);

    gvn_insert_expr(
        ctx,
        &key,
        value_number
    );

    return value_number;
}

static uint32_t gvn_make_phi_number(
    GVNContext* ctx,
    const IRInst* inst
) {
    uint32_t first = GVN_INVALID_VN;
    bool all_equal = true;
    size_t incoming = 0;
    uint32_t hash = 0;

    for (
        size_t i = 0;
        i + 1 < inst->extra_arg_count;
        i += 2
    ) {
        IROperand value =
            inst->extra_args[i];

        if (
            value.kind != IR_OP_VREG &&
            value.kind != IR_OP_CONST
        ) {
            return GVN_INVALID_VN;
        }

        uint32_t vn =
            gvn_operand_value_number(
                ctx,
                value
            );

        if (
            vn == GVN_INVALID_VN
        ) {
            return GVN_INVALID_VN;
        }

        if (first == GVN_INVALID_VN) {
            first = vn;
        } else if (first != vn) {
            all_equal = false;
        }

        hash ^=
            (uint32_t)gvn_mix64(
                ((uint64_t)vn << 1) ^
                (uint64_t)incoming
            );

        ++incoming;
    }

    if (incoming == 0) {
        return GVN_INVALID_VN;
    }

    if (all_equal) {
        return first;
    }

    GVNExprKey key = {
        .kind = GVN_KEY_PHI,
        .opcode = IR_PHI,
        .lhs = hash,
        .rhs = (uint32_t)incoming,
        .aux = first,
        .byte_size = inst->dst.byte_size,
        .is_signed = inst->dst.is_signed
    };

    GVNEntry* existing =
        gvn_lookup_expr(
            ctx,
            &key
        );

    if (existing != NULL) {
        return existing->value_number;
    }

    uint32_t value_number =
        gvn_new_value_number(ctx);

    gvn_insert_expr(
        ctx,
        &key,
        value_number
    );

    return value_number;
}

static bool gvn_mem_operand(
    const IRInst* inst,
    IROperand* out
) {
    if (inst->opcode == IR_LOAD) {
        if (
            inst->src1.kind != IR_OP_STACK &&
            inst->src1.kind != IR_OP_GLOBAL
        ) {
            return false;
        }

        *out = inst->src1;
        return true;
    }

    if (
        inst->opcode == IR_MOV &&
        inst->dst.kind == IR_OP_VREG &&
        (
            inst->src1.kind == IR_OP_STACK ||
            inst->src1.kind == IR_OP_GLOBAL
        )
    ) {
        *out = inst->src1;
        return true;
    }

    return false;
}

static bool gvn_store_mem_operand(
    const IRInst* inst,
    IROperand* out
) {
    if (inst->opcode == IR_STORE) {
        if (
            inst->dst.kind != IR_OP_STACK &&
            inst->dst.kind != IR_OP_GLOBAL
        ) {
            return false;
        }

        *out = inst->dst;
        return true;
    }

    if (
        inst->opcode == IR_MOV &&
        (
            inst->dst.kind == IR_OP_STACK ||
            inst->dst.kind == IR_OP_GLOBAL
        )
    ) {
        *out = inst->dst;
        return true;
    }

    return false;
}

static void gvn_invalidate_memory(
    GVNContext* ctx
) {
    while (ctx->mem_stack_count > 0) {
        GVNMemEntry* entry =
            ctx->mem_stack[
                --ctx->mem_stack_count
            ];

        gvn_remove_mem(
            ctx,
            entry
        );
    }
}

static bool gvn_try_forward_load(
    GVNContext* ctx,
    IRInst* inst,
    bool* changed
) {
    IROperand location;

    if (
        !gvn_mem_operand(
            inst,
            &location
        )
    ) {
        return false;
    }

    GVNMemKey key = {
        .base = location,
        .byte_size = inst->dst.byte_size,
        .is_signed = inst->dst.is_signed
    };

    GVNMemEntry* entry =
        gvn_lookup_mem(
            ctx,
            &key
        );

    if (entry == NULL) {
        return false;
    }

    uint32_t leader =
        gvn_get_leader(
            ctx,
            entry->value_number
        );

    if (
        leader == GVN_INVALID_VREG ||
        leader == inst->dst.vreg_id
    ) {
        return false;
    }

    inst->opcode = IR_MOV;
    inst->src1 =
        ir_op_vreg(
            leader,
            inst->dst.byte_size,
            inst->dst.is_signed
        );
    inst->src2 =
        ir_op_none();

    *changed = true;

    return true;
}

static void gvn_record_store(
    GVNContext* ctx,
    IRInst* inst
) {
    IROperand location;

    if (
        !gvn_store_mem_operand(
            inst,
            &location
        )
    ) {
        return;
    }

    if (
        inst->opcode == IR_MOV &&
        (
            inst->src1.kind == IR_OP_STACK ||
            inst->src1.kind == IR_OP_GLOBAL
        )
    ) {
        gvn_invalidate_memory(ctx);
        return;
    }

    uint32_t value_number =
        gvn_operand_value_number(
            ctx,
            inst->src1
        );

    if (
        value_number ==
        GVN_INVALID_VN
    ) {
        gvn_invalidate_memory(ctx);
        return;
    }

    GVNMemKey key = {
        .base = location,
        .byte_size = inst->src1.byte_size,
        .is_signed = inst->src1.is_signed
    };

    gvn_insert_mem(
        ctx,
        &key,
        value_number,
        inst->src1.kind == IR_OP_VREG
            ? inst->src1.vreg_id
            : GVN_INVALID_VREG
    );
}

static void gvn_process_inst(
    GVNContext* ctx,
    IRInst* inst
) {
    if (inst->opcode == IR_NOP) {
        return;
    }

    if (gvn_is_memory_clobber(inst)) {
        gvn_invalidate_memory(ctx);

        if (
            inst->dst.kind ==
                IR_OP_VREG
        ) {
            gvn_set_vreg_value(
                ctx,
                inst->dst.vreg_id,
                GVN_INVALID_VN
            );
        }

        return;
    }

    if (gvn_is_memory_read(inst)) {
        bool changed = false;

        if (
            gvn_try_forward_load(
                ctx,
                inst,
                &changed
            )
        ) {
            if (changed) {
                uint32_t vn =
                    gvn_operand_value_number(
                        ctx,
                        inst->src1
                    );

                gvn_set_vreg_value(
                    ctx,
                    inst->dst.vreg_id,
                    vn
                );

                gvn_set_leader(
                    ctx,
                    vn,
                    inst->dst.vreg_id
                );
            }

            return;
        }

        uint32_t vn =
            gvn_make_expr_number(
                ctx,
                IR_LOAD,
                gvn_operand_value_number(
                    ctx,
                    inst->src1
                ),
                GVN_INVALID_VN,
                0,
                inst->dst.byte_size,
                inst->dst.is_signed
            );

        if (
            vn != GVN_INVALID_VN
        ) {
            gvn_set_vreg_value(
                ctx,
                inst->dst.vreg_id,
                vn
            );

            gvn_set_leader(
                ctx,
                vn,
                inst->dst.vreg_id
            );
        }

        return;
    }

    if (gvn_is_memory_write(inst)) {
        gvn_record_store(
            ctx,
            inst
        );
        return;
    }

    if (
        inst->opcode == IR_PHI &&
        inst->dst.kind == IR_OP_VREG
    ) {
        uint32_t vn =
            gvn_make_phi_number(
                ctx,
                inst
            );

        if (
            vn != GVN_INVALID_VN
        ) {
            gvn_set_vreg_value(
                ctx,
                inst->dst.vreg_id,
                vn
            );

            gvn_set_leader(
                ctx,
                vn,
                inst->dst.vreg_id
            );
        }

        return;
    }

    if (
        inst->opcode == IR_MOV &&
        inst->dst.kind == IR_OP_VREG
    ) {
        uint32_t vn =
            gvn_operand_value_number(
                ctx,
                inst->src1
            );

        if (
            vn != GVN_INVALID_VN
        ) {
            gvn_set_vreg_value(
                ctx,
                inst->dst.vreg_id,
                vn
            );

            uint32_t leader =
                gvn_get_leader(
                    ctx,
                    vn
                );

            if (
                leader !=
                    GVN_INVALID_VREG &&
                leader !=
                    inst->dst.vreg_id &&
                inst->src1.kind ==
                    IR_OP_VREG
            ) {
                inst->src1 =
                    ir_op_vreg(
                        leader,
                        inst->dst.byte_size,
                        inst->dst.is_signed
                    );
            }

            gvn_set_leader(
                ctx,
                vn,
                inst->dst.vreg_id
            );
        }

        return;
    }

    if (
        inst->dst.kind != IR_OP_VREG ||
        !gvn_is_pure(inst->opcode)
    ) {
        return;
    }

    uint32_t lhs =
        gvn_operand_value_number(
            ctx,
            inst->src1
        );

    uint32_t rhs =
        gvn_operand_value_number(
            ctx,
            inst->src2
        );

    uint32_t vn =
        gvn_make_expr_number(
            ctx,
            inst->opcode,
            lhs,
            rhs,
            0,
            inst->dst.byte_size,
            inst->dst.is_signed
        );

    if (
        vn == GVN_INVALID_VN
    ) {
        return;
    }

    uint32_t leader =
        gvn_get_leader(
            ctx,
            vn
        );

    gvn_set_vreg_value(
        ctx,
        inst->dst.vreg_id,
        vn
    );

    if (
        leader != GVN_INVALID_VREG &&
        leader != inst->dst.vreg_id
    ) {
        inst->opcode = IR_MOV;
        inst->src1 =
            ir_op_vreg(
                leader,
                inst->dst.byte_size,
                inst->dst.is_signed
            );
        inst->src2 =
            ir_op_none();
        inst->extra_args = NULL;
        inst->extra_arg_count = 0;
    }

    gvn_set_leader(
        ctx,
        vn,
        inst->dst.vreg_id
    );
}

static void gvn_dom_walk(
    GVNContext* ctx,
    GVNBlock* block
) {
    gvn_invalidate_memory(ctx);

    size_t expr_mark =
        ctx->expr_stack_count;

    size_t mem_mark =
        ctx->mem_stack_count;

    size_t leader_mark =
        ctx->leader_stack_count;

    for (
        IRInst* inst =
            block->ir->first_inst;
        inst != NULL;
        inst = inst->next
    ) {
        gvn_process_inst(
            ctx,
            inst
        );
    }

    for (
        size_t i = 0;
        i < block->child_count;
        ++i
    ) {
        gvn_dom_walk(
            ctx,
            block->children[i]
        );
    }

    gvn_unwind(
        ctx,
        expr_mark,
        mem_mark,
        leader_mark
    );
}

void gvn_run_on_function(
    Arena* arena,
    IRFunction* func
) {
    if (
        arena == NULL ||
        func == NULL ||
        func->first_block == NULL
    ) {
        return;
    }

    size_t block_cap =
        (size_t)func->next_block_id + 1;

    size_t vreg_cap =
        (size_t)func->next_vreg_id;

    if (
        block_cap == 0 ||
        vreg_cap == 0
    ) {
        return;
    }

    GVNContext ctx = {
        .arena = arena,
        .func = func,
        .blocks = ARENA_NEW_ARRAY_ZERO(
            arena,
            GVNBlock*,
            block_cap
        ),
        .block_cap = block_cap,
        .block_count = 0,
        .rpo = NULL,
        .rpo_count = 0,
        .value_number = ARENA_NEW_ARRAY(
            arena,
            uint32_t,
            vreg_cap
        ),
        .next_value_number = 1,
        .leader_buckets = ARENA_NEW_ARRAY_ZERO(
            arena,
            GVNLeader*,
            vreg_cap * 2 + 1
        ),
        .leader_bucket_count =
            vreg_cap * 2 + 1
    };

    for (size_t i = 0; i < vreg_cap; ++i) {
        ctx.value_number[i] = GVN_INVALID_VN;
    }

    size_t count = 0;

    for (
        IRBlock* block =
            func->first_block;
        block != NULL;
        block = block->next_block
    ) {
        if (
            block->id >= block_cap ||
            ctx.blocks[block->id] != NULL
        ) {
            return;
        }

        GVNBlock* gvn_block =
            ARENA_NEW_ZERO(
                arena,
                GVNBlock
            );

        gvn_block->ir = block;
        gvn_block->id = block->id;

        ctx.blocks[block->id] =
            gvn_block;

        ++count;
    }

    ctx.block_count = count;

    gvn_build_cfg(&ctx);
    gvn_build_dominators(&ctx);

    if (
        ctx.rpo_count == 0
    ) {
        return;
    }

    gvn_dom_walk(
        &ctx,
        ctx.rpo[0]
    );
}

void gvn_run_on_module(
    Arena* arena,
    IRModule* module
) {
    if (
        arena == NULL ||
        module == NULL
    ) {
        return;
    }

    for (
        IRFunction* func =
            module->first_func;
        func != NULL;
        func = func->next
    ) {
        gvn_run_on_function(
            arena,
            func
        );
    }
}
