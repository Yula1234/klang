#include "sccp.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

typedef enum SCCPLatticeKind {
    SCCP_UNDEF = 0,
    SCCP_INT,
    SCCP_PTR,
    SCCP_OVERDEFINED
} SCCPLatticeKind;

typedef enum SCCPPointerKind {
    SCCP_PTR_UNKNOWN = 0,
    SCCP_PTR_NULL,
    SCCP_PTR_NONNULL,
    SCCP_PTR_EXACT
} SCCPPointerKind;

typedef enum SCCPPointerBaseKind {
    SCCP_PTR_BASE_GLOBAL = 0,
    SCCP_PTR_BASE_STRING,
    SCCP_PTR_BASE_STACK,
    SCCP_PTR_BASE_ABSOLUTE
} SCCPPointerBaseKind;

typedef struct SCCPInt {
    uint8_t bit_width;
    bool    is_signed;
    uint64_t bits;
} SCCPInt;

typedef struct SCCPPointer {
    SCCPPointerKind     kind;
    SCCPPointerBaseKind base_kind;
    StrView             global_name;
    uint32_t             string_id;
    int32_t              stack_offset;
    int64_t              offset;
} SCCPPointer;

typedef struct SCCPValue {
    SCCPLatticeKind kind;

    union {
        SCCPInt     integer;
        SCCPPointer pointer;
    };
} SCCPValue;

typedef struct SCCPEdge SCCPEdge;
typedef struct SCCPBlock SCCPBlock;
typedef struct SCCPUse SCCPUse;

struct SCCPEdge {
    SCCPBlock* pred;
    SCCPBlock* succ;
    bool       executable;
};

struct SCCPBlock {
    IRBlock*   block;
    size_t     dense_id;

    SCCPEdge** preds;
    size_t     pred_count;
    size_t     pred_cap;

    SCCPEdge** succs;
    size_t     succ_count;
    size_t     succ_cap;

    bool       executable;
};

struct SCCPUse {
    IRInst*    inst;
    IRBlock*   block;
    SCCPUse*   next;
};

typedef struct SCCPContext {
    Arena*      arena;
    IRFunction* func;

    SCCPValue*  values;
    size_t      value_cap;

    SCCPBlock** blocks_by_id;
    size_t      block_id_cap;
    size_t      block_count;

    SCCPEdge**  edges;
    size_t      edge_count;
    size_t      edge_cap;

    SCCPUse**   uses;

    uint32_t*   value_worklist;
    size_t      value_head;
    size_t      value_tail;
    size_t      value_cap_worklist;

    bool*       value_queued;

    SCCPEdge**  edge_worklist;
    size_t      edge_head;
    size_t      edge_tail;
    size_t      edge_cap_worklist;
} SCCPContext;

static SCCPValue sccp_undef(void) {
    return (SCCPValue){
        .kind = SCCP_UNDEF
    };
}

static SCCPValue sccp_overdefined(void) {
    return (SCCPValue){
        .kind = SCCP_OVERDEFINED
    };
}

static uint8_t sccp_width_from_bytes(size_t byte_size) {
    switch (byte_size) {
        case 1:
            return 8;

        case 2:
            return 16;

        case 4:
            return 32;

        case 8:
        default:
            return 64;
    }
}

static uint64_t sccp_bit_mask(uint8_t width) {
    if (width >= 64) {
        return UINT64_MAX;
    }

    if (width == 0) {
        return 0;
    }

    return (UINT64_C(1) << width) - 1;
}

static int64_t sccp_sign_extend(uint64_t bits, uint8_t width) {
    if (width >= 64) {
        return (int64_t)bits;
    }

    if (width == 0) {
        return 0;
    }

    uint64_t mask = sccp_bit_mask(width);
    uint64_t sign = UINT64_C(1) << (width - 1);

    bits &= mask;

    if ((bits & sign) != 0) {
        bits |= ~mask;
    }

    return (int64_t)bits;
}

static SCCPInt sccp_int_make(
    uint64_t bits,
    uint8_t width,
    bool is_signed
) {
    return (SCCPInt){
        .bit_width = width,
        .is_signed = is_signed,
        .bits = bits & sccp_bit_mask(width)
    };
}

static SCCPValue sccp_int_value(
    uint64_t bits,
    uint8_t width,
    bool is_signed
) {
    return (SCCPValue){
        .kind = SCCP_INT,
        .integer = sccp_int_make(
            bits,
            width,
            is_signed
        )
    };
}

static SCCPValue sccp_ptr_value(
    SCCPPointer pointer
) {
    return (SCCPValue){
        .kind = SCCP_PTR,
        .pointer = pointer
    };
}

static bool sccp_int_exact_equal(
    const SCCPInt* lhs,
    const SCCPInt* rhs
) {
    return
        lhs->bit_width == rhs->bit_width &&
        lhs->is_signed == rhs->is_signed &&
        lhs->bits == rhs->bits;
}

static bool strview_equal(
    StrView lhs,
    StrView rhs
) {
    return
        lhs.len == rhs.len &&
        (
            lhs.len == 0 ||
            memcmp(lhs.data, rhs.data, lhs.len) == 0
        );
}

static bool sccp_ptr_exact_equal(
    const SCCPPointer* lhs,
    const SCCPPointer* rhs
) {
    if (
        lhs->kind != SCCP_PTR_EXACT ||
        rhs->kind != SCCP_PTR_EXACT
    ) {
        return false;
    }

    if (
        lhs->base_kind != rhs->base_kind ||
        lhs->offset != rhs->offset
    ) {
        return false;
    }

    switch (lhs->base_kind) {
        case SCCP_PTR_BASE_GLOBAL:
            return strview_equal(
                lhs->global_name,
                rhs->global_name
            );

        case SCCP_PTR_BASE_STRING:
            return lhs->string_id == rhs->string_id;

        case SCCP_PTR_BASE_STACK:
            return lhs->stack_offset ==
                   rhs->stack_offset;

        case SCCP_PTR_BASE_ABSOLUTE:
            return true;
    }

    return false;
}

