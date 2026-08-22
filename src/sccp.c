#include "sccp.h"

#include <string.h>
#include <assert.h>

typedef enum LatticeKind {
    LATTICE_TOP = 0,
    LATTICE_CONST,
    LATTICE_BOT
} LatticeKind;

typedef struct LatticeVal {
    LatticeKind kind;
    int64_t     val;
} LatticeVal;

typedef struct CFGEdge {
    IRBlock* pred;
    IRBlock* succ;
} CFGEdge;

typedef struct UseNode {
    IRInst*         inst;
    IRBlock*        block;
    struct UseNode* next;
} UseNode;

typedef struct SCCPContext {
    Arena*        arena;
    IRFunction*   func;

    LatticeVal*   lat_vals;
    size_t        vreg_cap;

    bool*         block_visited;
    bool*         edge_executable;
    size_t        block_stride;

    CFGEdge*      edge_worklist;
    size_t        ew_head;
    size_t        ew_tail;
    size_t        ew_cap;

    uint32_t*     ssa_worklist;
    size_t        sw_head;
    size_t        sw_tail;
    size_t        sw_cap;
    bool*         in_ssa_worklist;

    UseNode**     use_chains;
} SCCPContext;

static int64_t truncate_val(int64_t val, size_t byte_size, bool is_signed) {
    switch (byte_size) {
        case 1:
            return is_signed ? (int64_t)(int8_t)val : (int64_t)(uint8_t)val;
        case 2:
            return is_signed ? (int64_t)(int16_t)val : (int64_t)(uint16_t)val;
        case 4:
            return is_signed ? (int64_t)(int32_t)val : (int64_t)(uint32_t)val;
        case 8:
        default:
            return is_signed ? val : (int64_t)(uint64_t)val;
    }
}

static LatticeVal lattice_meet(LatticeVal a, LatticeVal b) {
    if (a.kind == LATTICE_TOP) {
        return b;
    }

    if (b.kind == LATTICE_TOP) {
        return a;
    }

    if (a.kind == LATTICE_BOT || b.kind == LATTICE_BOT) {
        return (LatticeVal){ .kind = LATTICE_BOT, .val = 0 };
    }

    if (a.kind == LATTICE_CONST && b.kind == LATTICE_CONST) {
        if (a.val == b.val) {
            return a;
        }

        return (LatticeVal){ .kind = LATTICE_BOT, .val = 0 };
    }

    return (LatticeVal){ .kind = LATTICE_BOT, .val = 0 };
}

static LatticeVal get_operand_lattice(const SCCPContext* ctx, IROperand op) {
    if (op.kind == IR_OP_CONST) {
        return (LatticeVal){
            .kind = LATTICE_CONST,
            .val  = truncate_val(op.int_val, op.byte_size, op.is_signed)
        };
    }

    if (op.kind == IR_OP_VREG) {
        if (op.vreg_id < ctx->vreg_cap) {
            return ctx->lat_vals[op.vreg_id];
        }

        return (LatticeVal){ .kind = LATTICE_BOT, .val = 0 };
    }

    return (LatticeVal){ .kind = LATTICE_BOT, .val = 0 };
}

static bool is_edge_executable(const SCCPContext* ctx, const IRBlock* pred, const IRBlock* succ) {
    if (!pred || !succ) {
        return false;
    }

    size_t idx = (size_t)pred->id * ctx->block_stride + (size_t)succ->id;

    return ctx->edge_executable[idx];
}

static void mark_edge_executable(SCCPContext* ctx, IRBlock* pred, IRBlock* succ) {
    if (!pred || !succ) {
        return;
    }

    size_t idx = (size_t)pred->id * ctx->block_stride + (size_t)succ->id;

    if (!ctx->edge_executable[idx]) {
        ctx->edge_executable[idx] = true;

        CFGEdge edge = { .pred = pred, .succ = succ };
        ARENA_DA_PUSH(ctx->arena, ctx->edge_worklist, ctx->ew_tail, ctx->ew_cap, edge);
    }
}

static void push_ssa_worklist(SCCPContext* ctx, uint32_t vreg_id) {
    if (vreg_id < ctx->vreg_cap && !ctx->in_ssa_worklist[vreg_id]) {
        ctx->in_ssa_worklist[vreg_id] = true;

        ARENA_DA_PUSH(ctx->arena, ctx->ssa_worklist, ctx->sw_tail, ctx->sw_cap, vreg_id);
    }
}

