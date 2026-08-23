#include "ir.h"
#include "regalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#define K_REG_COUNT 9

static const X86Reg ALLOCATABLE_REGS[K_REG_COUNT] = {
    REG_RDI, REG_RSI, REG_R8,  REG_R9, 
    REG_RBX, REG_R12, REG_R13, REG_R14, REG_R15
};

static const X86Reg ALLOCATABLE_CALLER_SAVED[] = {
    REG_RDI, REG_RSI, REG_R8, REG_R9
};
#define ALLOCATABLE_CALLER_SAVED_COUNT ((size_t)(sizeof(ALLOCATABLE_CALLER_SAVED) / sizeof(ALLOCATABLE_CALLER_SAVED[0])))

static const X86Reg ALL_CALLER_SAVED_REGS[] = {
    REG_RAX, REG_RCX, REG_RDX, REG_RSI, REG_RDI, REG_R8, REG_R9, REG_R10, REG_R11
};
#define ALL_CALLER_SAVED_COUNT ((size_t)(sizeof(ALL_CALLER_SAVED_REGS) / sizeof(ALL_CALLER_SAVED_REGS[0])))

static const X86Reg CALLEE_SAVED_REGS[] = {
    REG_RBX, REG_R12, REG_R13, REG_R14, REG_R15
};
#define CALLEE_SAVED_COUNT ((size_t)(sizeof(CALLEE_SAVED_REGS) / sizeof(CALLEE_SAVED_REGS[0])))

const char* reg_name(X86Reg reg, size_t byte_size) {
    if (byte_size == 0) {
        byte_size = 8;
    }

    switch (reg) {
        case REG_RAX:
            if (byte_size == 1) return "al";
            if (byte_size == 2) return "ax";
            if (byte_size == 4) return "eax";
            return "rax";

        case REG_RCX:
            if (byte_size == 1) return "cl";
            if (byte_size == 2) return "cx";
            if (byte_size == 4) return "ecx";
            return "rcx";

        case REG_RDX:
            if (byte_size == 1) return "dl";
            if (byte_size == 2) return "dx";
            if (byte_size == 4) return "edx";
            return "rdx";

        case REG_RBX:
            if (byte_size == 1) return "bl";
            if (byte_size == 2) return "bx";
            if (byte_size == 4) return "ebx";
            return "rbx";

        case REG_RSI:
            if (byte_size == 1) return "sil";
            if (byte_size == 2) return "si";
            if (byte_size == 4) return "esi";
            return "rsi";

        case REG_RDI:
            if (byte_size == 1) return "dil";
            if (byte_size == 2) return "di";
            if (byte_size == 4) return "edi";
            return "rdi";

        case REG_RSP:
            if (byte_size == 1) return "spl";
            if (byte_size == 2) return "sp";
            if (byte_size == 4) return "esp";
            return "rsp";

        case REG_RBP:
            if (byte_size == 1) return "bpl";
            if (byte_size == 2) return "bp";
            if (byte_size == 4) return "ebp";
            return "rbp";

        case REG_R8:
            if (byte_size == 1) return "r8b";
            if (byte_size == 2) return "r8w";
            if (byte_size == 4) return "r8d";
            return "r8";

        case REG_R9:
            if (byte_size == 1) return "r9b";
            if (byte_size == 2) return "r9w";
            if (byte_size == 4) return "r9d";
            return "r9";

        case REG_R10:
            if (byte_size == 1) return "r10b";
            if (byte_size == 2) return "r10w";
            if (byte_size == 4) return "r10d";
            return "r10";

        case REG_R11:
            if (byte_size == 1) return "r11b";
            if (byte_size == 2) return "r11w";
            if (byte_size == 4) return "r11d";
            return "r11";

        case REG_R12:
            if (byte_size == 1) return "r12b";
            if (byte_size == 2) return "r12w";
            if (byte_size == 4) return "r12d";
            return "r12";

        case REG_R13:
            if (byte_size == 1) return "r13b";
            if (byte_size == 2) return "r13w";
            if (byte_size == 4) return "r13d";
            return "r13";

        case REG_R14:
            if (byte_size == 1) return "r14b";
            if (byte_size == 2) return "r14w";
            if (byte_size == 4) return "r14d";
            return "r14";

        case REG_R15:
            if (byte_size == 1) return "r15b";
            if (byte_size == 2) return "r15w";
            if (byte_size == 4) return "r15d";
            return "r15";

        default:
            return "unknown_reg";
    }
}

bool reg_is_callee_saved(X86Reg reg) {
    return (reg == REG_RBX || reg == REG_R12 || reg == REG_R13 || reg == REG_R14 || reg == REG_R15);
}