static bool sccp_value_equal(
    const SCCPValue* lhs,
    const SCCPValue* rhs
) {
    if (lhs->kind != rhs->kind) {
        return false;
    }

    if (
        lhs->kind == SCCP_UNDEF ||
        lhs->kind == SCCP_OVERDEFINED
    ) {
        return true;
    }

    if (lhs->kind == SCCP_INT) {
        return sccp_int_exact_equal(
            &lhs->integer,
            &rhs->integer
        );
    }

    return
        lhs->pointer.kind ==
            rhs->pointer.kind &&
        lhs->pointer.base_kind ==
            rhs->pointer.base_kind &&
        lhs->pointer.string_id ==
            rhs->pointer.string_id &&
        lhs->pointer.stack_offset ==
            rhs->pointer.stack_offset &&
        lhs->pointer.offset ==
            rhs->pointer.offset &&
        strview_equal(
            lhs->pointer.global_name,
            rhs->pointer.global_name
        );
}

static SCCPValue sccp_merge(
    SCCPValue lhs,
    SCCPValue rhs
) {
    if (lhs.kind == SCCP_UNDEF) {
        return rhs;
    }

    if (rhs.kind == SCCP_UNDEF) {
        return lhs;
    }

    if (
        lhs.kind == SCCP_OVERDEFINED ||
        rhs.kind == SCCP_OVERDEFINED
    ) {
        return sccp_overdefined();
    }

    if (
        lhs.kind == SCCP_INT &&
        rhs.kind == SCCP_INT
    ) {
        if (
            sccp_int_exact_equal(
                &lhs.integer,
                &rhs.integer
            )
        ) {
            return lhs;
        }

        return sccp_overdefined();
    }

    if (
        lhs.kind == SCCP_PTR &&
        rhs.kind == SCCP_PTR
    ) {
        if (
            lhs.pointer.kind == SCCP_PTR_NULL &&
            rhs.pointer.kind == SCCP_PTR_NULL
        ) {
            return lhs;
        }

        if (
            lhs.pointer.kind == SCCP_PTR_EXACT &&
            rhs.pointer.kind == SCCP_PTR_EXACT
        ) {
            if (
                sccp_ptr_exact_equal(
                    &lhs.pointer,
                    &rhs.pointer
                )
            ) {
                return lhs;
            }

            return sccp_ptr_value(
                (SCCPPointer){
                    .kind = SCCP_PTR_NONNULL
                }
            );
        }

        if (
            (
                lhs.pointer.kind ==
                    SCCP_PTR_NONNULL ||
                lhs.pointer.kind ==
                    SCCP_PTR_EXACT
            ) &&
            (
                rhs.pointer.kind ==
                    SCCP_PTR_NONNULL ||
                rhs.pointer.kind ==
                    SCCP_PTR_EXACT
            )
        ) {
            return sccp_ptr_value(
                (SCCPPointer){
                    .kind = SCCP_PTR_NONNULL
                }
            );
        }

        return sccp_ptr_value(
            (SCCPPointer){
                .kind = SCCP_PTR_UNKNOWN
            }
        );

        return sccp_ptr_value(
            (SCCPPointer){
                .kind = SCCP_PTR_UNKNOWN
            }
        );
    }

    return sccp_overdefined();
}

static uint64_t sccp_int_extend_to(
    SCCPInt value,
    uint8_t width
) {
    uint64_t raw =
        value.is_signed
            ? (uint64_t)sccp_sign_extend(
                  value.bits,
                  value.bit_width
              )
            : value.bits;

    return raw & sccp_bit_mask(width);
}

static SCCPValue sccp_narrow_to_operand(
    SCCPValue value,
    IROperand dst
) {
    uint8_t width =
        sccp_width_from_bytes(
            dst.byte_size
        );

    if (value.kind == SCCP_INT) {
        return sccp_int_value(
            sccp_int_extend_to(
                value.integer,
                width
            ),
            width,
            dst.is_signed
        );
    }

    if (value.kind == SCCP_PTR &&
        width != 64) {
        return sccp_overdefined();
    }

    return value;
}

static SCCPValue sccp_operand_value(
    const SCCPContext* ctx,
    IROperand operand
) {
    switch (operand.kind) {
        case IR_OP_CONST:
            return sccp_int_value(
                (uint64_t)operand.int_val,
                sccp_width_from_bytes(
                    operand.byte_size
                ),
                operand.is_signed
            );

        case IR_OP_VREG:
            if (operand.vreg_id < ctx->value_cap) {
                return sccp_narrow_to_operand(
                    ctx->values[
                        operand.vreg_id
                    ],
                    operand
                );
            }

            return sccp_overdefined();

        default:
            return sccp_overdefined();
    }
}

static bool sccp_int_to_signed(
    const SCCPInt* value,
    int64_t* result
) {
    if (value == NULL || result == NULL) {
        return false;
    }

    *result = sccp_sign_extend(
        value->bits,
        value->bit_width
    );

    return true;
}

static bool sccp_int_exact(
    const SCCPValue* value
) {
    return
        value != NULL &&
        value->kind == SCCP_INT;
}