static void update_lattice_value(SCCPContext* ctx, uint32_t vreg_id, LatticeVal new_val) {
    if (vreg_id >= ctx->vreg_cap) {
        return;
    }

    LatticeVal old_val = ctx->lat_vals[vreg_id];

    if (old_val.kind == new_val.kind && (old_val.kind != LATTICE_CONST || old_val.val == new_val.val)) {
        return;
    }

    ctx->lat_vals[vreg_id] = new_val;

    push_ssa_worklist(ctx, vreg_id);
}

static bool try_eval_binary(IROpcode op, int64_t a, int64_t b, size_t size, bool is_signed, int64_t* out_val) {
    switch (op) {
        case IR_ADD:
            *out_val = a + b;
            return true;

        case IR_SUB:
            *out_val = a - b;
            return true;

        case IR_MUL:
            *out_val = a * b;
            return true;

        case IR_DIV:
            if (b == 0) return false;
            if (is_signed) {
                if (a == (-9223372036854775807LL - 1) && b == -1) return false;
                *out_val = a / b;
            } else {
                *out_val = (int64_t)((uint64_t)a / (uint64_t)b);
            }
            return true;

        case IR_MOD:
            if (b == 0) return false;
            if (is_signed) {
                if (a == (-9223372036854775807LL - 1) && b == -1) return false;
                *out_val = a % b;
            } else {
                *out_val = (int64_t)((uint64_t)a % (uint64_t)b);
            }
            return true;

        case IR_AND:
            *out_val = a & b;
            return true;

        case IR_OR:
            *out_val = a | b;
            return true;

        case IR_XOR:
            *out_val = a ^ b;
            return true;

        case IR_SHL: {
            uint32_t shift = (uint32_t)(b & (size == 8 ? 63 : 31));
            *out_val = a << shift;
            return true;
        }

        case IR_SHR: {
            uint32_t shift = (uint32_t)(b & (size == 8 ? 63 : 31));
            if (is_signed) {
                *out_val = a >> shift;
            } else {
                *out_val = (int64_t)(((uint64_t)a) >> shift);
            }
            return true;
        }

        case IR_CMP_EQ:
            *out_val = (a == b) ? 1 : 0;
            return true;

        case IR_CMP_NE:
            *out_val = (a != b) ? 1 : 0;
            return true;

        case IR_CMP_LT:
            *out_val = is_signed ? (a < b ? 1 : 0) : ((uint64_t)a < (uint64_t)b ? 1 : 0);
            return true;

        case IR_CMP_LE:
            *out_val = is_signed ? (a <= b ? 1 : 0) : ((uint64_t)a <= (uint64_t)b ? 1 : 0);
            return true;

        case IR_CMP_GT:
            *out_val = is_signed ? (a > b ? 1 : 0) : ((uint64_t)a > (uint64_t)b ? 1 : 0);
            return true;

        case IR_CMP_GE:
            *out_val = is_signed ? (a >= b ? 1 : 0) : ((uint64_t)a >= (uint64_t)b ? 1 : 0);
            return true;

        default:
            return false;
    }
}

