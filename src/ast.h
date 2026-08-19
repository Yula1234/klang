#ifndef KLANG_AST_H
#define KLANG_AST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "lexer.h"
#include "type.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstExpr  AstExpr;
typedef struct AstStmt  AstStmt;
typedef struct AstProc  AstProc;
typedef struct Symbol   Symbol;

typedef enum SymbolKind {
    SYM_VAR,
    SYM_GLOBAL_VAR,
    SYM_CONST,
    SYM_PARAM,
    SYM_PROC,
    SYM_ENUM
} SymbolKind;

struct Symbol {
    SymbolKind kind;
    StrView    name;
    Type*      type;
    int64_t    const_val;
    SourceLoc  loc;
    bool       is_defined;
};

typedef enum AstExprKind {
    EXPR_INT_LIT,
    EXPR_STRING_LIT,
    EXPR_VAR,
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_CALL,
    EXPR_INDEX,
    EXPR_CAST,
    EXPR_SIZEOF,
    EXPR_ALIGNOF,
    EXPR_OFFSETOF,
    EXPR_ASM,
    EXPR_MEMBER,
    EXPR_STRUCT_LIT
} AstExprKind;

struct AstExpr {
    AstExprKind kind;
    SourceLoc   loc;
    Type*       type;

    union {
        int64_t int_val;

        StrView string_val;

        struct {
            StrView name;
            Symbol* symbol;
        } var;

        struct {
            TokenKind op;
            AstExpr*  operand;
        } unary;

        struct {
            TokenKind op;
            AstExpr*  lhs;
            AstExpr*  rhs;
        } binary;

        struct {
            StrView   callee_name;
            AstExpr*  callee_expr;
            Symbol*   callee_sym;
            AstExpr** args;
            size_t    arg_count;
            bool      is_method_call;
        } call;

        struct {
            AstExpr* ptr;
            AstExpr* index;
        } index;

        struct {
            StrView code;
            Type*   explicit_type;
        } inline_asm;

        struct {
            Type*    target_type;
            AstExpr* expr;
        } cast;

        struct {
            Type* target_type;
        } size_align_of;

        struct {
            Type*   struct_type;
            StrView field_name;
        } offset_of;

        struct {
            AstExpr*     target;
            StrView      field_name;
            StructField* field;
        } member;

        struct {
            StrView   struct_name;
            Type*     struct_type;
            StrView*  field_names;
            AstExpr** field_values;
            size_t    field_count;
        } struct_lit;
    };
};

typedef enum AstStmtKind {
    STMT_VAR_DECL,
    STMT_ASSIGN,
    STMT_COMPOUND_ASSIGN,
    STMT_RETURN,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_DEFER,
    STMT_IF,
    STMT_WHILE,
    STMT_EXPR,
    STMT_BLOCK
} AstStmtKind;

struct AstStmt {
    AstStmtKind kind;
    SourceLoc   loc;

    union {
        struct {
            StrView  name;
            Type*    declared_type;
            AstExpr* init_expr;
            Symbol*  symbol;
        } var_decl;

        struct {
            AstExpr* target;
            AstExpr* value;
        } assign;

        struct {
            TokenKind op;
            AstExpr*  target;
            AstExpr*  value;
        } compound_assign;

        struct {
            AstExpr* expr;
        } return_stmt;

        struct {
            AstStmt* stmt;
        } defer_stmt;

        struct {
            AstExpr* cond;
            AstStmt* then_branch;
            AstStmt* else_branch;
        } if_stmt;

        struct {
            AstExpr* cond;
            AstStmt* body;
        } while_stmt;

        struct {
            AstExpr* expr;
        } expr_stmt;

        struct {
            AstStmt** stmts;
            size_t    count;
        } block;
    };
};

typedef struct AstParam {
    StrView   name;
    Type*     type;
    SourceLoc loc;
} AstParam;

struct AstProc {
    StrView   name;
    StrView   method_struct;
    AstParam* params;
    size_t    param_count;
    Type*     return_type;
    AstStmt*  body;
    SourceLoc loc;
    Symbol*   symbol;
};

typedef struct AstStructDef {
    StrView      name;
    StructField* fields;
    size_t       field_count;
    bool         is_packed;
    SourceLoc    loc;
    Type*        type;
} AstStructDef;

typedef struct AstConstDef {
    StrView   name;
    Type*     type;
    int64_t   val;
    SourceLoc loc;
    Symbol*   symbol;
} AstConstDef;

typedef struct AstGlobalVarDef {
    StrView   name;
    Type*     type;
    AstExpr*  init_expr;
    SourceLoc loc;
    Symbol*   symbol;
} AstGlobalVarDef;

typedef struct AstEnumVariantDef {
    StrView   name;
    AstExpr*  explicit_value;
    SourceLoc loc;
} AstEnumVariantDef;

typedef struct AstEnumDef {
    StrView            name;
    Type*              underlying_type;
    AstEnumVariantDef* variants;
    size_t             variant_count;
    SourceLoc          loc;
    Type*              type;
    Symbol*            symbol;
} AstEnumDef;

typedef struct AstProgram {
    AstConstDef**     consts;
    size_t            const_count;

    AstGlobalVarDef** globals;
    size_t            global_count;

    AstStructDef**    structs;
    size_t            struct_count;

    AstEnumDef**      enums;
    size_t            enum_count;

    AstProc**         procs;
    size_t            proc_count;
} AstProgram;

AstExpr* ast_expr_int_lit(Arena* arena, int64_t val, SourceLoc loc);
AstExpr* ast_expr_string_lit(Arena* arena, StrView val, SourceLoc loc);
AstExpr* ast_expr_var(Arena* arena, StrView name, SourceLoc loc);
AstExpr* ast_expr_unary(Arena* arena, TokenKind op, AstExpr* operand, SourceLoc loc);
AstExpr* ast_expr_binary(Arena* arena, TokenKind op, AstExpr* lhs, AstExpr* rhs, SourceLoc loc);
AstExpr* ast_expr_call(Arena* arena, StrView callee, AstExpr* callee_expr, AstExpr** args, size_t arg_count, bool is_method, SourceLoc loc);
AstExpr* ast_expr_index(Arena* arena, AstExpr* ptr, AstExpr* index, SourceLoc loc);
AstExpr* ast_expr_cast(Arena* arena, Type* target_type, AstExpr* expr, SourceLoc loc);
AstExpr* ast_expr_sizeof(Arena* arena, Type* target_type, SourceLoc loc);
AstExpr* ast_expr_alignof(Arena* arena, Type* target_type, SourceLoc loc);
AstExpr* ast_expr_offsetof(Arena* arena, Type* struct_type, StrView field_name, SourceLoc loc);
AstExpr* ast_expr_asm(Arena* arena, StrView code, Type* explicit_type, SourceLoc loc);
AstExpr* ast_expr_member(Arena* arena, AstExpr* target, StrView field_name, SourceLoc loc);
AstExpr* ast_expr_struct_lit(Arena* arena, StrView struct_name, StrView* names, AstExpr** values, size_t count, SourceLoc loc);

AstStmt* ast_stmt_block(Arena* arena, AstStmt** stmts, size_t count, SourceLoc loc);
AstStmt* ast_stmt_break(Arena* arena, SourceLoc loc);
AstStmt* ast_stmt_continue(Arena* arena, SourceLoc loc);
AstStmt* ast_stmt_defer(Arena* arena, AstStmt* deferred, SourceLoc loc);

#ifdef __cplusplus
}
#endif

#endif // KLANG_AST_H