static SCCPValue evaluate_integer_binary(
    IROpcode opcode,
    SCCPValue lhs,
    SCCPValue rhs,
    const IRInst* inst
) {
    if (
        lhs.kind == SCCP_UNDEF ||
        rhs.kind == SCCP_UNDEF
    ) {
        return sccp_undef();
    }

    if (
        lhs.kind != SCCP_INT ||
        rhs.kind != SCCP_INT
    ) {
        return sccp_overdefined();
    }

    if (
        !sccp_int_exact(&lhs) ||
        !sccp_int_exact(&rhs)
    ) {
        return sccp_overdefined();
    }

    uint8_t width =
        sccp_width_from_bytes(
            inst->dst.byte_size
        );

    bool is_signed = inst->dst.is_signed;
    uint64_t mask = sccp_bit_mask(width);

    uint64_t lhs_bits =
        sccp_int_extend_to(
            lhs.integer,
            width
        );

    uint64_t rhs_bits =
        sccp_int_extend_to(
            rhs.integer,
            width
        );

    uint64_t result = 0;

    switch (opcode) {
        case IR_ADD:
            result = lhs_bits + rhs_bits;
            break;

        case IR_SUB:
            result = lhs_bits - rhs_bits;
            break;

        case IR_MUL:
            result = lhs_bits * rhs_bits;
            break;

        case IR_DIV: {
            if (rhs_bits == 0) {
                return sccp_overdefined();
            }

            if (is_signed) {
                int64_t a =
                    sccp_sign_extend(
                        lhs_bits,
                        width
                    );

                int64_t b =
                    sccp_sign_extend(
                        rhs_bits,
                        width
                    );

                if (
                    width == 64 &&
                    a == INT64_MIN &&
                    b == -1
                ) {
                    return sccp_overdefined();
                }

                result = (uint64_t)(a / b);
            } else {
                result = lhs_bits / rhs_bits;
            }

            break;
        }

        case IR_MOD: {
            if (rhs_bits == 0) {
                return sccp_overdefined();
            }

            if (is_signed) {
                int64_t a =
                    sccp_sign_extend(
                        lhs_bits,
                        width
                    );

                int64_t b =
                    sccp_sign_extend(
                        rhs_bits,
                        width
                    );

                if (
                    width == 64 &&
                    a == INT64_MIN &&
                    b == -1
                ) {
                    return sccp_overdefined();
                }

                result = (uint64_t)(a % b);
            } else {
                result = lhs_bits % rhs_bits;
            }

            break;
        }

        case IR_AND:
            result = lhs_bits & rhs_bits;
            break;

        case IR_OR:
            result = lhs_bits | rhs_bits;
            break;

        case IR_XOR:
            result = lhs_bits ^ rhs_bits;
            break;

        case IR_SHL: {
            uint32_t shift =
                (uint32_t)(
                    rhs_bits &
                    (
                        width == 64
                            ? 63u
                            : (uint32_t)(width - 1)
                    )
                );

            result = lhs_bits << shift;
            break;
        }

        case IR_SHR: {
            uint32_t shift =
                (uint32_t)(
                    rhs_bits &
                    (
                        width == 64
                            ? 63u
                            : (uint32_t)(width - 1)
                    )
                );

            if (is_signed) {
                result =
                    (uint64_t)(
                        sccp_sign_extend(
                            lhs_bits,
                            width
                        ) >> shift
                    );
            } else {
                result = lhs_bits >> shift;
            }

            break;
        }

        default:
            return sccp_overdefined();
    }

    return sccp_int_value(
        result & mask,
        width,
        is_signed
    );
}

static SCCPValue evaluate_compare(
    SCCPContext* ctx,
    IROpcode opcode,
    IROperand lhs_operand,
    IROperand rhs_operand
) {
    SCCPValue lhs =
        sccp_operand_value(
            ctx,
            lhs_operand
        );

    SCCPValue rhs =
        sccp_operand_value(
            ctx,
            rhs_operand
        );

    if (
        lhs.kind == SCCP_UNDEF ||
        rhs.kind == SCCP_UNDEF
    ) {
        return sccp_undef();
    }

    if (
        lhs.kind == SCCP_PTR &&
        rhs.kind == SCCP_PTR
    ) {
        if (
            opcode != IR_CMP_EQ &&
            opcode != IR_CMP_NE
        ) {
            return sccp_overdefined();
        }

        bool equal = false;
        bool known = true;

        if (
            lhs.pointer.kind == SCCP_PTR_NULL &&
            rhs.pointer.kind == SCCP_PTR_NULL
        ) {
            equal = true;
        } else if (
            lhs.pointer.kind == SCCP_PTR_EXACT &&
            rhs.pointer.kind == SCCP_PTR_EXACT
        ) {
            equal =
                sccp_ptr_exact_equal(
                    &lhs.pointer,
                    &rhs.pointer
                );
        } else if (
            (
                lhs.pointer.kind ==
                    SCCP_PTR_NULL &&
                (
                    rhs.pointer.kind ==
                        SCCP_PTR_NONNULL ||
                    rhs.pointer.kind ==
                        SCCP_PTR_EXACT
                )
            ) ||
            (
                rhs.pointer.kind ==
                    SCCP_PTR_NULL &&
                (
                    lhs.pointer.kind ==
                        SCCP_PTR_NONNULL ||
                    lhs.pointer.kind ==
                        SCCP_PTR_EXACT
                )
            )
        ) {
            equal = false;
        } else {
            known = false;
        }

        if (!known) {
            return sccp_overdefined();
        }

        if (opcode == IR_CMP_NE) {
            equal = !equal;
        }

        return sccp_int_value(
            equal ? 1 : 0,
            1,
            false
        );
    }

    if (
        lhs.kind != SCCP_INT ||
        rhs.kind != SCCP_INT
    ) {
        return sccp_overdefined();
    }

    if (
        lhs.integer.bit_width !=
            rhs.integer.bit_width ||
        lhs.integer.is_signed !=
            rhs.integer.is_signed
    ) {
        return sccp_overdefined();
    }

    bool result = false;

    if (lhs.integer.is_signed) {
        int64_t a;
        int64_t b;

        sccp_int_to_signed(
            &lhs.integer,
            &a
        );

        sccp_int_to_signed(
            &rhs.integer,
            &b
        );

        switch (opcode) {
            case IR_CMP_EQ:
                result = a == b;
                break;

            case IR_CMP_NE:
                result = a != b;
                break;

            case IR_CMP_LT:
                result = a < b;
                break;

            case IR_CMP_LE:
                result = a <= b;
                break;

            case IR_CMP_GT:
                result = a > b;
                break;

            case IR_CMP_GE:
                result = a >= b;
                break;

            default:
                return sccp_overdefined();
        }
    } else {
        uint64_t a = lhs.integer.bits;
        uint64_t b = rhs.integer.bits;

        switch (opcode) {
            case IR_CMP_EQ:
                result = a == b;
                break;

            case IR_CMP_NE:
                result = a != b;
                break;

            case IR_CMP_LT:
                result = a < b;
                break;

            case IR_CMP_LE:
                result = a <= b;
                break;

            case IR_CMP_GT:
                result = a > b;
                break;

            case IR_CMP_GE:
                result = a >= b;
                break;

            default:
                return sccp_overdefined();
        }
    }

    return sccp_int_value(
        result ? 1 : 0,
        1,
        false
    );
}

