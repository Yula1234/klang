#include "ast.h"

#include <string.h>

AstExpr* ast_expr_int_lit(Arena* arena, int64_t val, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind    = EXPR_INT_LIT;
    expr->loc     = loc;
    expr->int_val = val;

    return expr;
}

AstExpr* ast_expr_string_lit(Arena* arena, StrView val, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind       = EXPR_STRING_LIT;
    expr->loc        = loc;
    expr->string_val = val;

    return expr;
}

AstExpr* ast_expr_var(Arena* arena, StrView name, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind     = EXPR_VAR;
    expr->loc      = loc;
    expr->var.name = name;

    return expr;
}

AstExpr* ast_expr_unary(Arena* arena, TokenKind op, AstExpr* operand, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind          = EXPR_UNARY;
    expr->loc           = loc;
    expr->unary.op      = op;
    expr->unary.operand = operand;

    return expr;
}

AstExpr* ast_expr_binary(Arena* arena, TokenKind op, AstExpr* lhs, AstExpr* rhs, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind         = EXPR_BINARY;
    expr->loc          = loc;
    expr->binary.op    = op;
    expr->binary.lhs   = lhs;
    expr->binary.rhs   = rhs;

    return expr;
}

AstExpr* ast_expr_call(Arena* arena, StrView callee, AstExpr** args, size_t arg_count, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind             = EXPR_CALL;
    expr->loc              = loc;
    expr->call.callee_name = callee;
    expr->call.args        = args;
    expr->call.arg_count   = arg_count;

    return expr;
}

AstExpr* ast_expr_index(Arena* arena, AstExpr* ptr, AstExpr* index, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind        = EXPR_INDEX;
    expr->loc         = loc;
    expr->index.ptr   = ptr;
    expr->index.index = index;

    return expr;
}

AstStmt* ast_stmt_block(Arena* arena, AstStmt** stmts, size_t count, SourceLoc loc) {
    AstStmt* stmt = ARENA_NEW_ZERO(arena, AstStmt);

    stmt->kind        = STMT_BLOCK;
    stmt->loc         = loc;
    stmt->block.stmts = stmts;
    stmt->block.count = count;

    return stmt;
}