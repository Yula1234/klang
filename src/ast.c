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

AstExpr* ast_expr_call(Arena* arena, StrView callee, AstExpr* callee_expr, AstExpr** args, size_t arg_count, bool is_method, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind                 = EXPR_CALL;
    expr->loc                  = loc;
    expr->call.callee_name     = callee;
    expr->call.callee_expr     = callee_expr;
    expr->call.args            = args;
    expr->call.arg_count       = arg_count;
    expr->call.is_method_call  = is_method;

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

AstExpr* ast_expr_cast(Arena* arena, Type* target_type, AstExpr* expr, SourceLoc loc) {
    AstExpr* cast_expr = ARENA_NEW_ZERO(arena, AstExpr);

    cast_expr->kind             = EXPR_CAST;
    cast_expr->loc              = loc;
    cast_expr->cast.target_type = target_type;
    cast_expr->cast.expr        = expr;

    return cast_expr;
}

AstExpr* ast_expr_sizeof(Arena* arena, Type* target_type, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind                      = EXPR_SIZEOF;
    expr->loc                       = loc;
    expr->size_align_of.target_type = target_type;

    return expr;
}

AstExpr* ast_expr_alignof(Arena* arena, Type* target_type, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind                      = EXPR_ALIGNOF;
    expr->loc                       = loc;
    expr->size_align_of.target_type = target_type;

    return expr;
}

AstExpr* ast_expr_offsetof(Arena* arena, Type* struct_type, StrView field_name, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind                  = EXPR_OFFSETOF;
    expr->loc                   = loc;
    expr->offset_of.struct_type = struct_type;
    expr->offset_of.field_name  = field_name;

    return expr;
}

AstExpr* ast_expr_asm(Arena* arena, StrView code, Type* explicit_type, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind                     = EXPR_ASM;
    expr->loc                      = loc;
    expr->inline_asm.code          = code;
    expr->inline_asm.explicit_type = explicit_type;

    return expr;
}

AstExpr* ast_expr_member(Arena* arena, AstExpr* target, StrView field_name, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind              = EXPR_MEMBER;
    expr->loc               = loc;
    expr->member.target     = target;
    expr->member.field_name = field_name;

    return expr;
}

AstExpr* ast_expr_struct_lit(Arena* arena, StrView struct_name, StrView* names, AstExpr** values, size_t count, SourceLoc loc) {
    AstExpr* expr = ARENA_NEW_ZERO(arena, AstExpr);

    expr->kind                    = EXPR_STRUCT_LIT;
    expr->loc                     = loc;
    expr->struct_lit.struct_name  = struct_name;
    expr->struct_lit.field_names  = names;
    expr->struct_lit.field_values = values;
    expr->struct_lit.field_count  = count;

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

AstStmt* ast_stmt_break(Arena* arena, SourceLoc loc) {
    AstStmt* stmt = ARENA_NEW_ZERO(arena, AstStmt);

    stmt->kind = STMT_BREAK;
    stmt->loc  = loc;

    return stmt;
}

AstStmt* ast_stmt_continue(Arena* arena, SourceLoc loc) {
    AstStmt* stmt = ARENA_NEW_ZERO(arena, AstStmt);

    stmt->kind = STMT_CONTINUE;
    stmt->loc  = loc;

    return stmt;
}

AstStmt* ast_stmt_defer(Arena* arena, AstStmt* deferred, SourceLoc loc) {
    AstStmt* stmt = ARENA_NEW_ZERO(arena, AstStmt);

    stmt->kind            = STMT_DEFER;
    stmt->loc             = loc;
    stmt->defer_stmt.stmt = deferred;

    return stmt;
}

AstStmt* ast_stmt_for(Arena* arena, AstStmt* init, AstExpr* cond, AstStmt* step, AstStmt* body, SourceLoc loc) {
    AstStmt* stmt = ARENA_NEW_ZERO(arena, AstStmt);

    stmt->kind          = STMT_FOR;
    stmt->loc           = loc;
    stmt->for_stmt.init = init;
    stmt->for_stmt.cond = cond;
    stmt->for_stmt.step = step;
    stmt->for_stmt.body = body;

    return stmt;
}

AstStmt* ast_stmt_switch(Arena* arena, AstExpr* cond, AstSwitchCase* cases, size_t case_count, SourceLoc loc) {
    AstStmt* stmt = ARENA_NEW_ZERO(arena, AstStmt);

    stmt->kind                   = STMT_SWITCH;
    stmt->loc                    = loc;
    stmt->switch_stmt.cond       = cond;
    stmt->switch_stmt.cases      = cases;
    stmt->switch_stmt.case_count = case_count;

    return stmt;
}