static bool pointer_add_constant(
    SCCPValue base_value,
    SCCPValue offset_value,
    int direction,
    SCCPValue* result
) {
    if (
        base_value.kind != SCCP_PTR ||
        offset_value.kind != SCCP_INT
    ) {
        return false;
    }

    if (
        base_value.pointer.kind !=
        SCCP_PTR_EXACT
    ) {
        return false;
    }

    int64_t offset;

    if (offset_value.integer.is_signed) {
        offset =
            sccp_sign_extend(
                offset_value.integer.bits,
                offset_value.integer.bit_width
            );
    } else {
        uint64_t magnitude =
            offset_value.integer.bits;

        if (
            magnitude >
            (uint64_t)INT64_MAX
        ) {
            return false;
        }

        offset = (int64_t)magnitude;
    }

    if (direction < 0) {
        if (offset == INT64_MIN) {
            return false;
        }

        offset = -offset;
    }

    if (
        (offset > 0 &&
         base_value.pointer.offset >
             INT64_MAX - offset) ||
        (offset < 0 &&
         base_value.pointer.offset <
             INT64_MIN - offset)
    ) {
        return false;
    }

    SCCPPointer pointer =
        base_value.pointer;

    pointer.offset += offset;

    *result = sccp_ptr_value(pointer);

    return true;
}

static SCCPValue evaluate_inst(
    SCCPContext* ctx,
    IRInst* inst,
    IRBlock* current_block
) {
    (void)current_block;

    switch (inst->opcode) {
        case IR_MOV:
            return sccp_narrow_to_operand(
                sccp_operand_value(
                    ctx,
                    inst->src1
                ),
                inst->dst
            );

        case IR_PHI: {
            SCCPValue result =
                sccp_undef();

            bool saw_executable_edge =
                false;

            for (
                size_t i = 0;
                i + 1 < inst->extra_arg_count;
                i += 2
            ) {
                IROperand value_operand =
                    inst->extra_args[i];

                IROperand block_operand =
                    inst->extra_args[i + 1];

                if (
                    block_operand.kind !=
                        IR_OP_BLOCK ||
                    block_operand.block == NULL
                ) {
                    continue;
                }

                SCCPEdge* edge = NULL;

                SCCPBlock* pred =
                    NULL;

                for (
                    size_t e = 0;
                    e < ctx->edge_count;
                    ++e
                ) {
                    SCCPEdge* candidate =
                        ctx->edges[e];

                    if (
                        candidate->pred->block ==
                            block_operand.block &&
                        candidate->succ->block ==
                            current_block
                    ) {
                        pred =
                            candidate->pred;
                        edge =
                            candidate;
                        break;
                    }
                }

                (void)pred;

                if (
                    edge == NULL ||
                    !edge->executable
                ) {
                    continue;
                }

                saw_executable_edge = true;

                result =
                    sccp_merge(
                        result,
                        sccp_operand_value(
                            ctx,
                            value_operand
                        )
                    );

                if (
                    result.kind ==
                    SCCP_OVERDEFINED
                ) {
                    return result;
                }
            }

            return saw_executable_edge
                ? result
                : sccp_undef();
        }

        case IR_ADDR:
            if (
                inst->src1.kind ==
                IR_OP_GLOBAL
            ) {
                return sccp_ptr_value(
                    (SCCPPointer){
                        .kind =
                            SCCP_PTR_EXACT,
                        .base_kind =
                            SCCP_PTR_BASE_GLOBAL,
                        .global_name =
                            inst->src1.global_name,
                        .offset = 0
                    }
                );
            }

            if (
                inst->src1.kind ==
                IR_OP_STACK
            ) {
                return sccp_ptr_value(
                    (SCCPPointer){
                        .kind =
                            SCCP_PTR_EXACT,
                        .base_kind =
                            SCCP_PTR_BASE_STACK,
                        .stack_offset =
                            inst->src1.stack_offset,
                        .offset = 0
                    }
                );
            }

            return sccp_overdefined();

        case IR_GLOBAL_STR:
            return sccp_ptr_value(
                (SCCPPointer){
                    .kind =
                        SCCP_PTR_EXACT,
                    .base_kind =
                        SCCP_PTR_BASE_STRING,
                    .string_id =
                        inst->src1.str_id,
                    .offset = 0
                }
            );

        case IR_ALLOCA:
            return sccp_ptr_value(
                (SCCPPointer){
                    .kind =
                        SCCP_PTR_NONNULL
                }
            );

        case IR_ADD: {
            SCCPValue lhs =
                sccp_operand_value(
                    ctx,
                    inst->src1
                );

            SCCPValue rhs =
                sccp_operand_value(
                    ctx,
                    inst->src2
                );

            SCCPValue pointer_result;

            if (
                pointer_add_constant(
                    lhs,
                    rhs,
                    +1,
                    &pointer_result
                )
            ) {
                return pointer_result;
            }

            if (
                pointer_add_constant(
                    rhs,
                    lhs,
                    +1,
                    &pointer_result
                )
            ) {
                return pointer_result;
            }

            return evaluate_integer_binary(
                IR_ADD,
                lhs,
                rhs,
                inst
            );
        }

        case IR_SUB: {
            SCCPValue lhs =
                sccp_operand_value(
                    ctx,
                    inst->src1
                );

            SCCPValue rhs =
                sccp_operand_value(
                    ctx,
                    inst->src2
                );

            SCCPValue pointer_result;

            if (
                pointer_add_constant(
                    lhs,
                    rhs,
                    -1,
                    &pointer_result
                )
            ) {
                return pointer_result;
            }

            return evaluate_integer_binary(
                IR_SUB,
                lhs,
                rhs,
                inst
            );
        }

        case IR_MUL:
        case IR_DIV:
        case IR_MOD:
        case IR_AND:
        case IR_OR:
        case IR_XOR:
        case IR_SHL:
        case IR_SHR:
            return evaluate_integer_binary(
                inst->opcode,
                sccp_operand_value(
                    ctx,
                    inst->src1
                ),
                sccp_operand_value(
                    ctx,
                    inst->src2
                ),
                inst
            );

        case IR_NEG: {
            SCCPValue value =
                sccp_operand_value(
                    ctx,
                    inst->src1
                );

            if (value.kind == SCCP_UNDEF) {
                return sccp_undef();
            }

            if (value.kind != SCCP_INT) {
                return sccp_overdefined();
            }

            uint8_t width =
                sccp_width_from_bytes(
                    inst->dst.byte_size
                );

            uint64_t bits =
                value.integer.bits;

            if (value.integer.is_signed) {
                int64_t signed_value;

                sccp_int_to_signed(
                    &value.integer,
                    &signed_value
                );

                if (
                    width == 64 &&
                    signed_value == INT64_MIN
                ) {
                    return sccp_overdefined();
                }
            }

            return sccp_int_value(
                (UINT64_C(0) - bits) &
                    sccp_bit_mask(width),
                width,
                inst->dst.is_signed
            );
        }

        case IR_NOT: {
            SCCPValue value =
                sccp_operand_value(
                    ctx,
                    inst->src1
                );

            if (value.kind == SCCP_UNDEF) {
                return sccp_undef();
            }

            if (value.kind != SCCP_INT) {
                return sccp_overdefined();
            }

            uint8_t width =
                sccp_width_from_bytes(
                    inst->dst.byte_size
                );

            return sccp_int_value(
                ~value.integer.bits &
                    sccp_bit_mask(width),
                width,
                inst->dst.is_signed
            );
        }

        case IR_CMP_EQ:
        case IR_CMP_NE:
        case IR_CMP_LT:
        case IR_CMP_LE:
        case IR_CMP_GT:
        case IR_CMP_GE:
            return evaluate_compare(
                ctx,
                inst->opcode,
                inst->src1,
                inst->src2
            );

        case IR_LOAD:
        case IR_CALL:
        case IR_CALL_PTR:
        case IR_TAIL_CALL:
        case IR_TAIL_CALL_PTR:
        case IR_VA_ARG:
        case IR_INLINE_ASM:
        case IR_PARAM:
            return sccp_overdefined();

        default:
            return sccp_overdefined();
    }
}

