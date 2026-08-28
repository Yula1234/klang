#include "eval.h"
#include "sema.h"

#include <string.h>

static bool eval_unary_op(TokenKind op, EvalValue in, EvalValue* out) {
    if (in.kind != EVAL_VAL_INT) {
        return false;
    }

    out->kind = EVAL_VAL_INT;
    out->type = in.type;

    size_t sz = (in.type && in.type->size) ? in.type->size : 8;
    bool is_sgn = type_is_signed(in.type);

    switch (op) {
        case TOK_MINUS:
            out->int_val = int_truncate_to_width(-in.int_val, sz, is_sgn);
            return true;

        case TOK_PLUS:
            out->int_val = in.int_val;
            return true;

        case TOK_TILDE:
            out->int_val = int_truncate_to_width(~in.int_val, sz, is_sgn);
            return true;

        case TOK_BANG:
            out->type    = type_primitive(TYPE_BOOL);
            out->int_val = (in.int_val == 0) ? 1 : 0;
            return true;

        default:
            return false;
    }
}

static bool eval_binary_op(TokenKind op, EvalValue lhs, EvalValue rhs, Type* res_type, EvalValue* out) {
    if (lhs.kind != EVAL_VAL_INT || rhs.kind != EVAL_VAL_INT) {
        return false;
    }

    out->kind = EVAL_VAL_INT;
    out->type = res_type ? res_type : lhs.type;

    size_t sz   = (out->type && out->type->size) ? out->type->size : 8;
    bool is_sgn = type_is_signed(lhs.type);

    int64_t a = lhs.int_val;
    int64_t b = rhs.int_val;
    int64_t res = 0;

    switch (op) {
        case TOK_PLUS:
            res = a + b;
            break;

        case TOK_MINUS:
            res = a - b;
            break;

        case TOK_STAR:
            res = a * b;
            break;

        case TOK_SLASH:
            if (b == 0) return false;
            if (is_sgn) {
                if (a == (-9223372036854775807LL - 1) && b == -1) return false;
                res = a / b;
            } else {
                res = (int64_t)((uint64_t)a / (uint64_t)b);
            }
            break;

        case TOK_PERCENT:
            if (b == 0) return false;
            if (is_sgn) {
                if (a == (-9223372036854775807LL - 1) && b == -1) return false;
                res = a % b;
            } else {
                res = (int64_t)((uint64_t)a % (uint64_t)b);
            }
            break;

        case TOK_AMP:
            res = a & b;
            break;

        case TOK_PIPE:
            res = a | b;
            break;

        case TOK_CARET:
            res = a ^ b;
            break;

        case TOK_SHL: {
            uint32_t shift = (uint32_t)(b & (sz == 8 ? 63 : 31));
            res = (int64_t)((uint64_t)a << shift);
            break;
        }

        case TOK_SHR: {
            uint32_t shift = (uint32_t)(b & (sz == 8 ? 63 : 31));
            if (is_sgn) {
                res = a >> shift;
            } else {
                res = (int64_t)(((uint64_t)a) >> shift);
            }
            break;
        }

        case TOK_EQ_EQ:
            out->type    = type_primitive(TYPE_BOOL);
            out->int_val = (a == b) ? 1 : 0;
            return true;

        case TOK_BANG_EQ:
            out->type    = type_primitive(TYPE_BOOL);
            out->int_val = (a != b) ? 1 : 0;
            return true;

        case TOK_LESS:
            out->type    = type_primitive(TYPE_BOOL);
            out->int_val = is_sgn ? (a < b ? 1 : 0) : ((uint64_t)a < (uint64_t)b ? 1 : 0);
            return true;

        case TOK_LESS_EQ:
            out->type    = type_primitive(TYPE_BOOL);
            out->int_val = is_sgn ? (a <= b ? 1 : 0) : ((uint64_t)a <= (uint64_t)b ? 1 : 0);
            return true;

        case TOK_GREATER:
            out->type    = type_primitive(TYPE_BOOL);
            out->int_val = is_sgn ? (a > b ? 1 : 0) : ((uint64_t)a > (uint64_t)b ? 1 : 0);
            return true;

        case TOK_GREATER_EQ:
            out->type    = type_primitive(TYPE_BOOL);
            out->int_val = is_sgn ? (a >= b ? 1 : 0) : ((uint64_t)a >= (uint64_t)b ? 1 : 0);
            return true;

        case TOK_AMP_AMP:
            out->type    = type_primitive(TYPE_BOOL);
            out->int_val = (a != 0 && b != 0) ? 1 : 0;
            return true;

        case TOK_PIPE_PIPE:
            out->type    = type_primitive(TYPE_BOOL);
            out->int_val = (a != 0 || b != 0) ? 1 : 0;
            return true;

        default:
            return false;
    }

    out->int_val = int_truncate_to_width(res, sz, is_sgn);
    return true;
}