static LatticeVal evaluate_inst(SCCPContext* ctx, IRInst* inst, IRBlock* current_block) {
    switch (inst->opcode) {
        case IR_MOV: {
            LatticeVal v1 = get_operand_lattice(ctx, inst->src1);

            if (v1.kind == LATTICE_CONST) {
                return (LatticeVal){
                    .kind = LATTICE_CONST,
                    .val  = truncate_val(v1.val, inst->dst.byte_size, inst->dst.is_signed)
                };
            }

            return v1;
        }

        case IR_NEG: {
            LatticeVal v1 = get_operand_lattice(ctx, inst->src1);

            if (v1.kind == LATTICE_CONST) {
                return (LatticeVal){
                    .kind = LATTICE_CONST,
                    .val  = truncate_val(-v1.val, inst->dst.byte_size, inst->dst.is_signed)
                };
            }

            return v1;
        }

        case IR_NOT: {
            LatticeVal v1 = get_operand_lattice(ctx, inst->src1);

            if (v1.kind == LATTICE_CONST) {
                return (LatticeVal){
                    .kind = LATTICE_CONST,
                    .val  = truncate_val(~v1.val, inst->dst.byte_size, inst->dst.is_signed)
                };
            }

            return v1;
        }

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
        case IR_CMP_EQ:
        case IR_CMP_NE:
        case IR_CMP_LT:
        case IR_CMP_LE:
        case IR_CMP_GT:
        case IR_CMP_GE: {
            LatticeVal v1 = get_operand_lattice(ctx, inst->src1);
            LatticeVal v2 = get_operand_lattice(ctx, inst->src2);

            if (v1.kind == LATTICE_BOT || v2.kind == LATTICE_BOT) {
                return (LatticeVal){ .kind = LATTICE_BOT, .val = 0 };
            }

            if (v1.kind == LATTICE_TOP || v2.kind == LATTICE_TOP) {
                return (LatticeVal){ .kind = LATTICE_TOP, .val = 0 };
            }

            if (v1.kind == LATTICE_CONST && v2.kind == LATTICE_CONST) {
                int64_t res = 0;

                if (try_eval_binary(inst->opcode, v1.val, v2.val, inst->dst.byte_size, inst->src1.is_signed, &res)) {
                    return (LatticeVal){
                        .kind = LATTICE_CONST,
                        .val  = truncate_val(res, inst->dst.byte_size, inst->dst.is_signed)
                    };
                }
            }

            return (LatticeVal){ .kind = LATTICE_BOT, .val = 0 };
        }

        case IR_PHI: {
            LatticeVal res = { .kind = LATTICE_TOP, .val = 0 };

            for (size_t i = 0; i < inst->extra_arg_count; i += 2) {
                IROperand val  = inst->extra_args[i];
                IRBlock*  pred = inst->extra_args[i + 1].block;

                if (pred != NULL && is_edge_executable(ctx, pred, current_block)) {
                    LatticeVal v = get_operand_lattice(ctx, val);
                    res = lattice_meet(res, v);
                }
            }

            return res;
        }

        default:
            return (LatticeVal){ .kind = LATTICE_BOT, .val = 0 };
    }
}

static void evaluate_block(SCCPContext* ctx, IRBlock* b) {
    for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
        if (inst->opcode == IR_NOP) {
            continue;
        }

        if (inst->dst.kind == IR_OP_VREG) {
            LatticeVal val = evaluate_inst(ctx, inst, b);
            update_lattice_value(ctx, inst->dst.vreg_id, val);
        }

        if (inst->opcode == IR_BR) {
            LatticeVal cond_val = get_operand_lattice(ctx, inst->dst);

            if (cond_val.kind == LATTICE_CONST) {
                if (cond_val.val != 0) {
                    mark_edge_executable(ctx, b, inst->src1.block);
                } else {
                    mark_edge_executable(ctx, b, inst->src2.block);
                }
            } else if (cond_val.kind == LATTICE_BOT) {
                mark_edge_executable(ctx, b, inst->src1.block);
                mark_edge_executable(ctx, b, inst->src2.block);
            }
        }

        if (inst->opcode == IR_JMP) {
            mark_edge_executable(ctx, b, inst->dst.block);
        }
    }
}

static void register_vreg_use(SCCPContext* ctx, uint32_t vreg_id, IRInst* inst, IRBlock* block) {
    if (vreg_id >= ctx->vreg_cap) {
        return;
    }

    UseNode* node = ARENA_NEW(ctx->arena, UseNode);

    node->inst  = inst;
    node->block = block;
    node->next  = ctx->use_chains[vreg_id];

    ctx->use_chains[vreg_id] = node;
}

static void build_use_chains(SCCPContext* ctx) {
    IRFunction* func = ctx->func;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP) {
                continue;
            }

            if (inst->src1.kind == IR_OP_VREG) {
                register_vreg_use(ctx, inst->src1.vreg_id, inst, b);
            }

            if (inst->src2.kind == IR_OP_VREG) {
                register_vreg_use(ctx, inst->src2.vreg_id, inst, b);
            }

            if (inst->opcode == IR_BR || inst->opcode == IR_RET || inst->opcode == IR_STORE || inst->opcode == IR_MEMCPY) {
                if (inst->dst.kind == IR_OP_VREG) {
                    register_vreg_use(ctx, inst->dst.vreg_id, inst, b);
                }
            }

            if (inst->opcode == IR_PHI) {
                for (size_t i = 0; i < inst->extra_arg_count; i += 2) {
                    if (inst->extra_args[i].kind == IR_OP_VREG) {
                        register_vreg_use(ctx, inst->extra_args[i].vreg_id, inst, b);
                    }
                }
            }

            for (size_t i = 0; i < inst->extra_arg_count; ++i) {
                if (inst->opcode != IR_PHI && inst->extra_args[i].kind == IR_OP_VREG) {
                    register_vreg_use(ctx, inst->extra_args[i].vreg_id, inst, b);
                }
            }

            for (size_t i = 0; i < inst->asm_input_count; ++i) {
                if (inst->asm_inputs[i].val.kind == IR_OP_VREG) {
                    register_vreg_use(ctx, inst->asm_inputs[i].val.vreg_id, inst, b);
                }
            }
        }
    }
}