static bool inst_defines_vreg(
    const IRInst* inst
) {
    if (inst->dst.kind != IR_OP_VREG) {
        return false;
    }

    switch (inst->opcode) {
        case IR_MOV:
        case IR_LOAD:
        case IR_ADDR:
        case IR_ALLOCA:
        case IR_GLOBAL_STR:
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_DIV:
        case IR_MOD:
        case IR_NEG:
        case IR_AND:
        case IR_OR:
        case IR_XOR:
        case IR_SHL:
        case IR_SHR:
        case IR_NOT:
        case IR_CMP_EQ:
        case IR_CMP_NE:
        case IR_CMP_LT:
        case IR_CMP_LE:
        case IR_CMP_GT:
        case IR_CMP_GE:
        case IR_CALL:
        case IR_CALL_PTR:
        case IR_TAIL_CALL:
        case IR_TAIL_CALL_PTR:
        case IR_PARAM:
        case IR_VA_ARG:
        case IR_PHI:
            return true;

        default:
            return false;
    }
}

static bool inst_reads_dst(
    const IRInst* inst
) {
    switch (inst->opcode) {
        case IR_STORE:
        case IR_MEMCPY:
        case IR_BR:
        case IR_RET:
            return true;

        default:
            return false;
    }
}

static bool instruction_has_side_effects(
    const IRInst* inst
) {
    switch (inst->opcode) {
        case IR_STORE:
        case IR_MEMCPY:
        case IR_JMP:
        case IR_BR:
        case IR_RET:
        case IR_CALL:
        case IR_CALL_PTR:
        case IR_TAIL_CALL:
        case IR_TAIL_CALL_PTR:
        case IR_PARAM:
        case IR_INLINE_ASM:
        case IR_VA_START:
        case IR_VA_END:
        case IR_VA_COPY:
            return true;

        case IR_MOV:
            return
                inst->dst.kind == IR_OP_STACK ||
                inst->dst.kind == IR_OP_GLOBAL;

        default:
            return false;
    }
}

static SCCPBlock* context_block(
    const SCCPContext* ctx,
    const IRBlock* block
) {
    if (block == NULL ||
        block->id >= ctx->block_id_cap) {
        return NULL;
    }

    return ctx->blocks_by_id[block->id];
}

static SCCPEdge* find_edge(
    SCCPBlock* pred,
    SCCPBlock* succ
) {
    if (pred == NULL ||
        succ == NULL) {
        return NULL;
    }

    for (
        size_t i = 0;
        i < pred->succ_count;
        ++i
    ) {
        if (
            pred->succs[i]->succ == succ
        ) {
            return pred->succs[i];
        }
    }

    return NULL;
}

static void add_edge(
    SCCPContext* ctx,
    SCCPBlock* pred,
    SCCPBlock* succ
) {
    if (
        pred == NULL ||
        succ == NULL
    ) {
        return;
    }

    if (
        find_edge(pred, succ) != NULL
    ) {
        return;
    }

    SCCPEdge* edge =
        ARENA_NEW_ZERO(
            ctx->arena,
            SCCPEdge
        );

    edge->pred = pred;
    edge->succ = succ;
    edge->executable = false;

    ARENA_DA_PUSH(
        ctx->arena,
        pred->succs,
        pred->succ_count,
        pred->succ_cap,
        edge
    );

    ARENA_DA_PUSH(
        ctx->arena,
        succ->preds,
        succ->pred_count,
        succ->pred_cap,
        edge
    );

    ARENA_DA_PUSH(
        ctx->arena,
        ctx->edges,
        ctx->edge_count,
        ctx->edge_cap,
        edge
    );
}