bool eval_expr(Sema* sema, const AstExpr* expr, EvalValue* out_val) {
    if (!expr || !out_val) {
        return false;
    }

    switch (expr->kind) {
        case EXPR_INT_LIT: {
            out_val->kind    = EVAL_VAL_INT;
            out_val->type    = expr->type ? expr->type : type_primitive(TYPE_I64);
            out_val->int_val = expr->int_val;
            return true;
        }

        case EXPR_STRING_LIT: {
            out_val->kind    = EVAL_VAL_STR;
            out_val->type    = expr->type;
            out_val->str_val = expr->string_val;
            return true;
        }

        case EXPR_NULL: {
            out_val->kind    = EVAL_VAL_NULL;
            out_val->type    = expr->type ? expr->type : type_primitive(TYPE_NULL);
            out_val->int_val = 0;
            return true;
        }

        case EXPR_SIZEOF: {
            Type* t = expr->size_align_of.target_type;
            if (!t) return false;

            out_val->kind    = EVAL_VAL_INT;
            out_val->type    = type_primitive(TYPE_U64);
            out_val->int_val = (int64_t)t->size;
            return true;
        }

        case EXPR_ALIGNOF: {
            Type* t = expr->size_align_of.target_type;
            if (!t) return false;

            out_val->kind    = EVAL_VAL_INT;
            out_val->type    = type_primitive(TYPE_U64);
            out_val->int_val = (int64_t)(t->align ? t->align : 1);
            return true;
        }

        case EXPR_OFFSETOF: {
            Type* st = expr->offset_of.struct_type;
            if (type_is_pointer(st)) st = st->ptr.base;
            if (!st || (st->kind != TYPE_STRUCT && st->kind != TYPE_UNION)) return false;

            StructField* f = type_struct_lookup_field(st, expr->offset_of.field_name);
            if (!f) return false;

            out_val->kind    = EVAL_VAL_INT;
            out_val->type    = type_primitive(TYPE_U64);
            out_val->int_val = (int64_t)f->offset;
            return true;
        }

        case EXPR_VAR: {
            Symbol* sym = expr->var.symbol;
            if (!sym) return false;

            if (sym->kind == SYM_CONST) {
                out_val->kind    = EVAL_VAL_INT;
                out_val->type    = sym->type;
                out_val->int_val = sym->const_val;
                return true;
            }

            return false;
        }

        case EXPR_MEMBER: {
            if (expr->member.target->type && expr->member.target->type->kind == TYPE_ENUM) {
                EnumVariant* v = type_enum_lookup_variant(expr->member.target->type, expr->member.field_name);
                if (!v) return false;

                out_val->kind    = EVAL_VAL_INT;
                out_val->type    = expr->member.target->type;
                out_val->int_val = v->value;
                return true;
            }

            return false;
        }

        case EXPR_UNARY: {
            EvalValue inner;
            if (!eval_expr(sema, expr->unary.operand, &inner)) {
                return false;
            }

            return eval_unary_op(expr->unary.op, inner, out_val);
        }

        case EXPR_BINARY: {
            EvalValue lhs;
            if (!eval_expr(sema, expr->binary.lhs, &lhs)) {
                return false;
            }

            EvalValue rhs;
            if (!eval_expr(sema, expr->binary.rhs, &rhs)) {
                return false;
            }

            return eval_binary_op(expr->binary.op, lhs, rhs, expr->type, out_val);
        }

        case EXPR_CAST: {
            EvalValue inner;
            if (!eval_expr(sema, expr->cast.expr, &inner)) {
                return false;
            }

            Type* target_t = expr->cast.target_type;
            if (!target_t) return false;

            if (inner.kind == EVAL_VAL_INT) {
                size_t sz = target_t->size ? target_t->size : 8;
                bool is_sgn = type_is_signed(target_t);

                out_val->kind    = EVAL_VAL_INT;
                out_val->type    = target_t;
                out_val->int_val = int_truncate_to_width(inner.int_val, sz, is_sgn);
                return true;
            }

            if (inner.kind == EVAL_VAL_NULL && (type_is_pointer(target_t) || target_t->kind == TYPE_FUNC)) {
                out_val->kind    = EVAL_VAL_NULL;
                out_val->type    = target_t;
                out_val->int_val = 0;
                return true;
            }

            return false;
        }

        default:
            return false;
    }
}

bool eval_expr_const_int(Sema* sema, const AstExpr* expr, int64_t* out_int) {
    EvalValue val;

    if (!eval_expr(sema, expr, &val)) {
        return false;
    }

    if (val.kind == EVAL_VAL_INT || val.kind == EVAL_VAL_NULL) {
        if (out_int) {
            *out_int = val.int_val;
        }
        return true;
    }

    return false;
}
