#include "parser.h"

#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>

static bool parser_is_file_imported(const Parser* parser, const char* canonical_path) {
    for (ImportedFile* f = parser->imported_files; f != NULL; f = f->next) {
        if (strcmp(f->path, canonical_path) == 0) {
            return true;
        }
    }

    return false;
}

static void parser_mark_file_imported(Parser* parser, const char* canonical_path) {
    ImportedFile* node = ARENA_NEW_ZERO(parser->arena, ImportedFile);

    node->path = arena_strdup(parser->arena, canonical_path);
    node->next = parser->imported_files;

    parser->imported_files = node;
}

static bool canonicalize_path(const char* path, char* out_buf, size_t out_buf_size) {
    (void)out_buf_size;

#if defined(_WIN32) || defined(_WIN64)
    return _fullpath(out_buf, path, PATH_MAX) != NULL;
#else
    return realpath(path, out_buf) != NULL;
#endif
}

static char* resolve_import_path(Arena* arena, const char* current_file, StrView import_rel_path, const char* include_dir) {
    char combined[PATH_MAX];
    char resolved[PATH_MAX];

    const char* last_slash = strrchr(current_file, '/');

#if defined(_WIN32) || defined(_WIN64)
    const char* last_bslash = strrchr(current_file, '\\');
    if (!last_slash || (last_bslash && last_bslash > last_slash)) {
        last_slash = last_bslash;
    }
#endif

    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - current_file + 1);
        snprintf(combined, sizeof(combined), "%.*s%.*s",
                 (int)dir_len, current_file,
                 (int)import_rel_path.len, import_rel_path.data);
    } else {
        snprintf(combined, sizeof(combined), "%.*s",
                 (int)import_rel_path.len, import_rel_path.data);
    }

    if (canonicalize_path(combined, resolved, sizeof(resolved))) {
        return arena_strdup(arena, resolved);
    }

    if (include_dir != NULL) {
        size_t inc_len = strlen(include_dir);
        bool has_slash = (inc_len > 0 && (include_dir[inc_len - 1] == '/' || include_dir[inc_len - 1] == '\\'));

        if (has_slash) {
            snprintf(combined, sizeof(combined), "%s%.*s",
                     include_dir,
                     (int)import_rel_path.len, import_rel_path.data);
        } else {
            snprintf(combined, sizeof(combined), "%s/%.*s",
                     include_dir,
                     (int)import_rel_path.len, import_rel_path.data);
        }

        if (canonicalize_path(combined, resolved, sizeof(resolved))) {
            return arena_strdup(arena, resolved);
        }
    }

    return arena_strdup(arena, combined);
}

static char* read_file_into_arena(Arena* arena, const char* path, size_t* out_len) {
    FILE* file = fopen(path, "rb");

    if (!file) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    size_t size = (size_t)ftell(file);
    fseek(file, 0, SEEK_SET);

    char* buffer = (char*)arena_alloc(arena, size + 1);

    size_t read_bytes = fread(buffer, 1, size, file);
    buffer[read_bytes] = '\0';

    fclose(file);

    if (out_len) {
        *out_len = read_bytes;
    }

    return buffer;
}

static void parser_error_at(Parser* parser, SourceLoc loc, const char* fmt, ...) {
    parser->had_error = true;

    va_list args;
    va_start(args, fmt);
    diag_report_valist(DIAG_ERROR, loc, fmt, args);
    va_end(args);
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

static StrView unescape_string_literal(Arena* arena, StrView raw) {
    if (raw.len >= 2 && raw.data[0] == '"' && raw.data[raw.len - 1] == '"') {
        raw.data += 1;
        raw.len  -= 2;
    }

    char* buffer = (char*)arena_alloc(arena, raw.len + 1);
    size_t write_idx = 0;

    for (size_t i = 0; i < raw.len; ++i) {
        char c = raw.data[i];

        if (c == '\\' && i + 1 < raw.len) {
            i++;
            char next = raw.data[i];

            switch (next) {
                case 'n':  buffer[write_idx++] = '\n'; break;
                case 'r':  buffer[write_idx++] = '\r'; break;
                case 't':  buffer[write_idx++] = '\t'; break;
                case '\\': buffer[write_idx++] = '\\'; break;
                case '\"': buffer[write_idx++] = '\"'; break;
                case '\'': buffer[write_idx++] = '\''; break;
                case '0':  buffer[write_idx++] = '\0'; break;

                case 'x':
                case 'X': {
                    if (i + 2 < raw.len) {
                        char h1 = raw.data[i + 1];
                        char h2 = raw.data[i + 2];
                        int val = 0;
                        bool valid = true;

                        if (h1 >= '0' && h1 <= '9')      val = (h1 - '0') << 4;
                        else if (h1 >= 'a' && h1 <= 'f') val = (h1 - 'a' + 10) << 4;
                        else if (h1 >= 'A' && h1 <= 'F') val = (h1 - 'A' + 10) << 4;
                        else valid = false;

                        if (h2 >= '0' && h2 <= '9')      val |= (h2 - '0');
                        else if (h2 >= 'a' && h2 <= 'f') val |= (h2 - 'a' + 10);
                        else if (h2 >= 'A' && h2 <= 'F') val |= (h2 - 'A' + 10);
                        else valid = false;

                        if (valid) {
                            buffer[write_idx++] = (char)val;
                            i += 2;
                            break;
                        }
                    }
                    buffer[write_idx++] = next;
                    break;
                }

                default:
                    buffer[write_idx++] = next;
                    break;
            }
        } else {
            buffer[write_idx++] = c;
        }
    }

    buffer[write_idx] = '\0';

    return (StrView){ .data = buffer, .len = write_idx };
}

static bool parser_is_named_param(const Parser* parser) {
    if (parser->current.kind != TOK_IDENT) {
        return false;
    }

    size_t cursor = parser->lexer->cursor;

    while (cursor < parser->lexer->source_len) {
        char c = parser->lexer->source[cursor];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            cursor++;
            continue;
        }

        if (c == '/' && cursor + 1 < parser->lexer->source_len) {
            if (parser->lexer->source[cursor + 1] == '/') {
                cursor += 2;
                while (cursor < parser->lexer->source_len && parser->lexer->source[cursor] != '\n') {
                    cursor++;
                }
                continue;
            }
        }

        return (c == ':');
    }

    return false;
}