static void build_cfg(
    SCCPContext* ctx
) {
    for (
        size_t i = 0;
        i < ctx->block_id_cap;
        ++i
    ) {
        SCCPBlock* block =
            ctx->blocks_by_id[i];

        if (block == NULL) {
            continue;
        }

        IRInst* terminator =
            block->block->last_inst;

        if (terminator == NULL) {
            if (
                block->block->next_block != NULL
            ) {
                add_edge(
                    ctx,
                    block,
                    context_block(
                        ctx,
                        block->block->next_block
                    )
                );
            }

            continue;
        }

        switch (terminator->opcode) {
            case IR_JMP:
                if (
                    terminator->dst.kind ==
                    IR_OP_BLOCK
                ) {
                    add_edge(
                        ctx,
                        block,
                        context_block(
                            ctx,
                            terminator->dst.block
                        )
                    );
                }
                break;

            case IR_BR:
                if (
                    terminator->src1.kind ==
                    IR_OP_BLOCK
                ) {
                    add_edge(
                        ctx,
                        block,
                        context_block(
                            ctx,
                            terminator->src1.block
                        )
                    );
                }

                if (
                    terminator->src2.kind ==
                    IR_OP_BLOCK
                ) {
                    add_edge(
                        ctx,
                        block,
                        context_block(
                            ctx,
                            terminator->src2.block
                        )
                    );
                }
                break;

            case IR_RET:
            case IR_TAIL_CALL:
            case IR_TAIL_CALL_PTR:
                break;

            default:
                if (
                    block->block->next_block != NULL
                ) {
                    add_edge(
                        ctx,
                        block,
                        context_block(
                            ctx,
                            block->block->next_block
                        )
                    );
                }
                break;
        }
    }
}

static void register_use(
    SCCPContext* ctx,
    uint32_t vreg_id,
    IRInst* inst,
    IRBlock* block
) {
    if (
        vreg_id >= ctx->value_cap
    ) {
        return;
    }

    SCCPUse* use =
        ARENA_NEW(
            ctx->arena,
            SCCPUse
        );

    use->inst = inst;
    use->block = block;
    use->next = ctx->uses[vreg_id];

    ctx->uses[vreg_id] = use;
}

static void build_use_chains(
    SCCPContext* ctx
) {
    for (
        size_t i = 0;
        i < ctx->block_id_cap;
        ++i
    ) {
        SCCPBlock* block =
            ctx->blocks_by_id[i];

        if (block == NULL) {
            continue;
        }

        IRBlock* ir_block =
            block->block;

        for (
            IRInst* inst = ir_block->first_inst;
            inst != NULL;
            inst = inst->next
        ) {
            if (inst->src1.kind == IR_OP_VREG) {
                register_use(
                    ctx,
                    inst->src1.vreg_id,
                    inst,
                    ir_block
                );
            }

            if (inst->src2.kind == IR_OP_VREG) {
                register_use(
                    ctx,
                    inst->src2.vreg_id,
                    inst,
                    ir_block
                );
            }

            if (
                inst_reads_dst(inst) &&
                inst->dst.kind == IR_OP_VREG
            ) {
                register_use(
                    ctx,
                    inst->dst.vreg_id,
                    inst,
                    ir_block
                );
            }

            if (inst->opcode == IR_PHI) {
                for (
                    size_t k = 0;
                    k + 1 < inst->extra_arg_count;
                    k += 2
                ) {
                    if (
                        inst->extra_args[k].kind ==
                        IR_OP_VREG
                    ) {
                        register_use(
                            ctx,
                            inst->extra_args[k].vreg_id,
                            inst,
                            ir_block
                        );
                    }
                }
            } else {
                for (
                    size_t k = 0;
                    k < inst->extra_arg_count;
                    ++k
                ) {
                    if (
                        inst->extra_args[k].kind ==
                        IR_OP_VREG
                    ) {
                        register_use(
                            ctx,
                            inst->extra_args[k].vreg_id,
                            inst,
                            ir_block
                        );
                    }
                }
            }

            for (
                size_t k = 0;
                k < inst->asm_input_count;
                ++k
            ) {
                if (
                    inst->asm_inputs[k].val.kind ==
                    IR_OP_VREG
                ) {
                    register_use(
                        ctx,
                        inst->asm_inputs[k].val.vreg_id,
                        inst,
                        ir_block
                    );
                }
            }
        }
    }
}

static void queue_value(
    SCCPContext* ctx,
    uint32_t vreg_id
) {
    if (
        vreg_id >= ctx->value_cap ||
        ctx->value_queued[vreg_id]
    ) {
        return;
    }

    ctx->value_queued[vreg_id] = true;

    ARENA_DA_PUSH(
        ctx->arena,
        ctx->value_worklist,
        ctx->value_tail,
        ctx->value_cap_worklist,
        vreg_id
    );
}

static void queue_edge(
    SCCPContext* ctx,
    SCCPEdge* edge
) {
    ARENA_DA_PUSH(
        ctx->arena,
        ctx->edge_worklist,
        ctx->edge_tail,
        ctx->edge_cap_worklist,
        edge
    );
}

static void update_value(
    SCCPContext* ctx,
    uint32_t vreg_id,
    SCCPValue new_value
) {
    if (
        vreg_id >= ctx->value_cap
    ) {
        return;
    }

    SCCPValue old_value =
        ctx->values[vreg_id];

    SCCPValue merged =
        sccp_merge(
            old_value,
            new_value
        );

    if (
        sccp_value_equal(
            &old_value,
            &merged
        )
    ) {
        return;
    }

    ctx->values[vreg_id] = merged;

    queue_value(
        ctx,
        vreg_id
    );
}

static void mark_edge_executable(
    SCCPContext* ctx,
    SCCPEdge* edge
) {
    if (
        edge == NULL ||
        edge->executable
    ) {
        return;
    }

    edge->executable = true;

    queue_edge(
        ctx,
        edge
    );
}