static void apply_sccp_results(SCCPContext* ctx) {
    IRFunction* func = ctx->func;

    for (IRBlock* b = func->first_block; b != NULL; b = b->next_block) {
        if (!ctx->block_visited[b->id]) {
            for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
                inst->opcode = IR_NOP;
            }
            continue;
        }

        for (IRInst* inst = b->first_inst; inst != NULL; inst = inst->next) {
            if (inst->opcode == IR_NOP) {
                continue;
            }

            if (inst->src1.kind == IR_OP_VREG && inst->src1.vreg_id < ctx->vreg_cap) {
                if (ctx->lat_vals[inst->src1.vreg_id].kind == LATTICE_CONST) {
                    inst->src1 = ir_op_const(ctx->lat_vals[inst->src1.vreg_id].val, inst->src1.byte_size, inst->src1.is_signed);
                }
            }

            if (inst->src2.kind == IR_OP_VREG && inst->src2.vreg_id < ctx->vreg_cap) {
                if (ctx->lat_vals[inst->src2.vreg_id].kind == LATTICE_CONST) {
                    inst->src2 = ir_op_const(ctx->lat_vals[inst->src2.vreg_id].val, inst->src2.byte_size, inst->src2.is_signed);
                }
            }

            if (inst->opcode == IR_BR && inst->dst.kind == IR_OP_VREG && inst->dst.vreg_id < ctx->vreg_cap) {
                if (ctx->lat_vals[inst->dst.vreg_id].kind == LATTICE_CONST) {
                    inst->dst = ir_op_const(ctx->lat_vals[inst->dst.vreg_id].val, inst->dst.byte_size, inst->dst.is_signed);
                }
            }

            if (inst->opcode == IR_BR && inst->dst.kind == IR_OP_CONST) {
                IRBlock* target = (inst->dst.int_val != 0) ? inst->src1.block : inst->src2.block;

                inst->opcode = IR_JMP;
                inst->dst    = ir_op_block(target);
                inst->src1   = ir_op_none();
                inst->src2   = ir_op_none();
            }

            if (inst->opcode == IR_PHI) {
                size_t valid_preds = 0;
                IROperand last_val = ir_op_none();

                for (size_t i = 0; i < inst->extra_arg_count; i += 2) {
                    IRBlock* pred = inst->extra_args[i + 1].block;

                    if (pred != NULL && is_edge_executable(ctx, pred, b)) {
                        IROperand val = inst->extra_args[i];

                        if (val.kind == IR_OP_VREG && val.vreg_id < ctx->vreg_cap) {
                            if (ctx->lat_vals[val.vreg_id].kind == LATTICE_CONST) {
                                val = ir_op_const(ctx->lat_vals[val.vreg_id].val, val.byte_size, val.is_signed);
                            }
                        }

                        inst->extra_args[2 * valid_preds]     = val;
                        inst->extra_args[2 * valid_preds + 1] = ir_op_block(pred);

                        last_val = val;
                        valid_preds++;
                    }
                }

                inst->extra_arg_count = valid_preds * 2;

                if (inst->dst.kind == IR_OP_VREG && ctx->lat_vals[inst->dst.vreg_id].kind == LATTICE_CONST) {
                    inst->opcode          = IR_MOV;
                    inst->src1            = ir_op_const(ctx->lat_vals[inst->dst.vreg_id].val, inst->dst.byte_size, inst->dst.is_signed);
                    inst->src2            = ir_op_none();
                    inst->extra_args      = NULL;
                    inst->extra_arg_count = 0;
                } else if (valid_preds == 1) {
                    inst->opcode          = IR_MOV;
                    inst->src1            = last_val;
                    inst->src2            = ir_op_none();
                    inst->extra_args      = NULL;
                    inst->extra_arg_count = 0;
                } else if (valid_preds == 0) {
                    inst->opcode = IR_NOP;
                }
            } else if (inst->dst.kind == IR_OP_VREG && inst->dst.vreg_id < ctx->vreg_cap) {
                if (ctx->lat_vals[inst->dst.vreg_id].kind == LATTICE_CONST && inst->opcode != IR_PARAM) {
                    inst->opcode = IR_MOV;
                    inst->src1   = ir_op_const(ctx->lat_vals[inst->dst.vreg_id].val, inst->dst.byte_size, inst->dst.is_signed);
                    inst->src2   = ir_op_none();
                }
            }
        }
    }

    ir_eliminate_nops(func);
}