static Type* parse_type(Parser* parser) {
    Type* base_type = NULL;
    SourceLoc loc = parser->current.loc;

    if (parser_match(parser, TOK_LPAREN)) {
        Type* first_type = parse_type(parser);

        if (parser_match(parser, TOK_COMMA)) {
            size_t cap = 0;
            size_t count = 0;
            Type** types = NULL;

            ARENA_DA_PUSH(parser->arena, types, count, cap, first_type);

            while (true) {
                Type* t = parse_type(parser);
                ARENA_DA_PUSH(parser->arena, types, count, cap, t);

                if (!parser_match(parser, TOK_COMMA)) {
                    break;
                }
            }

            parser_expect(parser, TOK_RPAREN, "expected ')' after tuple types");

            base_type = type_tuple_create(parser->arena, types, count);
        } else {
            parser_expect(parser, TOK_RPAREN, "expected ')' after type");
            base_type = first_type;
        }

        while (parser_match(parser, TOK_STAR)) {
            base_type = type_ptr(parser->arena, base_type);
        }

        return base_type;
    }


    if (parser_match(parser, TOK_LBRACKET)) {
        if (parser_match(parser, TOK_RBRACKET)) {
            Type* elem_type = parse_type(parser);
            return type_slice_create(parser->arena, elem_type);
        }

        Token size_tok = parser_expect(parser, TOK_INT_LIT, "expected array size inside '['");

        int64_t count = parse_int_literal(size_tok.lexeme);
        
        parser_expect(parser, TOK_RBRACKET, "expected ']' after array size");
        
        Type* elem_type = parse_type(parser);

        return type_array_create(parser->arena, elem_type, (size_t)count);
    }

    if (parser_match(parser, TOK_PROC)) {
        parser_expect(parser, TOK_LPAREN, "expected '(' after 'proc'");

        size_t cap = 0;
        size_t count = 0;
        Type** param_types = NULL;

        if (!parser_check(parser, TOK_RPAREN)) {
            while (true) {
                if (parser_is_named_param(parser)) {
                    parser_advance(parser);
                    parser_expect(parser, TOK_COLON, "expected ':' after parameter name");
                }

                Type* pt = parse_type(parser);
                ARENA_DA_PUSH(parser->arena, param_types, count, cap, pt);

                if (!parser_match(parser, TOK_COMMA)) {
                    break;
                }
            }
        }

        parser_expect(parser, TOK_RPAREN, "expected ')' after procedure type parameters");

        Type* return_type = type_primitive(TYPE_VOID);

        if (parser_match(parser, TOK_ARROW)) {
            return_type = parse_type(parser);
        }

        base_type = type_func_create(parser->arena, return_type, param_types, count);

        while (parser_match(parser, TOK_STAR)) {
            base_type = type_ptr(parser->arena, base_type);
        }

        return base_type;
    }

    switch (parser->current.kind) {
        case TOK_U8:   base_type = type_primitive(TYPE_U8);   parser_advance(parser); break;
        case TOK_U16:  base_type = type_primitive(TYPE_U16);  parser_advance(parser); break;
        case TOK_U32:  base_type = type_primitive(TYPE_U32);  parser_advance(parser); break;
        case TOK_U64:  base_type = type_primitive(TYPE_U64);  parser_advance(parser); break;

        case TOK_I8:   base_type = type_primitive(TYPE_I8);   parser_advance(parser); break;
        case TOK_I16:  base_type = type_primitive(TYPE_I16);  parser_advance(parser); break;
        case TOK_I32:  base_type = type_primitive(TYPE_I32);  parser_advance(parser); break;
        case TOK_I64:  base_type = type_primitive(TYPE_I64);  parser_advance(parser); break;

        case TOK_BOOL: base_type = type_primitive(TYPE_BOOL); parser_advance(parser); break;
        case TOK_CHAR: base_type = type_primitive(TYPE_CHAR); parser_advance(parser); break;
        case TOK_VOID: base_type = type_primitive(TYPE_VOID); parser_advance(parser); break;

        case TOK_IDENT: {
            StrView struct_name = parser->current.lexeme;
            parser_advance(parser);

            Type* s_type = ARENA_NEW_ZERO(parser->arena, Type);
            s_type->kind            = TYPE_STRUCT;
            s_type->structure.name  = struct_name;
            s_type->size            = 8;
            s_type->align           = 8;
            base_type = s_type;
            break;
        }

        default:
            parser_error_at(parser, loc, "expected type name");
            parser_advance(parser);
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

            if (parser_match(parser, TOK_DOT_DOT)) {
                AstExpr* end_expr = NULL;

                if (!parser_check(parser, TOK_RBRACKET)) {
                    end_expr = parse_expr_precedence(parser, 0);
                }

                parser_expect(parser, TOK_RBRACKET, "expected ']' after slice expression");
                expr = ast_expr_slice(parser->arena, expr, NULL, end_expr, loc);
                continue;
            }

            AstExpr* first_expr = parse_expr_precedence(parser, 0);

            if (parser_match(parser, TOK_DOT_DOT)) {
                AstExpr* end_expr = NULL;

                if (!parser_check(parser, TOK_RBRACKET)) {
                    end_expr = parse_expr_precedence(parser, 0);
                }

                parser_expect(parser, TOK_RBRACKET, "expected ']' after slice expression");
                expr = ast_expr_slice(parser->arena, expr, first_expr, end_expr, loc);
                continue;
            }

            parser_expect(parser, TOK_RBRACKET, "expected ']' after array index");
            expr = ast_expr_index(parser->arena, expr, first_expr, loc);
            continue;
        }

        if (parser_match(parser, TOK_DOT)) {
            SourceLoc loc = parser->prev.loc;
            Token member_tok = parser_expect(parser, TOK_IDENT, "expected field or method name after '.'");

            if (parser_match(parser, TOK_LPAREN)) {
                size_t cap = 0;
                size_t count = 0;
                AstExpr** args = NULL;

                ARENA_DA_PUSH(parser->arena, args, count, cap, expr);

                if (!parser_check(parser, TOK_RPAREN)) {
                    while (true) {
                        AstExpr* arg = parse_expr_precedence(parser, 0);
                        ARENA_DA_PUSH(parser->arena, args, count, cap, arg);

                        if (!parser_match(parser, TOK_COMMA)) {
                            break;
                        }
                    }
                }

                parser_expect(parser, TOK_RPAREN, "expected ')' after method arguments");

                expr = ast_expr_call(parser->arena, member_tok.lexeme, NULL, args, count, true, loc);
                continue;
            }

            expr = ast_expr_member(parser->arena, expr, member_tok.lexeme, loc);
            continue;
        }

        if (parser_match(parser, TOK_LPAREN)) {
            SourceLoc loc = parser->prev.loc;
            size_t cap = 0;
            size_t count = 0;
            AstExpr** args = NULL;

            if (!parser_check(parser, TOK_RPAREN)) {
                while (true) {
                    AstExpr* arg = parse_expr_precedence(parser, 0);
                    ARENA_DA_PUSH(parser->arena, args, count, cap, arg);

                    if (!parser_match(parser, TOK_COMMA)) {
                        break;
                    }
                }
            }

            parser_expect(parser, TOK_RPAREN, "expected ')' after arguments");

            expr = ast_expr_call(parser->arena, (StrView){0}, expr, args, count, false, loc);
            continue;
        }

        break;
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
        StrView unescaped = unescape_string_literal(parser->arena, parser->prev.lexeme);
        return ast_expr_string_lit(parser->arena, unescaped, loc);
    }

    if (parser_match(parser, TOK_ASM)) {
        bool has_paren = parser_match(parser, TOK_LPAREN);

        Token code_tok = parser_expect(parser, TOK_STRING_LIT, "expected string literal after 'asm'");
        StrView code   = unescape_string_literal(parser->arena, code_tok.lexeme);

        if (has_paren) {
            parser_expect(parser, TOK_RPAREN, "expected ')' after asm code string");
        }

        Type* explicit_type = NULL;

        if (parser_match(parser, TOK_ARROW)) {
            explicit_type = parse_type(parser);
        }

        return ast_expr_asm(parser->arena, code, explicit_type, loc);
    }

    if (parser_match(parser, TOK_CAST)) {
        parser_expect(parser, TOK_LPAREN, "expected '(' after 'cast'");
        Type* target_type = parse_type(parser);
        parser_expect(parser, TOK_COMMA, "expected ',' after cast type");
        AstExpr* inner_expr = parse_expr_precedence(parser, 0);
        parser_expect(parser, TOK_RPAREN, "expected ')' after cast expression");

        expr = ast_expr_cast(parser->arena, target_type, inner_expr, loc);
        return parse_postfix(parser, expr);
    }

    if (parser_match(parser, TOK_SIZEOF)) {
        parser_expect(parser, TOK_LPAREN, "expected '(' after 'sizeof'");
        Type* target_type = parse_type(parser);
        parser_expect(parser, TOK_RPAREN, "expected ')' after sizeof type");

        expr = ast_expr_sizeof(parser->arena, target_type, loc);
        return parse_postfix(parser, expr);
    }

    if (parser_match(parser, TOK_ALIGNOF)) {
        parser_expect(parser, TOK_LPAREN, "expected '(' after 'alignof'");
        Type* target_type = parse_type(parser);
        parser_expect(parser, TOK_RPAREN, "expected ')' after alignof type");

        expr = ast_expr_alignof(parser->arena, target_type, loc);
        return parse_postfix(parser, expr);
    }

    if (parser_match(parser, TOK_OFFSETOF)) {
        parser_expect(parser, TOK_LPAREN, "expected '(' after 'offsetof'");
        Type* struct_type = parse_type(parser);
        parser_expect(parser, TOK_COMMA, "expected ',' after struct type in offsetof");
        Token field_tok = parser_expect(parser, TOK_IDENT, "expected field name in offsetof");
        parser_expect(parser, TOK_RPAREN, "expected ')' after offsetof field name");

        expr = ast_expr_offsetof(parser->arena, struct_type, field_tok.lexeme, loc);
        return parse_postfix(parser, expr);
    }

    if (parser_match(parser, TOK_IDENT)) {
        StrView name = parser->prev.lexeme;

        if (parser_match(parser, TOK_LBRACE)) {
            size_t name_cap = 0;
            size_t name_count = 0;
            StrView* f_names = NULL;

            size_t val_cap = 0;
            size_t val_count = 0;
            AstExpr** f_values = NULL;

            if (!parser_check(parser, TOK_RBRACE)) {
                while (true) {
                    Token f_tok = parser_expect(parser, TOK_IDENT, "expected field name in struct literal");
                    parser_expect(parser, TOK_COLON, "expected ':' after field name");
                    AstExpr* f_val = parse_expr_precedence(parser, 0);

                    ARENA_DA_PUSH(parser->arena, f_names, name_count, name_cap, f_tok.lexeme);
                    ARENA_DA_PUSH(parser->arena, f_values, val_count, val_cap, f_val);

                    if (!parser_match(parser, TOK_COMMA)) {
                        break;
                    }
                }
            }

            parser_expect(parser, TOK_RBRACE, "expected '}' after struct literal");

            expr = ast_expr_struct_lit(parser->arena, name, f_names, f_values, name_count, loc);
            return parse_postfix(parser, expr);
        }

        if (parser_match(parser, TOK_LPAREN)) {
            size_t cap = 0;
            size_t count = 0;
            AstExpr** args = NULL;

            if (!parser_check(parser, TOK_RPAREN)) {
                while (true) {
                    AstExpr* arg = parse_expr_precedence(parser, 0);
                    ARENA_DA_PUSH(parser->arena, args, count, cap, arg);

                    if (!parser_match(parser, TOK_COMMA)) {
                        break;
                    }
                }
            }

            parser_expect(parser, TOK_RPAREN, "expected ')' after argument list");

            expr = ast_expr_call(parser->arena, name, NULL, args, count, false, loc);
            return parse_postfix(parser, expr);
        }

        expr = ast_expr_var(parser->arena, name, loc);
        return parse_postfix(parser, expr);
    }

    if (parser_match(parser, TOK_STAR)  ||
        parser_match(parser, TOK_AMP)   ||
        parser_match(parser, TOK_MINUS) ||
        parser_match(parser, TOK_PLUS)  ||
        parser_match(parser, TOK_TILDE) ||
        parser_match(parser, TOK_BANG)) {

        TokenKind op = parser->prev.kind;
        AstExpr* operand = parse_expr_precedence(parser, 11);

        return ast_expr_unary(parser->arena, op, operand, loc);
    }

    if (parser_match(parser, TOK_LPAREN)) {
        AstExpr* inner = parse_expr_precedence(parser, 0);

        if (parser_match(parser, TOK_COMMA)) {
            size_t cap = 0;
            size_t count = 0;
            AstExpr** elements = NULL;

            ARENA_DA_PUSH(parser->arena, elements, count, cap, inner);

            while (true) {
                AstExpr* elem = parse_expr_precedence(parser, 0);
                ARENA_DA_PUSH(parser->arena, elements, count, cap, elem);

                if (!parser_match(parser, TOK_COMMA)) {
                    break;
                }
            }

            parser_expect(parser, TOK_RPAREN, "expected ')' after tuple elements");

            expr = ast_expr_tuple(parser->arena, elements, count, loc);
            return parse_postfix(parser, expr);
        }

        parser_expect(parser, TOK_RPAREN, "expected ')' after expression");

        return parse_postfix(parser, inner);
    }

    parser_error_at(parser, loc, "expected expression");
    parser_advance(parser);

    return ast_expr_int_lit(parser->arena, 0, loc);
}