static void branch_condition(
    const SCCPValue* value,
    bool* known,
    bool* result
) {
    *known = false;
    *result = false;

    if (value->kind == SCCP_INT) {
        *known = true;
        *result =
            value->integer.bits != 0;
        return;
    }

    if (value->kind == SCCP_PTR) {
        if (
            value->pointer.kind ==
            SCCP_PTR_NULL
        ) {
            *known = true;
            *result = false;
            return;
        }

        if (
            value->pointer.kind ==
                SCCP_PTR_NONNULL ||
            value->pointer.kind ==
                SCCP_PTR_EXACT
        ) {
            *known = true;
            *result = true;
            return;
        }
    }
}

static void evaluate_terminator(
    SCCPContext* ctx,
    SCCPBlock* block
) {
    IRInst* terminator =
        block->block->last_inst;

    if (terminator == NULL) {
        if (
            block->block->next_block != NULL
        ) {
            mark_edge_executable(
                ctx,
                find_edge(
                    block,
                    context_block(
                        ctx,
                        block->block->next_block
                    )
                )
            );
        }

        return;
    }

    if (terminator->opcode == IR_JMP) {
        if (
            terminator->dst.kind ==
            IR_OP_BLOCK
        ) {
            mark_edge_executable(
                ctx,
                find_edge(
                    block,
                    context_block(
                        ctx,
                        terminator->dst.block
                    )
                )
            );
        }

        return;
    }

    if (terminator->opcode != IR_BR) {
        return;
    }

    SCCPValue condition =
        sccp_operand_value(
            ctx,
            terminator->dst
        );

    bool known;
    bool result;

    branch_condition(
        &condition,
        &known,
        &result
    );

    SCCPBlock* true_block =
        context_block(
            ctx,
            terminator->src1.block
        );

    SCCPBlock* false_block =
        context_block(
            ctx,
            terminator->src2.block
        );

    SCCPEdge* true_edge =
        find_edge(
            block,
            true_block
        );

    SCCPEdge* false_edge =
        find_edge(
            block,
            false_block
        );

    if (!known || result) {
        mark_edge_executable(
            ctx,
            true_edge
        );
    }

    if (!known || !result) {
        mark_edge_executable(
            ctx,
            false_edge
        );
    }
}

static void process_block(
    SCCPContext* ctx,
    SCCPBlock* block
) {
    if (block == NULL) {
        return;
    }

    for (
        IRInst* inst =
            block->block->first_inst;
        inst != NULL;
        inst = inst->next
    ) {
        if (!inst_defines_vreg(inst)) {
            continue;
        }

        update_value(
            ctx,
            inst->dst.vreg_id,
            evaluate_inst(
                ctx,
                inst,
                block->block
            )
        );
    }

    evaluate_terminator(
        ctx,
        block
    );
}

static void process_value(
    SCCPContext* ctx,
    uint32_t vreg_id
) {
    for (
        SCCPUse* use =
            ctx->uses[vreg_id];
        use != NULL;
        use = use->next
    ) {
        SCCPBlock* block =
            context_block(
                ctx,
                use->block
            );

        if (
            block == NULL ||
            !block->executable
        ) {
            continue;
        }

        IRInst* inst = use->inst;

        if (inst_defines_vreg(inst)) {
            update_value(
                ctx,
                inst->dst.vreg_id,
                evaluate_inst(
                    ctx,
                    inst,
                    use->block
                )
            );
        }

        if (inst->opcode == IR_BR) {
            evaluate_terminator(
                ctx,
                block
            );
        }
    }
}

static void fold_operand(
    IROperand* operand,
    const SCCPContext* ctx
) {
    if (
        operand == NULL ||
        operand->kind != IR_OP_VREG ||
        operand->vreg_id >= ctx->value_cap
    ) {
        return;
    }

    SCCPValue value =
        ctx->values[
            operand->vreg_id
        ];

    if (value.kind == SCCP_INT) {
        *operand = ir_op_const(
            (int64_t)sccp_int_extend_to(
                value.integer,
                sccp_width_from_bytes(
                    operand->byte_size
                )
            ),
            operand->byte_size,
            operand->is_signed
        );
    }
}

