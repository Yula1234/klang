#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static void parser_error_at(Parser* parser, SourceLoc loc, const char* fmt, ...) {
    parser->had_error = true;

    fprintf(stderr, "%s:%u:%u: error: ", loc.filename, loc.line, loc.col);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
}

static void parser_advance(Parser* parser) {
    parser->prev = parser->current;

    while (true) {
        parser->current = lexer_next_token(parser->lexer);

        if (parser->current.kind != TOK_ERROR) {
            break;
        }

        parser_error_at(parser, parser->current.loc, "%.*s", 
                        (int)parser->current.lexeme.len, 
                        parser->current.lexeme.data);
    }
}

static inline bool parser_check(const Parser* parser, TokenKind kind) {
    return parser->current.kind == kind;
}

static bool parser_match(Parser* parser, TokenKind kind) {
    if (parser_check(parser, kind)) {
        parser_advance(parser);
        return true;
    }

    return false;
}

static Token parser_expect(Parser* parser, TokenKind kind, const char* err_msg) {
    if (parser_check(parser, kind)) {
        Token tok = parser->current;
        parser_advance(parser);
        return tok;
    }

    parser_error_at(parser, parser->current.loc, "%s (got '%s')", 
                    err_msg, token_kind_to_str(parser->current.kind));

    return parser->current;
}