static int get_binary_precedence(TokenKind kind) {
    switch (kind) {
        case TOK_PIPE_PIPE: return 1;
        case TOK_AMP_AMP:   return 2;
        case TOK_PIPE:      return 3;
        case TOK_CARET:     return 4;
        case TOK_AMP:       return 5;
        case TOK_EQ_EQ:
        case TOK_BANG_EQ:   return 6;
        case TOK_LESS:
        case TOK_LESS_EQ:
        case TOK_GREATER:
        case TOK_GREATER_EQ: return 7;
        case TOK_SHL:
        case TOK_SHR:       return 8;
        case TOK_PLUS:
        case TOK_MINUS:     return 9;
        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT:   return 10;
        default:            return 0;
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

    size_t cap = 0;
    size_t count = 0;
    AstStmt** stmts = NULL;

    while (!parser_check(parser, TOK_RBRACE) && !parser_check(parser, TOK_EOF)) {
        AstStmt* stmt = parse_stmt(parser);
        ARENA_DA_PUSH(parser->arena, stmts, count, cap, stmt);
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
        if (parser_match(parser, TOK_LPAREN)) {
            size_t name_cap = 0;
            size_t name_count = 0;
            StrView* names = NULL;

            size_t type_cap = 0;
            size_t type_count = 0;
            Type** declared_types = NULL;

            while (true) {
                Token name_tok = parser_expect(parser, TOK_IDENT, "expected variable name or '_' in destructuring");
                Type* t = NULL;

                if (parser_match(parser, TOK_COLON)) {
                    t = parse_type(parser);
                }

                ARENA_DA_PUSH(parser->arena, names, name_count, name_cap, name_tok.lexeme);
                ARENA_DA_PUSH(parser->arena, declared_types, type_count, type_cap, t);

                if (!parser_match(parser, TOK_COMMA)) {
                    break;
                }
            }

            parser_expect(parser, TOK_RPAREN, "expected ')' after destructuring variable list");
            parser_expect(parser, TOK_EQ, "expected '=' in destructuring declaration");
            AstExpr* init_expr = parse_expr(parser);
            parser_expect(parser, TOK_SEMICOLON, "expected ';' after destructuring declaration");

            AstStmt* stmt = ARENA_NEW_ZERO(parser->arena, AstStmt);
            stmt->kind = STMT_DESTRUCTURE_DECL;
            stmt->loc  = loc;
            stmt->destructure_decl.names          = names;
            stmt->destructure_decl.declared_types = declared_types;
            stmt->destructure_decl.symbols        = ARENA_NEW_ARRAY_ZERO(parser->arena, Symbol*, name_count);
            stmt->destructure_decl.count          = name_count;
            stmt->destructure_decl.init_expr      = init_expr;

            return stmt;
        }

        Token name_tok = parser_expect(parser, TOK_IDENT, "expected variable name after 'var'");
        Type* declared_type = NULL;

        if (parser_match(parser, TOK_COLON)) {
            declared_type = parse_type(parser);
        }

        AstExpr* init_expr = NULL;

        if (parser_match(parser, TOK_EQ)) {
            init_expr = parse_expr(parser);
        } else if (!declared_type) {
            parser_error_at(parser, name_tok.loc, "variable declaration without type must have an initializer");
        }

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

    if (parser_match(parser, TOK_BREAK)) {
        parser_expect(parser, TOK_SEMICOLON, "expected ';' after 'break'");
        return ast_stmt_break(parser->arena, loc);
    }

    if (parser_match(parser, TOK_CONTINUE)) {
        parser_expect(parser, TOK_SEMICOLON, "expected ';' after 'continue'");
        return ast_stmt_continue(parser->arena, loc);
    }

    if (parser_match(parser, TOK_DEFER)) {
        SourceLoc defer_loc = parser->prev.loc;
        AstStmt* deferred_stmt = parse_stmt(parser);

        return ast_stmt_defer(parser->arena, deferred_stmt, defer_loc);
    }

    if (parser_match(parser, TOK_SWITCH)) {
        SourceLoc switch_loc = parser->prev.loc;

        parser_expect(parser, TOK_LPAREN, "expected '(' after 'switch'");
        AstExpr* cond = parse_expr(parser);
        parser_expect(parser, TOK_RPAREN, "expected ')' after switch condition");

        parser_expect(parser, TOK_LBRACE, "expected '{' after switch condition");

        size_t case_cap = 0;
        size_t case_count = 0;
        AstSwitchCase* cases = NULL;

        while (!parser_check(parser, TOK_RBRACE) && !parser_check(parser, TOK_EOF)) {
            bool is_default = false;
            size_t val_cap = 0;
            size_t val_count = 0;
            AstExpr** values = NULL;
            SourceLoc case_loc = parser->current.loc;

            if (parser_match(parser, TOK_CASE)) {
                while (true) {
                    AstExpr* val_expr = parse_expr(parser);
                    ARENA_DA_PUSH(parser->arena, values, val_count, val_cap, val_expr);

                    if (!parser_match(parser, TOK_COMMA)) {
                        break;
                    }
                }

                parser_expect(parser, TOK_COLON, "expected ':' after case value");
            } else if (parser_match(parser, TOK_DEFAULT)) {
                is_default = true;
                parser_expect(parser, TOK_COLON, "expected ':' after 'default'");
            } else {
                parser_error_at(parser, parser->current.loc, "expected 'case' or 'default' inside switch");
                parser_advance(parser);
                continue;
            }

            size_t stmt_cap = 0;
            size_t stmt_count = 0;
            AstStmt** stmts = NULL;

            while (!parser_check(parser, TOK_CASE) &&
                   !parser_check(parser, TOK_DEFAULT) &&
                   !parser_check(parser, TOK_RBRACE) &&
                   !parser_check(parser, TOK_EOF)) {

                AstStmt* s = parse_stmt(parser);
                ARENA_DA_PUSH(parser->arena, stmts, stmt_count, stmt_cap, s);
            }

            AstSwitchCase c = {
                .values       = values,
                .const_values = NULL,
                .value_count  = val_count,
                .stmts        = stmts,
                .stmt_count   = stmt_count,
                .is_default   = is_default,
                .loc          = case_loc
            };

            ARENA_DA_PUSH(parser->arena, cases, case_count, case_cap, c);
        }

        parser_expect(parser, TOK_RBRACE, "expected '}' after switch body");

        return ast_stmt_switch(parser->arena, cond, cases, case_count, switch_loc);
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

    if (parser_match(parser, TOK_FOR)) {
        SourceLoc for_loc = parser->prev.loc;
        parser_expect(parser, TOK_LPAREN, "expected '(' after 'for'");

        AstStmt* init_stmt = NULL;

        if (!parser_match(parser, TOK_SEMICOLON)) {
            if (parser_match(parser, TOK_VAR)) {
                SourceLoc var_loc = parser->prev.loc;
                Token name_tok = parser_expect(parser, TOK_IDENT, "expected variable name after 'var'");
                Type* declared_type = NULL;

                if (parser_match(parser, TOK_COLON)) {
                    declared_type = parse_type(parser);
                }

                AstExpr* init_expr = NULL;

                if (parser_match(parser, TOK_EQ)) {
                    init_expr = parse_expr(parser);
                } else if (!declared_type) {
                    parser_error_at(parser, name_tok.loc, "variable declaration without type must have an initializer");
                }

                parser_expect(parser, TOK_SEMICOLON, "expected ';' after variable declaration in for loop");

                AstStmt* s = ARENA_NEW_ZERO(parser->arena, AstStmt);
                s->kind = STMT_VAR_DECL;
                s->loc  = var_loc;
                s->var_decl.name          = name_tok.lexeme;
                s->var_decl.declared_type = declared_type;
                s->var_decl.init_expr     = init_expr;

                init_stmt = s;
            } else {
                AstExpr* expr = parse_expr(parser);

                if (parser_match(parser, TOK_EQ)) {
                    AstExpr* value = parse_expr(parser);
                    parser_expect(parser, TOK_SEMICOLON, "expected ';' after assignment in for loop");

                    AstStmt* s = ARENA_NEW_ZERO(parser->arena, AstStmt);
                    s->kind = STMT_ASSIGN;
                    s->loc  = expr->loc;
                    s->assign.target = expr;
                    s->assign.value  = value;

                    init_stmt = s;
                } else if (parser_match(parser, TOK_PLUS_EQ)   ||
                           parser_match(parser, TOK_MINUS_EQ)  ||
                           parser_match(parser, TOK_STAR_EQ)   ||
                           parser_match(parser, TOK_SLASH_EQ)  ||
                           parser_match(parser, TOK_PERCENT_EQ)||
                           parser_match(parser, TOK_AMP_EQ)    ||
                           parser_match(parser, TOK_PIPE_EQ)   ||
                           parser_match(parser, TOK_CARET_EQ)  ||
                           parser_match(parser, TOK_SHL_EQ)    ||
                           parser_match(parser, TOK_SHR_EQ)) {

                    TokenKind op = parser->prev.kind;
                    AstExpr* value = parse_expr(parser);
                    parser_expect(parser, TOK_SEMICOLON, "expected ';' after compound assignment in for loop");

                    AstStmt* s = ARENA_NEW_ZERO(parser->arena, AstStmt);
                    s->kind = STMT_COMPOUND_ASSIGN;
                    s->loc  = expr->loc;
                    s->compound_assign.op     = op;
                    s->compound_assign.target = expr;
                    s->compound_assign.value  = value;

                    init_stmt = s;
                } else {
                    parser_expect(parser, TOK_SEMICOLON, "expected ';' after for loop init expression");

                    AstStmt* s = ARENA_NEW_ZERO(parser->arena, AstStmt);
                    s->kind = STMT_EXPR;
                    s->loc  = expr->loc;
                    s->expr_stmt.expr = expr;

                    init_stmt = s;
                }
            }
        }

        AstExpr* cond_expr = NULL;

        if (!parser_match(parser, TOK_SEMICOLON)) {
            cond_expr = parse_expr(parser);
            parser_expect(parser, TOK_SEMICOLON, "expected ';' after for loop condition");
        }

        AstStmt* step_stmt = NULL;

        if (!parser_check(parser, TOK_RPAREN)) {
            AstExpr* expr = parse_expr(parser);

            if (parser_match(parser, TOK_EQ)) {
                AstExpr* value = parse_expr(parser);

                AstStmt* s = ARENA_NEW_ZERO(parser->arena, AstStmt);
                s->kind = STMT_ASSIGN;
                s->loc  = expr->loc;
                s->assign.target = expr;
                s->assign.value  = value;

                step_stmt = s;
            } else if (parser_match(parser, TOK_PLUS_EQ)   ||
                       parser_match(parser, TOK_MINUS_EQ)  ||
                       parser_match(parser, TOK_STAR_EQ)   ||
                       parser_match(parser, TOK_SLASH_EQ)  ||
                       parser_match(parser, TOK_PERCENT_EQ)||
                       parser_match(parser, TOK_AMP_EQ)    ||
                       parser_match(parser, TOK_PIPE_EQ)   ||
                       parser_match(parser, TOK_CARET_EQ)  ||
                       parser_match(parser, TOK_SHL_EQ)    ||
                       parser_match(parser, TOK_SHR_EQ)) {

                TokenKind op = parser->prev.kind;
                AstExpr* value = parse_expr(parser);

                AstStmt* s = ARENA_NEW_ZERO(parser->arena, AstStmt);
                s->kind = STMT_COMPOUND_ASSIGN;
                s->loc  = expr->loc;
                s->compound_assign.op     = op;
                s->compound_assign.target = expr;
                s->compound_assign.value  = value;

                step_stmt = s;
            } else {
                AstStmt* s = ARENA_NEW_ZERO(parser->arena, AstStmt);
                s->kind = STMT_EXPR;
                s->loc  = expr->loc;
                s->expr_stmt.expr = expr;

                step_stmt = s;
            }
        }

        parser_expect(parser, TOK_RPAREN, "expected ')' after for clauses");

        AstStmt* body = parse_stmt(parser);

        return ast_stmt_for(parser->arena, init_stmt, cond_expr, step_stmt, body, for_loc);
    }

    AstExpr* expr = parse_expr(parser);

    if (expr->kind == EXPR_TUPLE && parser_match(parser, TOK_EQ)) {
        AstExpr* value = parse_expr(parser);
        parser_expect(parser, TOK_SEMICOLON, "expected ';' after destructuring assignment");

        AstStmt* stmt = ARENA_NEW_ZERO(parser->arena, AstStmt);
        stmt->kind = STMT_DESTRUCTURE_ASSIGN;
        stmt->loc  = loc;
        stmt->destructure_assign.targets = expr->tuple.elements;
        stmt->destructure_assign.count   = expr->tuple.count;
        stmt->destructure_assign.value   = value;

        return stmt;
    }

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

    if (parser_match(parser, TOK_PLUS_EQ)   ||
        parser_match(parser, TOK_MINUS_EQ)  ||
        parser_match(parser, TOK_STAR_EQ)   ||
        parser_match(parser, TOK_SLASH_EQ)  ||
        parser_match(parser, TOK_PERCENT_EQ)||
        parser_match(parser, TOK_AMP_EQ)    ||
        parser_match(parser, TOK_PIPE_EQ)   ||
        parser_match(parser, TOK_CARET_EQ)  ||
        parser_match(parser, TOK_SHL_EQ)    ||
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

static AstProc* parse_proc(Parser* parser, StrView method_struct) {
    SourceLoc loc = parser->current.loc;
    parser_expect(parser, TOK_PROC, "expected 'proc'");

    Token name_tok = parser_expect(parser, TOK_IDENT, "expected procedure name");

    parser_expect(parser, TOK_LPAREN, "expected '(' after procedure name");

    size_t cap = 0;
    size_t param_count = 0;
    AstParam* params = NULL;

    if (!parser_check(parser, TOK_RPAREN)) {
        while (true) {
            SourceLoc param_loc = parser->current.loc;
            Token param_name = parser_expect(parser, TOK_IDENT, "expected parameter name");
            parser_expect(parser, TOK_COLON, "expected ':' after parameter name");
            Type* param_type = parse_type(parser);

            AstParam param = {
                .name = param_name.lexeme,
                .type = param_type,
                .loc  = param_loc
            };
            ARENA_DA_PUSH(parser->arena, params, param_count, cap, param);

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

    StrView final_name = name_tok.lexeme;

    if (method_struct.len > 0) {
        char* mangled = arena_sprintf(parser->arena, "%.*s_%.*s",
                                      (int)method_struct.len, method_struct.data,
                                      (int)name_tok.lexeme.len, name_tok.lexeme.data);
        final_name = (StrView){ .data = mangled, .len = strlen(mangled) };
    }

    AstProc* proc = ARENA_NEW_ZERO(parser->arena, AstProc);
    proc->name          = final_name;
    proc->method_struct = method_struct;
    proc->params        = params;
    proc->param_count   = param_count;
    proc->return_type   = return_type;
    proc->body          = body;
    proc->loc           = loc;
    proc->symbol        = NULL;

    return proc;
}

void parser_init(Parser* parser, Lexer* lexer, Arena* arena, const char* include_dir) {
    parser->lexer          = lexer;
    parser->arena          = arena;
    parser->imported_files = NULL;
    parser->include_dir    = include_dir;
    parser->had_error      = false;

    char resolved[PATH_MAX];
    if (canonicalize_path(lexer->filename, resolved, sizeof(resolved))) {
        parser_mark_file_imported(parser, resolved);
    } else {
        parser_mark_file_imported(parser, lexer->filename);
    }

    parser_advance(parser);
}

typedef struct ProgramBuilder {
    AstConstDef**     consts;
    size_t            const_count;
    size_t            const_cap;

    AstTypeDef**      typedefs;
    size_t            typedef_count;
    size_t            typedef_cap;

    AstGlobalVarDef** globals;
    size_t            global_count;
    size_t            global_cap;

    AstStructDef**    structs;
    size_t            struct_count;
    size_t            struct_cap;

    AstEnumDef**      enums;
    size_t            enum_count;
    size_t            enum_cap;

    AstProc**         procs;
    size_t            proc_count;
    size_t            proc_cap;
} ProgramBuilder;

static AstEnumDef*   parse_enum_declaration(Parser* parser);
static AstStructDef* parse_struct_declaration(Parser* parser, bool is_packed, ProgramBuilder* b);
static void          parse_file_declarations(Parser* parser, ProgramBuilder* b);
static void          parse_top_level_declaration(Parser* parser, ProgramBuilder* b);
static void          parse_import_statement(Parser* parser, ProgramBuilder* b);

static void parse_import_statement(Parser* parser, ProgramBuilder* b) {
    SourceLoc import_loc = parser->current.loc;
    parser_advance(parser);

    Token path_tok = parser_expect(parser, TOK_STRING_LIT, "expected string literal after 'import'");
    parser_expect(parser, TOK_SEMICOLON, "expected ';' after import statement");

    StrView raw_path = unescape_string_literal(parser->arena, path_tok.lexeme);
    char* resolved_path = resolve_import_path(parser->arena, import_loc.filename, raw_path, parser->include_dir);

    if (parser_is_file_imported(parser, resolved_path)) {
        return;
    }
    parser_mark_file_imported(parser, resolved_path);

    size_t file_len = 0;
    char* file_content = read_file_into_arena(parser->arena, resolved_path, &file_len);

    if (!file_content) {
        parser_error_at(parser, import_loc, "cannot open imported file '%s'", resolved_path);
        return;
    }

    Lexer* parent_lexer   = parser->lexer;
    Token  parent_current = parser->current;
    Token  parent_prev    = parser->prev;

    Lexer sub_lexer;
    lexer_init(&sub_lexer, file_content, file_len, resolved_path);
    parser->lexer = &sub_lexer;
    parser_advance(parser);

    parse_file_declarations(parser, b);

    parser->lexer   = parent_lexer;
    parser->current = parent_current;
    parser->prev    = parent_prev;
}

static void parse_top_level_declaration(Parser* parser, ProgramBuilder* b) {
    if (parser_check(parser, TOK_IMPORT)) {
        parse_import_statement(parser, b);
        return;
    }

    if (parser_match(parser, TOK_TYPE)) {
        SourceLoc loc = parser->prev.loc;

        Token name_tok = parser_expect(parser, TOK_IDENT, "expected type alias name");
        
        parser_expect(parser, TOK_EQ, "expected '=' in type alias declaration");
        
        Type* target_type = parse_type(parser);
        
        parser_expect(parser, TOK_SEMICOLON, "expected ';' after type alias declaration");

        AstTypeDef* td = ARENA_NEW_ZERO(parser->arena, AstTypeDef);
        td->name        = name_tok.lexeme;
        td->target_type = target_type;
        td->loc         = loc;
        td->symbol      = NULL;

        ARENA_DA_PUSH(parser->arena, b->typedefs, b->typedef_count, b->typedef_cap, td);
        return;
    }

    if (parser_match(parser, TOK_CONST)) {
        SourceLoc loc = parser->prev.loc;
        Token name_tok = parser_expect(parser, TOK_IDENT, "expected constant name after 'const'");

        Type* c_type = NULL;
        if (parser_match(parser, TOK_COLON)) {
            c_type = parse_type(parser);
        }

        parser_expect(parser, TOK_EQ, "expected '=' in const declaration");

        bool is_neg = parser_match(parser, TOK_MINUS);
        Token val_tok = parser_expect(parser, TOK_INT_LIT, "expected integer literal for const value");
        int64_t val = parse_int_literal(val_tok.lexeme);
        if (is_neg) val = -val;

        parser_expect(parser, TOK_SEMICOLON, "expected ';' after const declaration");

        AstConstDef* cd = ARENA_NEW_ZERO(parser->arena, AstConstDef);
        cd->name   = name_tok.lexeme;
        cd->type   = c_type ? c_type : type_primitive(TYPE_I64);
        cd->val    = val;
        cd->loc    = loc;
        cd->symbol = NULL;

        ARENA_DA_PUSH(parser->arena, b->consts, b->const_count, b->const_cap, cd);
        return;
    }

    if (parser_match(parser, TOK_VAR)) {
        SourceLoc loc = parser->prev.loc;
        Token name_tok = parser_expect(parser, TOK_IDENT, "expected variable name after 'var'");

        Type* g_type = NULL;
        if (parser_match(parser, TOK_COLON)) {
            g_type = parse_type(parser);
        }

        AstExpr* init_expr = NULL;
        if (parser_match(parser, TOK_EQ)) {
            init_expr = parse_expr(parser);
        }

        parser_expect(parser, TOK_SEMICOLON, "expected ';' after global var declaration");

        AstGlobalVarDef* gd = ARENA_NEW_ZERO(parser->arena, AstGlobalVarDef);
        gd->name      = name_tok.lexeme;
        gd->type      = g_type;
        gd->init_expr = init_expr;
        gd->loc       = loc;
        gd->symbol    = NULL;

        ARENA_DA_PUSH(parser->arena, b->globals, b->global_count, b->global_cap, gd);
        return;
    }

    bool is_packed = false;
    if (parser_match(parser, TOK_PACKED)) {
        is_packed = true;
        parser_expect(parser, TOK_STRUCT, "expected 'struct' after 'packed'");
    }

    if (is_packed || parser_match(parser, TOK_STRUCT)) {
        AstStructDef* sd = parse_struct_declaration(parser, is_packed, b);
        ARENA_DA_PUSH(parser->arena, b->structs, b->struct_count, b->struct_cap, sd);
        return;
    }

    if (parser_match(parser, TOK_ENUM)) {
        AstEnumDef* ed = parse_enum_declaration(parser);
        ARENA_DA_PUSH(parser->arena, b->enums, b->enum_count, b->enum_cap, ed);
        return;
    }

    if (parser_check(parser, TOK_PROC)) {
        AstProc* p = parse_proc(parser, (StrView){ .data = NULL, .len = 0 });
        ARENA_DA_PUSH(parser->arena, b->procs, b->proc_count, b->proc_cap, p);
        return;
    }

    parser_error_at(parser, parser->current.loc, "expected top-level declaration (proc, struct, const, var, import)");
    parser_advance(parser);
}

static void parse_file_declarations(Parser* parser, ProgramBuilder* b) {
    while (!parser_check(parser, TOK_EOF)) {
        parse_top_level_declaration(parser, b);
        if (parser->had_error) {
            break;
        }
    }
}

static AstEnumDef* parse_enum_declaration(Parser* parser) {
    SourceLoc loc = parser->prev.loc;
    Token name_tok = parser_expect(parser, TOK_IDENT, "expected enum name");

    Type* underlying_type = NULL;

    if (parser_match(parser, TOK_COLON)) {
        underlying_type = parse_type(parser);
    }

    parser_expect(parser, TOK_LBRACE, "expected '{' after enum name");

    size_t v_cap = 0;
    size_t v_count = 0;
    AstEnumVariantDef* variants = NULL;

    while (!parser_check(parser, TOK_RBRACE) && !parser_check(parser, TOK_EOF)) {
        Token v_name = parser_expect(parser, TOK_IDENT, "expected variant name in enum");
        AstExpr* explicit_val = NULL;

        if (parser_match(parser, TOK_EQ)) {
            explicit_val = parse_expr(parser);
        }

        AstEnumVariantDef variant = {
            .name           = v_name.lexeme,
            .explicit_value = explicit_val,
            .loc            = v_name.loc
        };

        ARENA_DA_PUSH(parser->arena, variants, v_count, v_cap, variant);

        if (!parser_match(parser, TOK_COMMA)) {
            break;
        }
    }

    parser_expect(parser, TOK_RBRACE, "expected '}' after enum body");

    AstEnumDef* e_def = ARENA_NEW_ZERO(parser->arena, AstEnumDef);

    e_def->name            = name_tok.lexeme;
    e_def->underlying_type = underlying_type;
    e_def->variants        = variants;
    e_def->variant_count   = v_count;
    e_def->loc             = loc;
    e_def->type            = type_enum_create(parser->arena, name_tok.lexeme, underlying_type, NULL, 0);

    return e_def;
}

static AstStructDef* parse_struct_declaration(Parser* parser, bool is_packed, ProgramBuilder* b) {
    SourceLoc loc = parser->current.loc;
    Token name_tok = parser_expect(parser, TOK_IDENT, "expected struct name");
    parser_expect(parser, TOK_LBRACE, "expected '{' after struct name");

    size_t f_cap = 0;
    size_t f_count = 0;
    StructField* fields = NULL;

    while (!parser_check(parser, TOK_RBRACE) && !parser_check(parser, TOK_EOF)) {
        if (parser_check(parser, TOK_PROC)) {
            AstProc* method = parse_proc(parser, name_tok.lexeme);
            ARENA_DA_PUSH(parser->arena, b->procs, b->proc_count, b->proc_cap, method);
            continue;
        }

        Token field_name = parser_expect(parser, TOK_IDENT, "expected field name in struct");
        parser_expect(parser, TOK_COLON, "expected ':' after field name");
        Type* field_type = parse_type(parser);
        AstExpr* default_val = NULL;

        if (parser_match(parser, TOK_EQ)) {
            default_val = parse_expr_precedence(parser, 0);
        }

        StructField field = {
            .name          = field_name.lexeme,
            .type          = field_type,
            .offset        = 0,
            .default_value = default_val
        };
        ARENA_DA_PUSH(parser->arena, fields, f_count, f_cap, field);

        parser_match(parser, TOK_COMMA);
    }

    parser_expect(parser, TOK_RBRACE, "expected '}' after struct body");

    AstStructDef* s_def = ARENA_NEW_ZERO(parser->arena, AstStructDef);
    s_def->name        = name_tok.lexeme;
    s_def->fields      = fields;
    s_def->field_count = f_count;
    s_def->is_packed   = is_packed;
    s_def->loc         = loc;
    s_def->type        = type_struct_create(parser->arena, name_tok.lexeme, fields, f_count, is_packed);

    return s_def;
}

AstProgram* parse_program(Parser* parser) {
    ProgramBuilder b = {
        .consts        = NULL,
        .const_count   = 0,
        .const_cap     = 0,

        .typedefs      = NULL,
        .typedef_count = 0,
        .typedef_cap   = 0,

        .globals       = NULL,
        .global_count  = 0,
        .global_cap    = 0,

        .structs       = NULL,
        .struct_count  = 0,
        .struct_cap    = 0,

        .enums         = NULL,
        .enum_count    = 0,
        .enum_cap      = 0,

        .procs         = NULL,
        .proc_count    = 0,
        .proc_cap      = 0,
    };

    parse_file_declarations(parser, &b);

    AstProgram* program   = ARENA_NEW_ZERO(parser->arena, AstProgram);
    program->consts       = b.consts;
    program->const_count  = b.const_count;
    program->typedefs     = b.typedefs;
    program->typedef_count = b.typedef_count;
    program->globals      = b.globals;
    program->global_count = b.global_count;
    program->structs      = b.structs;
    program->struct_count = b.struct_count;
    program->enums        = b.enums;
    program->enum_count   = b.enum_count;
    program->procs        = b.procs;
    program->proc_count   = b.proc_count;

    return program;
}