static void apply_results(
    SCCPContext* ctx
) {
    for (
        size_t i = 0;
        i < ctx->block_id_cap;
        ++i
    ) {
        SCCPBlock* block =
            ctx->blocks_by_id[i];

        if (block == NULL) {
            continue;
        }

        if (!block->executable) {
            for (
                IRInst* inst =
                    block->block->first_inst;
                inst != NULL;
                inst = inst->next
            ) {
                inst->opcode = IR_NOP;
            }

            continue;
        }

        for (
            IRInst* inst =
                block->block->first_inst;
            inst != NULL;
            inst = inst->next
        ) {
            if (inst->opcode == IR_NOP) {
                continue;
            }

            fold_operand(
                &inst->src1,
                ctx
            );

            fold_operand(
                &inst->src2,
                ctx
            );

            if (
                inst_reads_dst(inst) &&
                inst->dst.kind ==
                    IR_OP_VREG
            ) {
                fold_operand(
                    &inst->dst,
                    ctx
                );
            }

            if (inst->opcode == IR_PHI) {
                for (
                    size_t k = 0;
                    k + 1 < inst->extra_arg_count;
                    k += 2
                ) {
                    fold_operand(
                        &inst->extra_args[k],
                        ctx
                    );
                }
            } else {
                for (
                    size_t k = 0;
                    k < inst->extra_arg_count;
                    ++k
                ) {
                    fold_operand(
                        &inst->extra_args[k],
                        ctx
                    );
                }
            }

            for (
                size_t k = 0;
                k < inst->asm_input_count;
                ++k
            ) {
                fold_operand(
                    &inst->asm_inputs[k].val,
                    ctx
                );
            }

            if (inst->opcode == IR_BR) {
                SCCPValue condition =
                    sccp_operand_value(
                        ctx,
                        inst->dst
                    );

                bool known;
                bool result;

                branch_condition(
                    &condition,
                    &known,
                    &result
                );

                if (known) {
                    IRBlock* target =
                        result
                            ? inst->src1.block
                            : inst->src2.block;

                    inst->opcode = IR_JMP;
                    inst->dst =
                        ir_op_block(target);
                    inst->src1 =
                        ir_op_none();
                    inst->src2 =
                        ir_op_none();
                    inst->extra_args = NULL;
                    inst->extra_arg_count = 0;

                    continue;
                }
            }

            if (
                inst->opcode == IR_PHI &&
                inst->dst.kind ==
                    IR_OP_VREG &&
                inst->dst.vreg_id <
                    ctx->value_cap
            ) {
                SCCPValue value =
                    ctx->values[
                        inst->dst.vreg_id
                    ];

                if (value.kind == SCCP_INT) {
                    inst->opcode = IR_MOV;
                    inst->src1 =
                        ir_op_const(
                            (int64_t)value.integer.bits,
                            inst->dst.byte_size,
                            inst->dst.is_signed
                        );
                    inst->src2 =
                        ir_op_none();
                    inst->extra_args = NULL;
                    inst->extra_arg_count = 0;

                    continue;
                }
            }

            if (
                inst->dst.kind ==
                    IR_OP_VREG &&
                inst->dst.vreg_id <
                    ctx->value_cap &&
                !instruction_has_side_effects(inst)
            ) {
                SCCPValue value =
                    ctx->values[
                        inst->dst.vreg_id
                    ];

                if (value.kind == SCCP_INT) {
                    inst->opcode = IR_MOV;
                    inst->src1 =
                        ir_op_const(
                            (int64_t)value.integer.bits,
                            inst->dst.byte_size,
                            inst->dst.is_signed
                        );
                    inst->src2 =
                        ir_op_none();
                    inst->extra_args = NULL;
                    inst->extra_arg_count = 0;
                }
            }
        }
    }

    ir_eliminate_nops(
        ctx->func
    );
}

static void initialize_blocks(
    SCCPContext* ctx
) {
    size_t dense_id = 0;

    for (
        IRBlock* block =
            ctx->func->first_block;
        block != NULL;
        block = block->next_block
    ) {
        assert(
            block->id <
            ctx->block_id_cap
        );

        assert(
            ctx->blocks_by_id[
                block->id
            ] == NULL
        );

        SCCPBlock* sccp_block =
            ARENA_NEW_ZERO(
                ctx->arena,
                SCCPBlock
            );

        sccp_block->block = block;
        sccp_block->dense_id = dense_id++;

        ctx->blocks_by_id[
            block->id
        ] = sccp_block;
    }

    ctx->block_count = dense_id;
}

void sccp_run_on_function(
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

    size_t value_cap =
        (size_t)func->next_vreg_id + 1;

    size_t block_id_cap =
        (size_t)func->next_block_id + 1;

    if (
        value_cap == 0 ||
        block_id_cap == 0
    ) {
        return;
    }

    SCCPContext ctx = {
        .arena = arena,
        .func = func,

        .values =
            ARENA_NEW_ARRAY(
                arena,
                SCCPValue,
                value_cap
            ),

        .value_cap = value_cap,

        .blocks_by_id =
            ARENA_NEW_ARRAY_ZERO(
                arena,
                SCCPBlock*,
                block_id_cap
            ),

        .block_id_cap = block_id_cap,
        .block_count = 0,

        .edges = NULL,
        .edge_count = 0,
        .edge_cap = 0,

        .uses =
            ARENA_NEW_ARRAY_ZERO(
                arena,
                SCCPUse*,
                value_cap
            ),

        .value_worklist = NULL,
        .value_head = 0,
        .value_tail = 0,
        .value_cap_worklist = 0,

        .value_queued =
            ARENA_NEW_ARRAY_ZERO(
                arena,
                bool,
                value_cap
            ),

        .edge_worklist = NULL,
        .edge_head = 0,
        .edge_tail = 0,
        .edge_cap_worklist = 0
    };

    for (
        size_t i = 0;
        i < value_cap;
        ++i
    ) {
        ctx.values[i] =
            sccp_undef();
    }

    initialize_blocks(&ctx);
    build_cfg(&ctx);
    build_use_chains(&ctx);

    SCCPBlock* entry =
        context_block(
            &ctx,
            func->first_block
        );

    if (entry == NULL) {
        return;
    }

    if (!entry->executable) {
        entry->executable = true;
        process_block(
            &ctx,
            entry
        );
    }

    while (
        ctx.edge_head <
            ctx.edge_tail ||
        ctx.value_head <
            ctx.value_tail
    ) {
        while (
            ctx.edge_head <
            ctx.edge_tail
        ) {
            SCCPEdge* edge =
                ctx.edge_worklist[
                    ctx.edge_head++
                ];

            if (
                edge == NULL ||
                edge->succ == NULL
            ) {
                continue;
            }

            bool first_visit =
                !edge->succ->executable;

            edge->succ->executable =
                true;

            if (first_visit) {
                process_block(
                    &ctx,
                    edge->succ
                );
            } else {
                for (
                    IRInst* inst =
                        edge->succ->block->first_inst;
                    inst != NULL &&
                    inst->opcode == IR_PHI;
                    inst = inst->next
                ) {
                    if (
                        inst->dst.kind ==
                        IR_OP_VREG
                    ) {
                        update_value(
                            &ctx,
                            inst->dst.vreg_id,
                            evaluate_inst(
                                &ctx,
                                inst,
                                edge->succ->block
                            )
                        );
                    }
                }
            }
        }

        while (
            ctx.value_head <
            ctx.value_tail
        ) {
            uint32_t vreg_id =
                ctx.value_worklist[
                    ctx.value_head++
                ];

            ctx.value_queued[
                vreg_id
            ] = false;

            process_value(
                &ctx,
                vreg_id
            );
        }
    }

    apply_results(&ctx);
}

void sccp_run_on_module(
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
        sccp_run_on_function(
            arena,
            func
        );
    }
}