X86Reg parse_reg_name(StrView name, size_t* out_byte_size) {
    if (name.len == 0 || name.data == NULL) {
        return REG_NONE;
    }

    if (name.len == 2) {
        if (memcmp(name.data, "al", 2) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RAX; }
        if (memcmp(name.data, "cl", 2) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RCX; }
        if (memcmp(name.data, "dl", 2) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RDX; }
        if (memcmp(name.data, "bl", 2) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RBX; }

        if (memcmp(name.data, "ax", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RAX; }
        if (memcmp(name.data, "cx", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RCX; }
        if (memcmp(name.data, "dx", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RDX; }
        if (memcmp(name.data, "bx", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RBX; }
        if (memcmp(name.data, "si", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RSI; }
        if (memcmp(name.data, "di", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RDI; }
        if (memcmp(name.data, "sp", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RSP; }
        if (memcmp(name.data, "bp", 2) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_RBP; }

        if (memcmp(name.data, "r8", 2) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R8; }
        if (memcmp(name.data, "r9", 2) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R9; }
    }

    if (name.len == 3) {
        if (memcmp(name.data, "eax", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RAX; }
        if (memcmp(name.data, "ecx", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RCX; }
        if (memcmp(name.data, "edx", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RDX; }
        if (memcmp(name.data, "ebx", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RBX; }
        if (memcmp(name.data, "esi", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RSI; }
        if (memcmp(name.data, "edi", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RDI; }
        if (memcmp(name.data, "esp", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RSP; }
        if (memcmp(name.data, "ebp", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_RBP; }

        if (memcmp(name.data, "rax", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RAX; }
        if (memcmp(name.data, "rcx", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RCX; }
        if (memcmp(name.data, "rdx", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RDX; }
        if (memcmp(name.data, "rbx", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RBX; }
        if (memcmp(name.data, "rsi", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RSI; }
        if (memcmp(name.data, "rdi", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RDI; }
        if (memcmp(name.data, "rsp", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RSP; }
        if (memcmp(name.data, "rbp", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_RBP; }

        if (memcmp(name.data, "sil", 3) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RSI; }
        if (memcmp(name.data, "dil", 3) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RDI; }
        if (memcmp(name.data, "spl", 3) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RSP; }
        if (memcmp(name.data, "bpl", 3) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_RBP; }

        if (memcmp(name.data, "r8b", 3) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R8; }
        if (memcmp(name.data, "r9b", 3) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R9; }
        if (memcmp(name.data, "r8w", 3) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R8; }
        if (memcmp(name.data, "r9w", 3) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R9; }
        if (memcmp(name.data, "r8d", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R8; }
        if (memcmp(name.data, "r9d", 3) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R9; }

        if (memcmp(name.data, "r10", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R10; }
        if (memcmp(name.data, "r11", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R11; }
        if (memcmp(name.data, "r12", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R12; }
        if (memcmp(name.data, "r13", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R13; }
        if (memcmp(name.data, "r14", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R14; }
        if (memcmp(name.data, "r15", 3) == 0) { if (out_byte_size) *out_byte_size = 8; return REG_R15; }
    }

    if (name.len == 4) {
        if (memcmp(name.data, "r10b", 4) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R10; }
        if (memcmp(name.data, "r11b", 4) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R11; }
        if (memcmp(name.data, "r12b", 4) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R12; }
        if (memcmp(name.data, "r13b", 4) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R13; }
        if (memcmp(name.data, "r14b", 4) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R14; }
        if (memcmp(name.data, "r15b", 4) == 0) { if (out_byte_size) *out_byte_size = 1; return REG_R15; }

        if (memcmp(name.data, "r10w", 4) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R10; }
        if (memcmp(name.data, "r11w", 4) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R11; }
        if (memcmp(name.data, "r12w", 4) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R12; }
        if (memcmp(name.data, "r13w", 4) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R13; }
        if (memcmp(name.data, "r14w", 4) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R14; }
        if (memcmp(name.data, "r15w", 4) == 0) { if (out_byte_size) *out_byte_size = 2; return REG_R15; }

        if (memcmp(name.data, "r10d", 4) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R10; }
        if (memcmp(name.data, "r11d", 4) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R11; }
        if (memcmp(name.data, "r12d", 4) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R12; }
        if (memcmp(name.data, "r13d", 4) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R13; }
        if (memcmp(name.data, "r14d", 4) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R14; }
        if (memcmp(name.data, "r15d", 4) == 0) { if (out_byte_size) *out_byte_size = 4; return REG_R15; }
    }

    return REG_NONE;
}

typedef struct BitSet {
    uint64_t* words;
    size_t    word_count;
    size_t    bit_count;
} BitSet;

static BitSet bitset_create(Arena* arena, size_t bit_count) {
    size_t word_count = (bit_count + 63) / 64;
    BitSet bs;
    bs.words      = (word_count > 0) ? ARENA_NEW_ARRAY_ZERO(arena, uint64_t, word_count) : NULL;
    bs.word_count = word_count;
    bs.bit_count  = bit_count;
    return bs;
}

static inline bool bitset_test(const BitSet* bs, size_t bit) {
    if (!bs->words || bit >= bs->bit_count) return false;
    return (bs->words[bit / 64] & (1ULL << (bit % 64))) != 0;
}

static inline void bitset_set(BitSet* bs, size_t bit) {
    if (bs->words && bit < bs->bit_count) {
        bs->words[bit / 64] |= (1ULL << (bit % 64));
    }
}

static inline void bitset_reset(BitSet* bs, size_t bit) {
    if (bs->words && bit < bs->bit_count) {
        bs->words[bit / 64] &= ~(1ULL << (bit % 64));
    }
}

static inline bool bitset_union(BitSet* dst, const BitSet* src) {
    bool changed = false;
    size_t count = (dst->word_count < src->word_count) ? dst->word_count : src->word_count;
    for (size_t i = 0; i < count; ++i) {
        uint64_t old_val = dst->words[i];
        dst->words[i] |= src->words[i];
        if (dst->words[i] != old_val) {
            changed = true;
        }
    }
    return changed;
}

static inline void bitset_copy(BitSet* dst, const BitSet* src) {
    size_t count = (dst->word_count < src->word_count) ? dst->word_count : src->word_count;
    memcpy(dst->words, src->words, count * sizeof(uint64_t));
}

static inline bool bitset_equals(const BitSet* a, const BitSet* b) {
    if (a->bit_count != b->bit_count) return false;
    return memcmp(a->words, b->words, a->word_count * sizeof(uint64_t)) == 0;
}

typedef enum MoveState {
    MOVE_WORKLIST,
    MOVE_ACTIVE,
    MOVE_FROZEN,
    MOVE_CONSTRAINED,
    MOVE_COALESCED
} MoveState;

typedef struct MoveEntry {
    uint32_t  dst_id;
    uint32_t  src_id;
    MoveState state;
} MoveEntry;

typedef enum NodeState {
    NODE_INITIAL,
    NODE_SIMPLIFY_WORKLIST,
    NODE_FREEZE_WORKLIST,
    NODE_SPILL_WORKLIST,
    NODE_SELECT_STACK,
    NODE_COALESCED,
    NODE_COLORED,
    NODE_SPILLED
} NodeState;

typedef struct MoveList {
    uint32_t* items;
    size_t    count;
    size_t    cap;
} MoveList;

typedef struct IRCNode {
    uint32_t   id;
    uint32_t   alias;
    X86Reg     color;
    X86Reg     hint;
    size_t     degree;
    double     spill_cost;
    size_t     byte_size;
    bool       is_signed;
    bool       is_precolored;
    bool       crosses_call;
    bool       is_spilled;
    int32_t    spill_slot;

    NodeState  state;

    uint32_t*  adj_list;
    size_t     adj_count;
    size_t     adj_cap;

    MoveList   moves;
} IRCNode;

typedef struct IRCGraph {
    Arena*      arena;
    IRFunction* func;

    size_t      total_nodes;
    size_t      vreg_count;
    IRCNode*    nodes;

    BitSet*     adj_matrix;

    MoveEntry*  moves;
    size_t      move_count;
    size_t      move_cap;

    uint32_t*   worklist_moves;
    size_t      worklist_move_count;
    size_t      worklist_move_cap;

    uint32_t*   simplify_worklist;
    size_t      simplify_count;
    size_t      simplify_cap;

    uint32_t*   freeze_worklist;
    size_t      freeze_count;
    size_t      freeze_cap;

    uint32_t*   spill_worklist;
    size_t      spill_count;
    size_t      spill_cap;

    uint32_t*   select_stack;
    size_t      select_top;

    uint32_t*   spilled_nodes;
    size_t      spilled_count;

    uint32_t*   coalesced_nodes;
    size_t      coalesced_count;

    uint32_t*   block_loop_depth;
    size_t      block_count;

    size_t      total_spills_allocated;
} IRCGraph;

static inline uint32_t vreg_to_node_id(uint32_t vreg) {
    return (uint32_t)REG_COUNT + vreg;
}

static inline bool is_vreg_node(uint32_t node_id) {
    return node_id >= (uint32_t)REG_COUNT;
}

static inline uint32_t node_id_to_vreg(uint32_t node_id) {
    assert(is_vreg_node(node_id));
    return node_id - (uint32_t)REG_COUNT;
}

static uint32_t get_operand_node_id(const IROperand* op) {
    if (!op) return 0;
    if (op->kind == IR_OP_VREG) {
        return vreg_to_node_id(op->vreg_id);
    }
    if (op->kind == IR_OP_REG && op->reg != REG_NONE && op->reg < REG_COUNT) {
        return (uint32_t)op->reg;
    }
    return 0;
}

static uint32_t irc_get_alias(IRCGraph* g, uint32_t node_id) {
    uint32_t root = node_id;
    size_t depth = 0;
    while (g->nodes[root].alias != root && depth < 64) {
        root = g->nodes[root].alias;
        depth++;
    }

    uint32_t curr = node_id;
    while (curr != root) {
        uint32_t next = g->nodes[curr].alias;
        g->nodes[curr].alias = root;
        curr = next;
    }

    return root;
}

static void compute_loop_depths(IRCGraph* g) {
    IRFunction* func = g->func;
    size_t count = func->block_count + func->next_block_id + 16;
    g->block_count = count;
    g->block_loop_depth = ARENA_NEW_ARRAY_ZERO(g->arena, uint32_t, count);

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        if (b->id >= count) continue;
        IRInst* term = b->last_inst;
        if (!term) continue;

        if (term->opcode == IR_JMP && term->dst.kind == IR_OP_BLOCK && term->dst.block) {
            if (term->dst.block->id <= b->id) {
                for (IRBlock* cur = term->dst.block; cur != NULL && cur != b->next_block; cur = cur->next_block) {
                    if (cur->id < count) g->block_loop_depth[cur->id]++;
                }
            }
        } else if (term->opcode == IR_BR) {
            IRBlock* t1 = (term->src1.kind == IR_OP_BLOCK) ? term->src1.block : NULL;
            IRBlock* t2 = (term->src2.kind == IR_OP_BLOCK) ? term->src2.block : NULL;
            if (t1 && t1->id <= b->id) {
                for (IRBlock* cur = t1; cur != NULL && cur != b->next_block; cur = cur->next_block) {
                    if (cur->id < count) g->block_loop_depth[cur->id]++;
                }
            }
            if (t2 && t2->id <= b->id && t2 != t1) {
                for (IRBlock* cur = t2; cur != NULL && cur != b->next_block; cur = cur->next_block) {
                    if (cur->id < count) g->block_loop_depth[cur->id]++;
                }
            }
        }
    }
}

typedef struct BlockLiveness {
    BitSet live_in;
    BitSet live_out;
    BitSet gen;
    BitSet kill;
} BlockLiveness;

static bool inst_dst_is_read_only(const IRInst* inst) {
    if (!inst) return false;
    return (inst->opcode == IR_STORE || inst->opcode == IR_MEMCPY ||
            inst->opcode == IR_BR    || inst->opcode == IR_RET);
}

static void compute_liveness(IRCGraph* g, BlockLiveness* block_live) {
    IRFunction* func = g->func;
    size_t total_nodes = g->total_nodes;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        if (b->id >= g->block_count) continue;
        BlockLiveness* bl = &block_live[b->id];

        bl->live_in  = bitset_create(g->arena, total_nodes);
        bl->live_out = bitset_create(g->arena, total_nodes);
        bl->gen      = bitset_create(g->arena, total_nodes);
        bl->kill     = bitset_create(g->arena, total_nodes);

        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP) continue;

            uint32_t uses[16];
            size_t use_count = 0;

            uint32_t u1 = get_operand_node_id(&inst->src1);
            if (u1) uses[use_count++] = u1;

            uint32_t u2 = get_operand_node_id(&inst->src2);
            if (u2) uses[use_count++] = u2;

            if (inst_dst_is_read_only(inst)) {
                uint32_t ud = get_operand_node_id(&inst->dst);
                if (ud) uses[use_count++] = ud;
            }

            for (size_t a = 0; a < inst->extra_arg_count && use_count < 16; ++a) {
                uint32_t ua = get_operand_node_id(&inst->extra_args[a]);
                if (ua) uses[use_count++] = ua;
            }

            for (size_t a = 0; a < inst->asm_input_count && use_count < 16; ++a) {
                uint32_t ua = get_operand_node_id(&inst->asm_inputs[a].val);
                if (ua) uses[use_count++] = ua;
            }

            for (size_t i = 0; i < use_count; ++i) {
                uint32_t nid = uses[i];
                if (!bitset_test(&bl->kill, nid)) {
                    bitset_set(&bl->gen, nid);
                }
            }

            if (!inst_dst_is_read_only(inst)) {
                uint32_t def_id = get_operand_node_id(&inst->dst);
                if (def_id) {
                    bitset_set(&bl->kill, def_id);
                }
            }
        }
    }

    bool changed = true;
    size_t iter = 0;
    while (changed && iter < 100) {
        changed = false;
        iter++;

        for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
            if (b->id >= g->block_count) continue;
            BlockLiveness* bl = &block_live[b->id];

            BitSet new_live_out = bitset_create(g->arena, total_nodes);
            IRInst* term = b->last_inst;

            if (term) {
                if (term->opcode == IR_JMP && term->dst.kind == IR_OP_BLOCK && term->dst.block) {
                    if (term->dst.block->id < g->block_count) {
                        bitset_union(&new_live_out, &block_live[term->dst.block->id].live_in);
                    }
                } else if (term->opcode == IR_BR) {
                    if (term->src1.kind == IR_OP_BLOCK && term->src1.block && term->src1.block->id < g->block_count) {
                        bitset_union(&new_live_out, &block_live[term->src1.block->id].live_in);
                    }
                    if (term->src2.kind == IR_OP_BLOCK && term->src2.block && term->src2.block->id < g->block_count) {
                        bitset_union(&new_live_out, &block_live[term->src2.block->id].live_in);
                    }
                }
            } else if (b->next_block && b->next_block->id < g->block_count) {
                bitset_union(&new_live_out, &block_live[b->next_block->id].live_in);
            }

            if (bitset_union(&bl->live_out, &new_live_out)) {
                changed = true;
            }

            BitSet new_live_in = bitset_create(g->arena, total_nodes);
            bitset_copy(&new_live_in, &bl->live_out);

            for (size_t w = 0; w < bl->kill.word_count; ++w) {
                new_live_in.words[w] &= ~bl->kill.words[w];
            }
            bitset_union(&new_live_in, &bl->gen);

            if (!bitset_equals(&bl->live_in, &new_live_in)) {
                bitset_copy(&bl->live_in, &new_live_in);
                changed = true;
            }
        }
    }
}

static inline bool irc_is_adjacent(const IRCGraph* g, uint32_t u, uint32_t v) {
    if (u == v) return false;
    return bitset_test(&g->adj_matrix[u], v);
}

static void irc_add_edge(IRCGraph* g, uint32_t u, uint32_t v) {
    if (u == v || irc_is_adjacent(g, u, v)) {
        return;
    }

    bitset_set(&g->adj_matrix[u], v);
    bitset_set(&g->adj_matrix[v], u);

    if (!g->nodes[u].is_precolored) {
        ARENA_DA_PUSH(g->arena, g->nodes[u].adj_list, g->nodes[u].adj_count, g->nodes[u].adj_cap, v);
        g->nodes[u].degree++;
    }

    if (!g->nodes[v].is_precolored) {
        ARENA_DA_PUSH(g->arena, g->nodes[v].adj_list, g->nodes[v].adj_count, g->nodes[v].adj_cap, u);
        g->nodes[v].degree++;
    }
}

static void irc_add_move(IRCGraph* g, uint32_t dst_id, uint32_t src_id) {
    if (dst_id == src_id || dst_id == 0 || src_id == 0) return;

    uint32_t m_idx = (uint32_t)g->move_count;

    MoveEntry m;
    m.dst_id = dst_id;
    m.src_id = src_id;
    m.state  = MOVE_WORKLIST;

    ARENA_DA_PUSH(g->arena, g->moves, g->move_count, g->move_cap, m);

    ARENA_DA_PUSH(g->arena, g->nodes[dst_id].moves.items, g->nodes[dst_id].moves.count, g->nodes[dst_id].moves.cap, m_idx);
    ARENA_DA_PUSH(g->arena, g->nodes[src_id].moves.items, g->nodes[src_id].moves.count, g->nodes[src_id].moves.cap, m_idx);

    ARENA_DA_PUSH(g->arena, g->worklist_moves, g->worklist_move_count, g->worklist_move_cap, m_idx);
}

static void irc_build_graph(IRCGraph* g, BlockLiveness* block_live) {
    IRFunction* func = g->func;
    size_t total_nodes = g->total_nodes;

    for (size_t i = 1; i < REG_COUNT; ++i) {
        for (size_t j = i + 1; j < REG_COUNT; ++j) {
            bitset_set(&g->adj_matrix[i], j);
            bitset_set(&g->adj_matrix[j], i);
        }
    }

    BitSet live = bitset_create(g->arena, total_nodes);

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        if (b->id >= g->block_count) continue;
        bitset_copy(&live, &block_live[b->id].live_out);

        uint32_t loop_depth = g->block_loop_depth[b->id];
        double weight = 1.0;
        for (uint32_t d = 0; d < loop_depth && d < 6; ++d) {
            weight *= 10.0;
        }

        size_t icount = b->inst_count;
        IRInst** inst_arr = ARENA_NEW_ARRAY(g->arena, IRInst*, icount + 16);
        size_t actual_count = 0;
        for (IRInst* cur = b->first_inst; cur != NULL; cur = cur->next) {
            inst_arr[actual_count++] = cur;
        }

        for (int idx = (int)actual_count - 1; idx >= 0; --idx) {
            IRInst* inst = inst_arr[idx];
            if (inst->opcode == IR_NOP) continue;

            bool is_call = (inst->opcode == IR_CALL || inst->opcode == IR_CALL_PTR ||
                            inst->opcode == IR_TAIL_CALL || inst->opcode == IR_TAIL_CALL_PTR);

            if (is_call) {
                for (size_t n = 1; n < total_nodes; ++n) {
                    if (bitset_test(&live, n)) {
                        g->nodes[n].crosses_call = true;
                        for (size_t c = 0; c < ALL_CALLER_SAVED_COUNT; ++c) {
                            irc_add_edge(g, (uint32_t)n, (uint32_t)ALL_CALLER_SAVED_REGS[c]);
                        }
                    }
                }
            }

            if (inst->opcode == IR_INLINE_ASM) {
                uint32_t asm_clobbers = inst->clobber_mask;

                for (size_t a = 0; a < inst->asm_input_count; ++a) {
                    if (inst->asm_inputs[a].reg != REG_NONE) {
                        asm_clobbers |= (1 << inst->asm_inputs[a].reg);
                    }
                }
                for (size_t a = 0; a < inst->asm_output_count; ++a) {
                    if (inst->asm_outputs[a].reg != REG_NONE) {
                        asm_clobbers |= (1 << inst->asm_outputs[a].reg);
                    }
                }

                if (asm_clobbers != 0) {
                    for (size_t r = 1; r < REG_COUNT; ++r) {
                        if (asm_clobbers & (1 << r)) {
                            for (size_t n = 1; n < total_nodes; ++n) {
                                if (bitset_test(&live, n)) {
                                    irc_add_edge(g, (uint32_t)n, (uint32_t)r);
                                }
                            }
                        }
                    }
                }
            }

            uint32_t def_id = 0;
            if (!inst_dst_is_read_only(inst)) {
                def_id = get_operand_node_id(&inst->dst);
            }

            bool is_mov = (inst->opcode == IR_MOV && inst->dst.kind == IR_OP_VREG &&
                           (inst->src1.kind == IR_OP_VREG || inst->src1.kind == IR_OP_REG));

            if (is_mov) {
                uint32_t src_id = get_operand_node_id(&inst->src1);
                if (def_id && src_id && def_id != src_id) {
                    irc_add_move(g, def_id, src_id);
                    bitset_reset(&live, src_id);
                }
            }

            X86Reg param_phys_r = REG_NONE;
            if (inst->opcode == IR_PARAM && inst->dst.kind == IR_OP_VREG) {
                size_t p_idx = (size_t)inst->src1.int_val;
                if (p_idx == 0)      param_phys_r = REG_RDI;
                else if (p_idx == 1) param_phys_r = REG_RSI;
                else if (p_idx == 2) param_phys_r = REG_RDX;
                else if (p_idx == 3) param_phys_r = REG_RCX;
                else if (p_idx == 4) param_phys_r = REG_R8;
                else if (p_idx == 5) param_phys_r = REG_R9;

                if (param_phys_r != REG_NONE) {
                    g->nodes[def_id].hint = param_phys_r;
                    irc_add_move(g, def_id, (uint32_t)param_phys_r);
                    bitset_reset(&live, (uint32_t)param_phys_r);
                }
            }

            if (def_id) {
                g->nodes[def_id].spill_cost += weight;
                for (size_t n = 1; n < total_nodes; ++n) {
                    if (bitset_test(&live, n)) {
                        irc_add_edge(g, def_id, (uint32_t)n);
                    }
                }
                bitset_reset(&live, def_id);
            }

            if (param_phys_r != REG_NONE) {
                bitset_set(&live, (uint32_t)param_phys_r);
            }

            uint32_t u1 = get_operand_node_id(&inst->src1);
            if (u1) {
                bitset_set(&live, u1);
                g->nodes[u1].spill_cost += weight;
            }

            uint32_t u2 = get_operand_node_id(&inst->src2);
            if (u2) {
                bitset_set(&live, u2);
                g->nodes[u2].spill_cost += weight;
            }

            if (inst_dst_is_read_only(inst)) {
                uint32_t ud = get_operand_node_id(&inst->dst);
                if (ud) {
                    bitset_set(&live, ud);
                    g->nodes[ud].spill_cost += weight;
                }
            }

            for (size_t a = 0; a < inst->extra_arg_count; ++a) {
                uint32_t ua = get_operand_node_id(&inst->extra_args[a]);
                if (ua) {
                    bitset_set(&live, ua);
                    g->nodes[ua].spill_cost += weight;
                }
            }

            for (size_t a = 0; a < inst->asm_input_count; ++a) {
                uint32_t ua = get_operand_node_id(&inst->asm_inputs[a].val);
                if (ua) {
                    bitset_set(&live, ua);
                    g->nodes[ua].spill_cost += weight;
                }
            }
        }
    }
}

static bool irc_is_move_related(IRCGraph* g, uint32_t node_id) {
    uint32_t n = irc_get_alias(g, node_id);
    for (size_t i = 0; i < g->nodes[n].moves.count; ++i) {
        uint32_t m_idx = g->nodes[n].moves.items[i];
        MoveState state = g->moves[m_idx].state;
        if (state == MOVE_WORKLIST || state == MOVE_ACTIVE) {
            return true;
        }
    }
    return false;
}

static void irc_make_worklists(IRCGraph* g) {
    g->simplify_count = 0;
    g->freeze_count   = 0;
    g->spill_count    = 0;

    for (size_t i = REG_COUNT; i < g->total_nodes; ++i) {
        IRCNode* n = &g->nodes[i];
        if (n->is_spilled || n->is_precolored) continue;

        if (n->degree >= K_REG_COUNT) {
            n->state = NODE_SPILL_WORKLIST;
            ARENA_DA_PUSH(g->arena, g->spill_worklist, g->spill_count, g->spill_cap, (uint32_t)i);
        } else if (irc_is_move_related(g, (uint32_t)i)) {
            n->state = NODE_FREEZE_WORKLIST;
            ARENA_DA_PUSH(g->arena, g->freeze_worklist, g->freeze_count, g->freeze_cap, (uint32_t)i);
        } else {
            n->state = NODE_SIMPLIFY_WORKLIST;
            ARENA_DA_PUSH(g->arena, g->simplify_worklist, g->simplify_count, g->simplify_cap, (uint32_t)i);
        }
    }
}

static void irc_enable_moves_for_node(IRCGraph* g, uint32_t node_id) {
    uint32_t n = irc_get_alias(g, node_id);
    for (size_t i = 0; i < g->nodes[n].moves.count; ++i) {
        uint32_t m_idx = g->nodes[n].moves.items[i];
        if (g->moves[m_idx].state == MOVE_ACTIVE) {
            g->moves[m_idx].state = MOVE_WORKLIST;
            ARENA_DA_PUSH(g->arena, g->worklist_moves, g->worklist_move_count, g->worklist_move_cap, m_idx);
        }
    }
}

static void irc_decrement_degree(IRCGraph* g, uint32_t node_id) {
    uint32_t n = irc_get_alias(g, node_id);
    if (g->nodes[n].is_precolored) return;

    size_t d = g->nodes[n].degree--;
    if (d == K_REG_COUNT) {
        irc_enable_moves_for_node(g, n);
        for (size_t i = 0; i < g->nodes[n].adj_count; ++i) {
            uint32_t adj = irc_get_alias(g, g->nodes[n].adj_list[i]);
            if (g->nodes[adj].state != NODE_SELECT_STACK) {
                irc_enable_moves_for_node(g, adj);
            }
        }

        if (g->nodes[n].state == NODE_SPILL_WORKLIST) {
            if (irc_is_move_related(g, n)) {
                g->nodes[n].state = NODE_FREEZE_WORKLIST;
                ARENA_DA_PUSH(g->arena, g->freeze_worklist, g->freeze_count, g->freeze_cap, n);
            } else {
                g->nodes[n].state = NODE_SIMPLIFY_WORKLIST;
                ARENA_DA_PUSH(g->arena, g->simplify_worklist, g->simplify_count, g->simplify_cap, n);
            }
        }
    }
}

static void irc_simplify(IRCGraph* g) {
    while (g->simplify_count > 0) {
        uint32_t node_id = g->simplify_worklist[--g->simplify_count];
        if (g->nodes[node_id].state != NODE_SIMPLIFY_WORKLIST) {
            continue;
        }

        g->nodes[node_id].state = NODE_SELECT_STACK;
        g->select_stack[g->select_top++] = node_id;

        for (size_t i = 0; i < g->nodes[node_id].adj_count; ++i) {
            uint32_t adj = irc_get_alias(g, g->nodes[node_id].adj_list[i]);
            if (g->nodes[adj].state != NODE_SELECT_STACK) {
                irc_decrement_degree(g, adj);
            }
        }
        return;
    }
}

static bool irc_briggs_check(IRCGraph* g, uint32_t u, uint32_t v) {
    size_t high_degree_count = 0;

    for (size_t i = 0; i < g->nodes[u].adj_count; ++i) {
        uint32_t adj = irc_get_alias(g, g->nodes[u].adj_list[i]);
        if (g->nodes[adj].state == NODE_SELECT_STACK || g->nodes[adj].state == NODE_COALESCED) continue;
        if (g->nodes[adj].degree >= K_REG_COUNT) {
            high_degree_count++;
        }
    }

    for (size_t i = 0; i < g->nodes[v].adj_count; ++i) {
        uint32_t adj = irc_get_alias(g, g->nodes[v].adj_list[i]);
        if (g->nodes[adj].state == NODE_SELECT_STACK || g->nodes[adj].state == NODE_COALESCED) continue;

        if (!irc_is_adjacent(g, u, adj)) {
            if (g->nodes[adj].degree >= K_REG_COUNT) {
                high_degree_count++;
            }
        }
    }

    return high_degree_count < K_REG_COUNT;
}

static bool irc_george_check(IRCGraph* g, uint32_t precolored, uint32_t vreg) {
    for (size_t i = 0; i < g->nodes[vreg].adj_count; ++i) {
        uint32_t t = irc_get_alias(g, g->nodes[vreg].adj_list[i]);
        if (g->nodes[t].state == NODE_SELECT_STACK || g->nodes[t].state == NODE_COALESCED) continue;

        if (g->nodes[t].degree < K_REG_COUNT || irc_is_adjacent(g, t, precolored) || g->nodes[t].is_precolored) {
            continue;
        }
        return false;
    }
    return true;
}

static void irc_combine_nodes(IRCGraph* g, uint32_t u, uint32_t v) {
    g->nodes[v].alias = u;
    g->nodes[v].state = NODE_COALESCED;

    for (size_t i = 0; i < g->nodes[v].moves.count; ++i) {
        ARENA_DA_PUSH(g->arena, g->nodes[u].moves.items, g->nodes[u].moves.count, g->nodes[u].moves.cap, g->nodes[v].moves.items[i]);
    }

    irc_enable_moves_for_node(g, v);

    for (size_t i = 0; i < g->nodes[v].adj_count; ++i) {
        uint32_t t = irc_get_alias(g, g->nodes[v].adj_list[i]);
        if (g->nodes[t].state != NODE_SELECT_STACK && g->nodes[t].state != NODE_COALESCED) {
            irc_add_edge(g, t, u);
            irc_decrement_degree(g, t);
        }
    }

    if (g->nodes[u].degree >= K_REG_COUNT && g->nodes[u].state == NODE_FREEZE_WORKLIST) {
        g->nodes[u].state = NODE_SPILL_WORKLIST;
        ARENA_DA_PUSH(g->arena, g->spill_worklist, g->spill_count, g->spill_cap, u);
    }

    g->coalesced_nodes[g->coalesced_count++] = v;
}

static void irc_coalesce(IRCGraph* g) {
    while (g->worklist_move_count > 0) {
        uint32_t m_idx = g->worklist_moves[--g->worklist_move_count];
        MoveEntry* m   = &g->moves[m_idx];

        if (m->state != MOVE_WORKLIST) {
            continue;
        }

        uint32_t x = irc_get_alias(g, m->dst_id);
        uint32_t y = irc_get_alias(g, m->src_id);

        uint32_t u = x;
        uint32_t v = y;

        if (g->nodes[v].is_precolored) {
            u = y;
            v = x;
        }

        if (u == v) {
            m->state = MOVE_COALESCED;
            irc_enable_moves_for_node(g, u);
            if (!g->nodes[u].is_precolored && !irc_is_move_related(g, u) && g->nodes[u].degree < K_REG_COUNT) {
                if (g->nodes[u].state == NODE_FREEZE_WORKLIST) {
                    g->nodes[u].state = NODE_SIMPLIFY_WORKLIST;
                    ARENA_DA_PUSH(g->arena, g->simplify_worklist, g->simplify_count, g->simplify_cap, u);
                }
            }
            return;
        }

        if (g->nodes[v].is_precolored || irc_is_adjacent(g, u, v)) {
            m->state = MOVE_CONSTRAINED;
            irc_enable_moves_for_node(g, u);
            irc_enable_moves_for_node(g, v);
            if (!g->nodes[u].is_precolored && !irc_is_move_related(g, u) && g->nodes[u].degree < K_REG_COUNT) {
                if (g->nodes[u].state == NODE_FREEZE_WORKLIST) {
                    g->nodes[u].state = NODE_SIMPLIFY_WORKLIST;
                    ARENA_DA_PUSH(g->arena, g->simplify_worklist, g->simplify_count, g->simplify_cap, u);
                }
            }
            if (!g->nodes[v].is_precolored && !irc_is_move_related(g, v) && g->nodes[v].degree < K_REG_COUNT) {
                if (g->nodes[v].state == NODE_FREEZE_WORKLIST) {
                    g->nodes[v].state = NODE_SIMPLIFY_WORKLIST;
                    ARENA_DA_PUSH(g->arena, g->simplify_worklist, g->simplify_count, g->simplify_cap, v);
                }
            }
            return;
        }

        bool can_coalesce = false;
        if (g->nodes[u].is_precolored) {
            can_coalesce = irc_george_check(g, u, v);
        } else {
            can_coalesce = irc_briggs_check(g, u, v);
        }

        if (can_coalesce) {
            m->state = MOVE_COALESCED;
            irc_combine_nodes(g, u, v);
            if (!g->nodes[u].is_precolored && g->nodes[u].degree < K_REG_COUNT && !irc_is_move_related(g, u)) {
                if (g->nodes[u].state == NODE_FREEZE_WORKLIST) {
                    g->nodes[u].state = NODE_SIMPLIFY_WORKLIST;
                    ARENA_DA_PUSH(g->arena, g->simplify_worklist, g->simplify_count, g->simplify_cap, u);
                }
            }
        } else {
            m->state = MOVE_ACTIVE;
        }
        return;
    }
}

static void irc_freeze(IRCGraph* g) {
    while (g->freeze_count > 0) {
        uint32_t u = g->freeze_worklist[--g->freeze_count];
        if (g->nodes[u].state != NODE_FREEZE_WORKLIST) {
            continue;
        }

        g->nodes[u].state = NODE_SIMPLIFY_WORKLIST;
        ARENA_DA_PUSH(g->arena, g->simplify_worklist, g->simplify_count, g->simplify_cap, u);

        for (size_t i = 0; i < g->nodes[u].moves.count; ++i) {
            uint32_t m_idx = g->nodes[u].moves.items[i];
            MoveEntry* m   = &g->moves[m_idx];

            if (m->state == MOVE_ACTIVE || m->state == MOVE_WORKLIST) {
                m->state = MOVE_FROZEN;
                uint32_t x = irc_get_alias(g, m->dst_id);
                uint32_t y = irc_get_alias(g, m->src_id);
                uint32_t v = (x == u) ? y : x;

                if (!g->nodes[v].is_precolored && g->nodes[v].degree < K_REG_COUNT && !irc_is_move_related(g, v)) {
                    if (g->nodes[v].state == NODE_FREEZE_WORKLIST) {
                        g->nodes[v].state = NODE_SIMPLIFY_WORKLIST;
                        ARENA_DA_PUSH(g->arena, g->simplify_worklist, g->simplify_count, g->simplify_cap, v);
                    }
                }
            }
        }
        return;
    }
}

static void irc_select_spill(IRCGraph* g) {
    size_t best_idx = (size_t)-1;
    double min_metric = 1e30;

    for (size_t i = 0; i < g->spill_count; ++i) {
        uint32_t nid = g->spill_worklist[i];
        if (g->nodes[nid].state != NODE_SPILL_WORKLIST) continue;

        if (g->nodes[nid].spill_cost >= 1e18) continue;

        double metric = (g->nodes[nid].spill_cost + 0.01) / (double)g->nodes[nid].degree;
        if (metric < min_metric) {
            min_metric = metric;
            best_idx   = i;
        }
    }

    if (best_idx == (size_t)-1) {
        for (size_t i = 0; i < g->spill_count; ++i) {
            uint32_t nid = g->spill_worklist[i];
            if (g->nodes[nid].state == NODE_SPILL_WORKLIST) {
                best_idx = i;
                break;
            }
        }
        if (best_idx == (size_t)-1) {
            g->spill_count = 0;
            return;
        }
    }

    uint32_t chosen = g->spill_worklist[best_idx];
    g->spill_worklist[best_idx] = g->spill_worklist[--g->spill_count];

    g->nodes[chosen].state = NODE_SELECT_STACK;
    g->select_stack[g->select_top++] = chosen;

    for (size_t i = 0; i < g->nodes[chosen].adj_count; ++i) {
        uint32_t adj = irc_get_alias(g, g->nodes[chosen].adj_list[i]);
        if (g->nodes[adj].state != NODE_SELECT_STACK && g->nodes[adj].state != NODE_COALESCED) {
            irc_decrement_degree(g, adj);
        }
    }
}

static void irc_assign_colors(IRCGraph* g) {
    g->spilled_count = 0;

    while (g->select_top > 0) {
        uint32_t n = g->select_stack[--g->select_top];
        if (g->nodes[n].is_precolored) continue;

        bool ok_colors[REG_COUNT];
        memset(ok_colors, true, sizeof(ok_colors));

        for (size_t i = 0; i < g->nodes[n].adj_count; ++i) {
            uint32_t w = irc_get_alias(g, g->nodes[n].adj_list[i]);
            X86Reg c = g->nodes[w].color;
            if (c != REG_NONE && c < REG_COUNT) {
                ok_colors[c] = false;
            }
        }

        X86Reg chosen_color = REG_NONE;

        if (g->nodes[n].hint != REG_NONE && ok_colors[g->nodes[n].hint]) {
            chosen_color = g->nodes[n].hint;
        }

        if (chosen_color == REG_NONE && g->nodes[n].crosses_call) {
            for (size_t i = 0; i < CALLEE_SAVED_COUNT; ++i) {
                X86Reg r = CALLEE_SAVED_REGS[i];
                if (ok_colors[r]) {
                    chosen_color = r;
                    break;
                }
            }
        }

        if (chosen_color == REG_NONE && !g->nodes[n].crosses_call) {
            for (size_t i = 0; i < ALLOCATABLE_CALLER_SAVED_COUNT; ++i) {
                X86Reg r = ALLOCATABLE_CALLER_SAVED[i];
                if (ok_colors[r]) {
                    chosen_color = r;
                    break;
                }
            }
        }

        if (chosen_color == REG_NONE) {
            for (size_t i = 0; i < K_REG_COUNT; ++i) {
                X86Reg r = ALLOCATABLE_REGS[i];
                if (ok_colors[r]) {
                    chosen_color = r;
                    break;
                }
            }
        }

        if (chosen_color != REG_NONE) {
            g->nodes[n].color = chosen_color;
            g->nodes[n].state = NODE_COLORED;
        } else {
            g->nodes[n].state      = NODE_SPILLED;
            g->nodes[n].is_spilled = true;
            g->spilled_nodes[g->spilled_count++] = n;
        }
    }

    for (size_t i = 0; i < g->coalesced_count; ++i) {
        uint32_t n = g->coalesced_nodes[i];
        uint32_t alias = irc_get_alias(g, n);
        g->nodes[n].color = g->nodes[alias].color;
    }
}

static void insert_inst_before(IRBlock* block, IRInst* target, IRInst* new_inst) {
    assert(block != NULL && target != NULL && new_inst != NULL);
    if (block->first_inst == target) {
        new_inst->next    = block->first_inst;
        block->first_inst = new_inst;
    } else {
        IRInst* prev = block->first_inst;
        while (prev && prev->next != target) {
            prev = prev->next;
        }
        assert(prev != NULL && "Target instruction not found in block");
        new_inst->next = target;
        prev->next     = new_inst;
    }
    block->inst_count++;
}

static void insert_inst_after(IRBlock* block, IRInst* target, IRInst* new_inst) {
    assert(block != NULL && target != NULL && new_inst != NULL);
    new_inst->next = target->next;
    target->next   = new_inst;
    if (block->last_inst == target) {
        block->last_inst = new_inst;
    }
    block->inst_count++;
}

static void rewrite_spilled_program(IRCGraph* g) {
    IRFunction* func = g->func;

    for (size_t i = 0; i < g->spilled_count; ++i) {
        uint32_t node_id = g->spilled_nodes[i];
        assert(is_vreg_node(node_id));
        uint32_t spilled_vreg = node_id_to_vreg(node_id);

        size_t byte_size = g->nodes[node_id].byte_size ? g->nodes[node_id].byte_size : 8;
        size_t align     = (byte_size >= 8) ? 8 : (byte_size >= 4 ? 4 : (byte_size >= 2 ? 2 : 1));

        int32_t slot = ir_func_alloc_stack_slot(func, byte_size, align);
        g->nodes[node_id].spill_slot = slot;
        g->total_spills_allocated++;

        for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
            IRInst* inst = b->first_inst;

            while (inst != NULL) {
                IRInst* next_inst = inst->next;
                if (inst->opcode == IR_NOP) {
                    inst = next_inst;
                    continue;
                }

                bool used = false;
                if (inst->src1.kind == IR_OP_VREG && inst->src1.vreg_id == spilled_vreg) used = true;
                if (inst->src2.kind == IR_OP_VREG && inst->src2.vreg_id == spilled_vreg) used = true;
                if (inst_dst_is_read_only(inst) && inst->dst.kind == IR_OP_VREG && inst->dst.vreg_id == spilled_vreg) used = true;

                for (size_t a = 0; a < inst->extra_arg_count; ++a) {
                    if (inst->extra_args[a].kind == IR_OP_VREG && inst->extra_args[a].vreg_id == spilled_vreg) used = true;
                }
                for (size_t a = 0; a < inst->asm_input_count; ++a) {
                    if (inst->asm_inputs[a].val.kind == IR_OP_VREG && inst->asm_inputs[a].val.vreg_id == spilled_vreg) used = true;
                }

                if (used) {
                    uint32_t reload_vreg = ir_vreg_alloc(func);
                    IRInst* load_inst = ARENA_NEW_ZERO(func->arena, IRInst);
                    load_inst->opcode = IR_MOV;
                    load_inst->dst    = ir_op_vreg(reload_vreg, byte_size, g->nodes[node_id].is_signed);
                    load_inst->src1   = ir_op_stack(slot, byte_size, g->nodes[node_id].is_signed);
                    load_inst->loc    = inst->loc;

                    insert_inst_before(b, inst, load_inst);

                    if (inst->src1.kind == IR_OP_VREG && inst->src1.vreg_id == spilled_vreg) {
                        inst->src1.vreg_id = reload_vreg;
                    }
                    if (inst->src2.kind == IR_OP_VREG && inst->src2.vreg_id == spilled_vreg) {
                        inst->src2.vreg_id = reload_vreg;
                    }
                    if (inst_dst_is_read_only(inst) && inst->dst.kind == IR_OP_VREG && inst->dst.vreg_id == spilled_vreg) {
                        inst->dst.vreg_id = reload_vreg;
                    }
                    for (size_t a = 0; a < inst->extra_arg_count; ++a) {
                        if (inst->extra_args[a].kind == IR_OP_VREG && inst->extra_args[a].vreg_id == spilled_vreg) {
                            inst->extra_args[a].vreg_id = reload_vreg;
                        }
                    }
                    for (size_t a = 0; a < inst->asm_input_count; ++a) {
                        if (inst->asm_inputs[a].val.kind == IR_OP_VREG && inst->asm_inputs[a].val.vreg_id == spilled_vreg) {
                            inst->asm_inputs[a].val.vreg_id = reload_vreg;
                        }
                    }
                }

                if (!inst_dst_is_read_only(inst) && inst->dst.kind == IR_OP_VREG && inst->dst.vreg_id == spilled_vreg) {
                    uint32_t def_temp = ir_vreg_alloc(func);
                    inst->dst.vreg_id = def_temp;

                    IRInst* store_inst = ARENA_NEW_ZERO(func->arena, IRInst);
                    store_inst->opcode = IR_MOV;
                    store_inst->dst    = ir_op_stack(slot, byte_size, g->nodes[node_id].is_signed);
                    store_inst->src1   = ir_op_vreg(def_temp, byte_size, g->nodes[node_id].is_signed);
                    store_inst->loc    = inst->loc;

                    insert_inst_after(b, inst, store_inst);
                    inst = store_inst;
                }

                inst = next_inst;
            }
        }
    }
}

static void rewrite_operand_to_color(IROperand* op, IRCGraph* g) {
    if (!op || op->kind != IR_OP_VREG) {
        return;
    }

    uint32_t node_id = vreg_to_node_id(op->vreg_id);
    if (node_id >= g->total_nodes) {
        return;
    }

    uint32_t alias = irc_get_alias(g, node_id);
    X86Reg c = g->nodes[alias].color;

    if (c != REG_NONE) {
        *op = ir_op_reg(c, op->byte_size, op->is_signed);
    } else if (g->nodes[alias].is_spilled) {
        *op = ir_op_stack(g->nodes[alias].spill_slot, op->byte_size, op->is_signed);
    }
}

static void rewrite_final_function_operands(IRCGraph* g) {
    IRFunction* func = g->func;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP) continue;

            rewrite_operand_to_color(&inst->src1, g);
            rewrite_operand_to_color(&inst->src2, g);
            rewrite_operand_to_color(&inst->dst, g);

            for (size_t a = 0; a < inst->extra_arg_count; ++a) {
                rewrite_operand_to_color(&inst->extra_args[a], g);
            }
            for (size_t a = 0; a < inst->asm_input_count; ++a) {
                rewrite_operand_to_color(&inst->asm_inputs[a].val, g);
            }
        }
    }
}

RegAllocResult regalloc_run_on_function(Arena* arena, IRFunction* func) {
    RegAllocResult result;
    result.callee_saved_mask = 0;
    result.spill_slot_count  = 0;

    if (!func || !func->first_block || func->next_vreg_id == 0) {
        return result;
    }

    size_t orig_vreg_count = func->next_vreg_id;
    size_t iteration = 0;
    IRCGraph g;

    while (iteration < 32) {
        iteration++;

        size_t vreg_count  = func->next_vreg_id;
        size_t total_nodes = (size_t)REG_COUNT + vreg_count;

        ArenaTemp scratch = arena_scratch_get(&arena, 1);

        g.arena       = scratch.arena;
        g.func        = func;
        g.vreg_count  = vreg_count;
        g.total_nodes = total_nodes;

        g.nodes = ARENA_NEW_ARRAY_ZERO(scratch.arena, IRCNode, total_nodes);
        g.adj_matrix = ARENA_NEW_ARRAY(scratch.arena, BitSet, total_nodes);

        for (size_t i = 0; i < total_nodes; ++i) {
            g.nodes[i].id            = (uint32_t)i;
            g.nodes[i].alias         = (uint32_t)i;
            g.nodes[i].color         = (i < REG_COUNT) ? (X86Reg)i : REG_NONE;
            g.nodes[i].hint          = REG_NONE;
            g.nodes[i].degree        = (i < REG_COUNT) ? 1000000 : 0;
            
            if (i >= REG_COUNT + orig_vreg_count) {
                g.nodes[i].spill_cost = 1e20;
            } else {
                g.nodes[i].spill_cost = 0.0;
            }

            g.nodes[i].is_precolored = (i < REG_COUNT);
            g.nodes[i].is_spilled    = false;
            g.nodes[i].crosses_call  = false;
            g.nodes[i].spill_slot    = 0;
            g.nodes[i].state         = NODE_INITIAL;

            g.nodes[i].adj_list  = NULL;
            g.nodes[i].adj_count = 0;
            g.nodes[i].adj_cap   = 0;

            g.nodes[i].moves.items = NULL;
            g.nodes[i].moves.count = 0;
            g.nodes[i].moves.cap   = 0;

            g.adj_matrix[i] = bitset_create(scratch.arena, total_nodes);
        }

        for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
            for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
                if (inst->dst.kind == IR_OP_VREG) {
                    uint32_t nid = vreg_to_node_id(inst->dst.vreg_id);
                    if (nid < total_nodes) {
                        g.nodes[nid].byte_size = inst->dst.byte_size;
                        g.nodes[nid].is_signed = inst->dst.is_signed;
                    }
                }
                if (inst->src1.kind == IR_OP_VREG) {
                    uint32_t nid = vreg_to_node_id(inst->src1.vreg_id);
                    if (nid < total_nodes && g.nodes[nid].byte_size == 0) {
                        g.nodes[nid].byte_size = inst->src1.byte_size;
                        g.nodes[nid].is_signed = inst->src1.is_signed;
                    }
                }
            }
        }

        g.simplify_worklist = NULL;
        g.simplify_count    = 0;
        g.simplify_cap      = 0;

        g.freeze_worklist   = NULL;
        g.freeze_count      = 0;
        g.freeze_cap        = 0;

        g.spill_worklist    = NULL;
        g.spill_count       = 0;
        g.spill_cap         = 0;

        g.select_stack      = ARENA_NEW_ARRAY_ZERO(scratch.arena, uint32_t, total_nodes);
        g.select_top        = 0;

        g.spilled_nodes     = ARENA_NEW_ARRAY_ZERO(scratch.arena, uint32_t, total_nodes);
        g.spilled_count     = 0;

        g.coalesced_nodes   = ARENA_NEW_ARRAY_ZERO(scratch.arena, uint32_t, total_nodes);
        g.coalesced_count   = 0;

        g.moves               = NULL;
        g.move_count          = 0;
        g.move_cap            = 0;

        g.worklist_moves      = NULL;
        g.worklist_move_count = 0;
        g.worklist_move_cap   = 0;

        g.total_spills_allocated = 0;

        compute_loop_depths(&g);
        BlockLiveness* block_live = ARENA_NEW_ARRAY_ZERO(scratch.arena, BlockLiveness, g.block_count);
        compute_liveness(&g, block_live);

        irc_build_graph(&g, block_live);
        irc_make_worklists(&g);

        do {
            if (g.simplify_count > 0) {
                irc_simplify(&g);
            } else if (g.worklist_move_count > 0) {
                irc_coalesce(&g);
            } else if (g.freeze_count > 0) {
                irc_freeze(&g);
            } else if (g.spill_count > 0) {
                irc_select_spill(&g);
            }
        } while (g.simplify_count > 0 || g.worklist_move_count > 0 ||
                 g.freeze_count > 0   || g.spill_count > 0);

        irc_assign_colors(&g);

        if (g.spilled_count == 0) {
            rewrite_final_function_operands(&g);

            for (size_t i = REG_COUNT; i < total_nodes; ++i) {
                X86Reg c = g.nodes[i].color;
                if (c != REG_NONE && reg_is_callee_saved(c)) {
                    result.callee_saved_mask |= (1 << c);
                }
            }

            arena_scratch_release(scratch);
            break;
        }

        rewrite_spilled_program(&g);
        result.spill_slot_count += g.spilled_count;

        arena_scratch_release(scratch);
    }

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_MOV && inst->dst.kind == IR_OP_REG && inst->src1.kind == IR_OP_REG) {
                if (inst->dst.reg == inst->src1.reg && inst->dst.byte_size == inst->src1.byte_size) {
                    inst->opcode = IR_NOP;
                }
            }
        }
    }

    size_t max_stack_depth = 0;
    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP) continue;

            if (inst->dst.kind == IR_OP_STACK && inst->dst.stack_offset < 0) {
                size_t d = (size_t)(-inst->dst.stack_offset);
                if (d > max_stack_depth) max_stack_depth = d;
            }
            if (inst->src1.kind == IR_OP_STACK && inst->src1.stack_offset < 0) {
                size_t d = (size_t)(-inst->src1.stack_offset);
                if (d > max_stack_depth) max_stack_depth = d;
            }
            if (inst->src2.kind == IR_OP_STACK && inst->src2.stack_offset < 0) {
                size_t d = (size_t)(-inst->src2.stack_offset);
                if (d > max_stack_depth) max_stack_depth = d;
            }
            for (size_t k = 0; k < inst->extra_arg_count; ++k) {
                if (inst->extra_args[k].kind == IR_OP_STACK && inst->extra_args[k].stack_offset < 0) {
                    size_t d = (size_t)(-inst->extra_args[k].stack_offset);
                    if (d > max_stack_depth) max_stack_depth = d;
                }
            }
            for (size_t k = 0; k < inst->asm_input_count; ++k) {
                if (inst->asm_inputs[k].val.kind == IR_OP_STACK && inst->asm_inputs[k].val.stack_offset < 0) {
                    size_t d = (size_t)(-inst->asm_inputs[k].val.stack_offset);
                    if (d > max_stack_depth) max_stack_depth = d;
                }
            }
        }
    }

    func->stack_frame_size  = max_stack_depth;
    func->callee_saved_mask = result.callee_saved_mask;

    ir_eliminate_nops(func);

    return result;
}

void regalloc_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        regalloc_run_on_function(arena, f);
    }
}