void sccp_run_on_function(Arena* arena, IRFunction* func) {
    if (!func || !func->first_block) {
        return;
    }

    size_t vreg_cap     = func->next_vreg_id + 1024;
    size_t block_stride = func->next_block_id + 1;
    size_t edge_matrix_size = block_stride * block_stride;

    SCCPContext ctx = {
        .arena           = arena,
        .func            = func,
        .lat_vals        = ARENA_NEW_ARRAY_ZERO(arena, LatticeVal, vreg_cap),
        .vreg_cap        = vreg_cap,
        .block_visited   = ARENA_NEW_ARRAY_ZERO(arena, bool, block_stride),
        .edge_executable = ARENA_NEW_ARRAY_ZERO(arena, bool, edge_matrix_size),
        .block_stride    = block_stride,
        .edge_worklist   = NULL,
        .ew_head         = 0,
        .ew_tail         = 0,
        .ew_cap          = 0,
        .ssa_worklist    = NULL,
        .sw_head         = 0,
        .sw_tail         = 0,
        .sw_cap          = 0,
        .in_ssa_worklist = ARENA_NEW_ARRAY_ZERO(arena, bool, vreg_cap),
        .use_chains      = ARENA_NEW_ARRAY_ZERO(arena, UseNode*, vreg_cap)
    };

    build_use_chains(&ctx);

    CFGEdge entry_edge = { .pred = NULL, .succ = func->first_block };
    ARENA_DA_PUSH(arena, ctx.edge_worklist, ctx.ew_tail, ctx.ew_cap, entry_edge);

    while (ctx.ew_head < ctx.ew_tail || ctx.sw_head < ctx.sw_tail) {
        while (ctx.ew_head < ctx.ew_tail) {
            CFGEdge edge = ctx.edge_worklist[ctx.ew_head++];
            IRBlock* succ = edge.succ;

            bool first_visit = !ctx.block_visited[succ->id];
            ctx.block_visited[succ->id] = true;

            if (first_visit) {
                evaluate_block(&ctx, succ);
            } else {
                for (IRInst* inst = succ->first_inst; inst != NULL && inst->opcode == IR_PHI; inst = inst->next) {
                    if (inst->dst.kind == IR_OP_VREG) {
                        LatticeVal val = evaluate_inst(&ctx, inst, succ);
                        update_lattice_value(&ctx, inst->dst.vreg_id, val);
                    }
                }
            }
        }

        while (ctx.sw_head < ctx.sw_tail) {
            uint32_t vid = ctx.ssa_worklist[ctx.sw_head++];
            ctx.in_ssa_worklist[vid] = false;

            for (UseNode* u = ctx.use_chains[vid]; u != NULL; u = u->next) {
                if (ctx.block_visited[u->block->id]) {
                    if (u->inst->dst.kind == IR_OP_VREG) {
                        LatticeVal val = evaluate_inst(&ctx, u->inst, u->block);
                        update_lattice_value(&ctx, u->inst->dst.vreg_id, val);
                    }

                    if (u->inst->opcode == IR_BR) {
                        LatticeVal cond_val = get_operand_lattice(&ctx, u->inst->dst);

                        if (cond_val.kind == LATTICE_CONST) {
                            if (cond_val.val != 0) {
                                mark_edge_executable(&ctx, u->block, u->inst->src1.block);
                            } else {
                                mark_edge_executable(&ctx, u->block, u->inst->src2.block);
                            }
                        } else if (cond_val.kind == LATTICE_BOT) {
                            mark_edge_executable(&ctx, u->block, u->inst->src1.block);
                            mark_edge_executable(&ctx, u->block, u->inst->src2.block);
                        }
                    }
                }
            }
        }
    }

    apply_sccp_results(&ctx);
}

void sccp_run_on_module(Arena* arena, IRModule* module) {
    if (!module) {
        return;
    }

    for (IRFunction* f = module->first_func; f != NULL; f = f->next) {
        sccp_run_on_function(arena, f);
    }
}