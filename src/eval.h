#ifndef KLANG_EVAL_H
#define KLANG_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#include "ast.h"
#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Sema Sema;

typedef enum EvalValueKind {
    EVAL_VAL_INT,
    EVAL_VAL_STR,
    EVAL_VAL_NULL
} EvalValueKind;

typedef struct EvalValue {
    EvalValueKind kind;
    Type*         type;

    union {
        int64_t int_val;
        StrView str_val;
    };
} EvalValue;

bool eval_expr(Sema* sema, const AstExpr* expr, EvalValue* out_val);

bool eval_expr_const_int(Sema* sema, const AstExpr* expr, int64_t* out_int);

#ifdef __cplusplus
}
#endif

#endif