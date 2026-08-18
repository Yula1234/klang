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

typedef struct AstExpr AstExpr;
typedef struct AstStmt AstStmt;
typedef struct Symbol  Symbol;

typedef enum SymbolKind {
    SYM_VAR,
    SYM_PARAM,
    SYM_PROC
} SymbolKind;

struct Symbol {
    SymbolKind kind;
    StrView    name;
    Type*      type;
    int32_t    stack_offset;
    bool       is_defined;
};

typedef enum AstExprKind {
    EXPR_INT_LIT,    
    EXPR_STRING_LIT, 
    EXPR_VAR,        
    EXPR_UNARY,      
    EXPR_BINARY,     
    EXPR_CALL,
    EXPR_INDEX       
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
            Symbol*   callee_sym;
            AstExpr** args;
            size_t    arg_count;
        } call;

        struct {
            AstExpr* ptr;
            AstExpr* index;
        } index;
    };
};

typedef enum AstStmtKind {
    STMT_VAR_DECL,         
    STMT_ASSIGN,           
    STMT_COMPOUND_ASSIGN,  
    STMT_RETURN,           
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

typedef struct AstProc {
    StrView   name;
    AstParam* params;
    size_t    param_count;
    Type*     return_type;
    AstStmt*  body;
    SourceLoc loc;

    size_t    stack_frame_size;
    Symbol*   symbol;
} AstProc;

typedef struct AstProgram {
    AstProc** procs;
    size_t    proc_count;
} AstProgram;

AstExpr* ast_expr_int_lit(Arena* arena, int64_t val, SourceLoc loc);

AstExpr* ast_expr_string_lit(Arena* arena, StrView val, SourceLoc loc);

AstExpr* ast_expr_var(Arena* arena, StrView name, SourceLoc loc);

AstExpr* ast_expr_unary(Arena* arena, TokenKind op, AstExpr* operand, SourceLoc loc);

AstExpr* ast_expr_binary(Arena* arena, TokenKind op, AstExpr* lhs, AstExpr* rhs, SourceLoc loc);

AstExpr* ast_expr_call(Arena* arena, StrView callee, AstExpr** args, size_t arg_count, SourceLoc loc);

AstExpr* ast_expr_index(Arena* arena, AstExpr* ptr, AstExpr* index, SourceLoc loc);

AstStmt* ast_stmt_block(Arena* arena, AstStmt** stmts, size_t count, SourceLoc loc);

#ifdef __cplusplus
}
#endif

#endif // KLANG_AST_H