static int64_t parse_int_literal(StrView text) {
    int64_t result = 0;
    size_t i = 0;

    if (text.len > 2 && text.data[0] == '0' && (text.data[1] == 'x' || text.data[1] == 'X')) {
        i = 2;
        while (i < text.len) {
            char c = text.data[i++];
            if (c == '_') continue;

            result *= 16;
            if (c >= '0' && c <= '9')      result += (c - '0');
            else if (c >= 'a' && c <= 'f') result += (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') result += (c - 'A' + 10);
        }
        return result;
    }

    if (text.len > 2 && text.data[0] == '0' && (text.data[1] == 'b' || text.data[1] == 'B')) {
        i = 2;
        while (i < text.len) {
            char c = text.data[i++];
            if (c == '_') continue;

            result = (result << 1) | (c - '0');
        }
        return result;
    }

    while (i < text.len) {
        char c = text.data[i++];
        if (c == '_') continue;

        result = (result * 10) + (c - '0');
    }

    return result;
}

static Type* parse_type(Parser* parser) {
    Type* base_type = NULL;
    SourceLoc loc = parser->current.loc;

    switch (parser->current.kind) {
        case TOK_U8:   base_type = type_primitive(TYPE_U8);   parser_advance(parser); break;
        case TOK_U16:  base_type = type_primitive(TYPE_U16);  parser_advance(parser); break;
        case TOK_U32:  base_type = type_primitive(TYPE_U32);  parser_advance(parser); break;
        case TOK_U64:  base_type = type_primitive(TYPE_U64);  parser_advance(parser); break;

        case TOK_I8:   base_type = type_primitive(TYPE_I8);   parser_advance(parser); break;
        case TOK_I16:  base_type = type_primitive(TYPE_I16);  parser_advance(parser); break;
        case TOK_I32:  base_type = type_primitive(TYPE_I32);  parser_advance(parser); break;
        case TOK_I64:  base_type = type_primitive(TYPE_I64);  parser_advance(parser); break;

        case TOK_CHAR: base_type = type_primitive(TYPE_CHAR); parser_advance(parser); break;
        case TOK_VOID: base_type = type_primitive(TYPE_VOID); parser_advance(parser); break;

        default:
            parser_error_at(parser, loc, "expected type name");
            return type_primitive(TYPE_VOID);
    }

    while (parser_match(parser, TOK_STAR)) {
        base_type = type_ptr(parser->arena, base_type);
    }

    return base_type;
}

static AstExpr* parse_expr_precedence(Parser* parser, int min_prec);

static AstExpr* parse_postfix(Parser* parser, AstExpr* expr) {
    while (true) {
        if (parser_match(parser, TOK_LBRACKET)) {
            SourceLoc loc = parser->prev.loc;
            AstExpr* index_expr = parse_expr_precedence(parser, 0);

            parser_expect(parser, TOK_RBRACKET, "expected ']' after array index");

            expr = ast_expr_index(parser->arena, expr, index_expr, loc);
        } else {
            break;
        }
    }

    return expr;
}

static AstExpr* parse_prefix_expr(Parser* parser) {
    SourceLoc loc = parser->current.loc;
    AstExpr* expr = NULL;

    if (parser_match(parser, TOK_INT_LIT)) {
        int64_t val = parse_int_literal(parser->prev.lexeme);
        return ast_expr_int_lit(parser->arena, val, loc);
    }

    if (parser_match(parser, TOK_STRING_LIT)) {
        StrView raw = parser->prev.lexeme;

        if (raw.len >= 2 && raw.data[0] == '"') {
            raw.data += 1;
            raw.len  -= 2;
        }

        return ast_expr_string_lit(parser->arena, raw, loc);
    }

    if (parser_match(parser, TOK_ASM)) {
        Token code_tok = parser_expect(parser, TOK_STRING_LIT, "expected string literal after 'asm'");
        StrView code = code_tok.lexeme;

        if (code.len >= 2 && code.data[0] == '"') {
            code.data += 1;
            code.len  -= 2;
        }

        Type* explicit_type = NULL;

        if (parser_match(parser, TOK_ARROW)) {
            explicit_type = parse_type(parser);
        }

        return ast_expr_asm(parser->arena, code, explicit_type, loc);
    }

    if (parser_match(parser, TOK_IDENT)) {
        StrView name = parser->prev.lexeme;

        if (parser_match(parser, TOK_LPAREN)) {
            size_t cap = 4;
            size_t count = 0;
            AstExpr** args = ARENA_NEW_ARRAY(parser->arena, AstExpr*, cap);

            if (!parser_check(parser, TOK_RPAREN)) {
                while (true) {
                    if (count >= cap) {
                        size_t new_cap = cap * 2;
                        args = (AstExpr**)arena_realloc(parser->arena, args, 
                                                        cap * sizeof(AstExpr*), 
                                                        new_cap * sizeof(AstExpr*));
                        cap = new_cap;
                    }

                    args[count++] = parse_expr_precedence(parser, 0);

                    if (!parser_match(parser, TOK_COMMA)) {
                        break;
                    }
                }
            }

            parser_expect(parser, TOK_RPAREN, "expected ')' after argument list");

            expr = ast_expr_call(parser->arena, name, args, count, loc);
            return parse_postfix(parser, expr);
        }

        expr = ast_expr_var(parser->arena, name, loc);
        return parse_postfix(parser, expr);
    }

     if (parser_match(parser, TOK_STAR)  || 
        parser_match(parser, TOK_MINUS) || 
        parser_match(parser, TOK_PLUS)  || 
        parser_match(parser, TOK_TILDE)) {
        
        TokenKind op = parser->prev.kind;
        AstExpr* operand = parse_expr_precedence(parser, 9);

        return ast_expr_unary(parser->arena, op, operand, loc);
    }

    if (parser_match(parser, TOK_LPAREN)) {
        AstExpr* inner = parse_expr_precedence(parser, 0);

        parser_expect(parser, TOK_RPAREN, "expected ')' after expression");

        return parse_postfix(parser, inner);
    }

    parser_error_at(parser, loc, "expected expression");
    parser_advance(parser);

    return ast_expr_int_lit(parser->arena, 0, loc);
}

static int get_binary_precedence(TokenKind kind) {
    switch (kind) {
        case TOK_PIPE:     return 1;
        case TOK_CARET:    return 2;
        case TOK_AMP:      return 3;
        case TOK_EQ_EQ:
        case TOK_BANG_EQ:  return 4;
        case TOK_LESS:
        case TOK_GREATER:  return 5;
        case TOK_SHL:
        case TOK_SHR:      return 6;
        case TOK_PLUS:
        case TOK_MINUS:    return 7;
        case TOK_STAR:
        case TOK_SLASH:    return 8;
        default:           return 0;
    }
}

static AstExpr* parse_expr_precedence(Parser* parser, int min_prec) {
    AstExpr* lhs = parse_prefix_expr(parser);

    while (true) {
        TokenKind op = parser->current.kind;
        int prec = get_binary_precedence(op);

        if (prec == 0 || prec < min_prec) {
            break;
        }

        SourceLoc op_loc = parser->current.loc;
        parser_advance(parser);

        AstExpr* rhs = parse_expr_precedence(parser, prec + 1);

        lhs = ast_expr_binary(parser->arena, op, lhs, rhs, op_loc);
    }

    return lhs;
}

static inline AstExpr* parse_expr(Parser* parser) {
    return parse_expr_precedence(parser, 0);
}

static AstStmt* parse_stmt(Parser* parser);

static AstStmt* parse_block(Parser* parser) {
    SourceLoc loc = parser->current.loc;
    parser_expect(parser, TOK_LBRACE, "expected '{' to begin block");

    size_t cap = 8;
    size_t count = 0;
    AstStmt** stmts = ARENA_NEW_ARRAY(parser->arena, AstStmt*, cap);

    while (!parser_check(parser, TOK_RBRACE) && !parser_check(parser, TOK_EOF)) {
        if (count >= cap) {
            size_t new_cap = cap * 2;
            stmts = (AstStmt**)arena_realloc(parser->arena, stmts, 
                                             cap * sizeof(AstStmt*), 
                                             new_cap * sizeof(AstStmt*));
            cap = new_cap;
        }

        stmts[count++] = parse_stmt(parser);
    }

    parser_expect(parser, TOK_RBRACE, "expected '}' to end block");

    return ast_stmt_block(parser->arena, stmts, count, loc);
}

static AstStmt* parse_stmt(Parser* parser) {
    SourceLoc loc = parser->current.loc;

    if (parser_check(parser, TOK_LBRACE)) {
        return parse_block(parser);
    }

    if (parser_match(parser, TOK_VAR)) {
        Token name_tok = parser_expect(parser, TOK_IDENT, "expected variable name after 'var'");
        Type* declared_type = NULL;

        if (parser_match(parser, TOK_COLON)) {
            declared_type = parse_type(parser);
        }

        parser_expect(parser, TOK_EQ, "expected '=' in variable declaration");

        AstExpr* init_expr = parse_expr(parser);

        parser_expect(parser, TOK_SEMICOLON, "expected ';' after variable declaration");

        AstStmt* stmt = ARENA_NEW_ZERO(parser->arena, AstStmt);
        stmt->kind = STMT_VAR_DECL;
        stmt->loc  = loc;
        stmt->var_decl.name          = name_tok.lexeme;
        stmt->var_decl.declared_type = declared_type;
        stmt->var_decl.init_expr     = init_expr;

        return stmt;
    }

    if (parser_match(parser, TOK_RETURN)) {
        AstExpr* expr = NULL;

        if (!parser_check(parser, TOK_SEMICOLON)) {
            expr = parse_expr(parser);
        }

        parser_expect(parser, TOK_SEMICOLON, "expected ';' after return statement");

        AstStmt* stmt = ARENA_NEW_ZERO(parser->arena, AstStmt);
        stmt->kind = STMT_RETURN;
        stmt->loc  = loc;
        stmt->return_stmt.expr = expr;

        return stmt;
    }

    if (parser_match(parser, TOK_IF)) {
        parser_expect(parser, TOK_LPAREN, "expected '(' after 'if'");
        AstExpr* cond = parse_expr(parser);
        parser_expect(parser, TOK_RPAREN, "expected ')' after if condition");

        AstStmt* then_branch = parse_stmt(parser);
        AstStmt* else_branch = NULL;

        if (parser_match(parser, TOK_ELSE)) {
            else_branch = parse_stmt(parser);
        }

        AstStmt* stmt = ARENA_NEW_ZERO(parser->arena, AstStmt);
        stmt->kind = STMT_IF;
        stmt->loc  = loc;
        stmt->if_stmt.cond        = cond;
        stmt->if_stmt.then_branch = then_branch;
        stmt->if_stmt.else_branch = else_branch;

        return stmt;
    }

    if (parser_match(parser, TOK_WHILE)) {
        parser_expect(parser, TOK_LPAREN, "expected '(' after 'while'");
        AstExpr* cond = parse_expr(parser);
        parser_expect(parser, TOK_RPAREN, "expected ')' after while condition");

        AstStmt* body = parse_stmt(parser);

        AstStmt* stmt = ARENA_NEW_ZERO(parser->arena, AstStmt);
        stmt->kind = STMT_WHILE;
        stmt->loc  = loc;
        stmt->while_stmt.cond = cond;
        stmt->while_stmt.body = body;

        return stmt;
    }

    AstExpr* expr = parse_expr(parser);

    if (parser_match(parser, TOK_EQ)) {
        AstExpr* value = parse_expr(parser);

        parser_expect(parser, TOK_SEMICOLON, "expected ';' after assignment");

        AstStmt* stmt = ARENA_NEW_ZERO(parser->arena, AstStmt);
        stmt->kind = STMT_ASSIGN;
        stmt->loc  = loc;
        stmt->assign.target = expr;
        stmt->assign.value  = value;

        return stmt;
    }

    if (parser_match(parser, TOK_PLUS_EQ)  || 
        parser_match(parser, TOK_MINUS_EQ) ||
        parser_match(parser, TOK_AMP_EQ)   ||
        parser_match(parser, TOK_PIPE_EQ)  ||
        parser_match(parser, TOK_CARET_EQ) ||
        parser_match(parser, TOK_SHL_EQ)   ||
        parser_match(parser, TOK_SHR_EQ)) {

        TokenKind op = parser->prev.kind;
        AstExpr* value = parse_expr(parser);

        parser_expect(parser, TOK_SEMICOLON, "expected ';' after compound assignment");

        AstStmt* stmt = ARENA_NEW_ZERO(parser->arena, AstStmt);
        stmt->kind = STMT_COMPOUND_ASSIGN;
        stmt->loc  = loc;
        stmt->compound_assign.op     = op;
        stmt->compound_assign.target = expr;
        stmt->compound_assign.value  = value;

        return stmt;
    }

    parser_expect(parser, TOK_SEMICOLON, "expected ';' after expression statement");

    AstStmt* stmt = ARENA_NEW_ZERO(parser->arena, AstStmt);
    stmt->kind = STMT_EXPR;
    stmt->loc  = loc;
    stmt->expr_stmt.expr = expr;

    return stmt;
}

static AstProc* parse_proc(Parser* parser) {
    SourceLoc loc = parser->current.loc;
    parser_expect(parser, TOK_PROC, "expected 'proc'");

    Token name_tok = parser_expect(parser, TOK_IDENT, "expected procedure name");

    parser_expect(parser, TOK_LPAREN, "expected '(' after procedure name");

    size_t cap = 4;
    size_t param_count = 0;

    AstParam* params = ARENA_NEW_ARRAY(parser->arena, AstParam, cap);

    if (!parser_check(parser, TOK_RPAREN)) {
        while (true) {
            if (param_count >= cap) {
                size_t new_cap = cap * 2;
                params = (AstParam*)arena_realloc(parser->arena, params, 
                                                  cap * sizeof(AstParam), 
                                                  new_cap * sizeof(AstParam));
                cap = new_cap;
            }

            SourceLoc param_loc = parser->current.loc;
            Type* param_type = parse_type(parser);
            Token param_name = parser_expect(parser, TOK_IDENT, "expected parameter name");

            params[param_count].name = param_name.lexeme;
            params[param_count].type = param_type;
            params[param_count].loc  = param_loc;
            param_count++;

            if (!parser_match(parser, TOK_COMMA)) {
                break;
            }
        }
    }

    parser_expect(parser, TOK_RPAREN, "expected ')' after parameter list");

    Type* return_type = type_primitive(TYPE_VOID);

    if (parser_match(parser, TOK_ARROW)) {
        return_type = parse_type(parser);
    }

    AstStmt* body = parse_block(parser);

    AstProc* proc = ARENA_NEW_ZERO(parser->arena, AstProc);
    proc->name             = name_tok.lexeme;
    proc->params           = params;
    proc->param_count      = param_count;
    proc->return_type      = return_type;
    proc->body             = body;
    proc->loc              = loc;
    proc->stack_frame_size = 0;

    return proc;
}

void parser_init(Parser* parser, Lexer* lexer, Arena* arena) {
    parser->lexer     = lexer;
    parser->arena     = arena;
    parser->had_error = false;

    parser_advance(parser);
}

AstProgram* parse_program(Parser* parser) {
    size_t cap = 8;
    size_t count = 0;
    AstProc** procs = ARENA_NEW_ARRAY(parser->arena, AstProc*, cap);

    while (!parser_check(parser, TOK_EOF)) {
        if (count >= cap) {
            size_t new_cap = cap * 2;
            procs = (AstProc**)arena_realloc(parser->arena, procs, 
                                             cap * sizeof(AstProc*), 
                                             new_cap * sizeof(AstProc*));
            cap = new_cap;
        }

        procs[count++] = parse_proc(parser);

        if (parser->had_error) {
            break;
        }
    }

    AstProgram* program = ARENA_NEW_ZERO(parser->arena, AstProgram);
    program->procs      = procs;
    program->proc_count = count;

    return program;
}