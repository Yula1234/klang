#include "lexer.h"

#include <ctype.h>
#include <string.h>

static inline bool is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static inline bool is_ident_part(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static inline bool is_digit(char c) {
    return isdigit((unsigned char)c);
}

static inline bool is_hex_digit(char c) {
    return isxdigit((unsigned char)c);
}

static inline bool lexer_is_eof(const Lexer* lexer) {
    return lexer->cursor >= lexer->source_len;
}

static inline char lexer_peek(const Lexer* lexer) {
    if (lexer_is_eof(lexer)) {
        return '\0';
    }

    return lexer->source[lexer->cursor];
}

static inline char lexer_peek_next(const Lexer* lexer) {
    if (lexer->cursor + 1 >= lexer->source_len) {
        return '\0';
    }

    return lexer->source[lexer->cursor + 1];
}

static char lexer_advance(Lexer* lexer) {
    if (lexer_is_eof(lexer)) {
        return '\0';
    }

    char c = lexer->source[lexer->cursor++];

    lexer->col++;

    return c;
}

static bool lexer_match(Lexer* lexer, char expected) {
    if (lexer_is_eof(lexer) || lexer->source[lexer->cursor] != expected) {
        return false;
    }

    lexer->cursor++;
    lexer->col++;

    return true;
}

static void lexer_skip_whitespace_and_comments(Lexer* lexer) {
    while (!lexer_is_eof(lexer)) {
        char c = lexer_peek(lexer);

        if (c == ' ' || c == '\t' || c == '\r') {
            lexer_advance(lexer);
            continue;
        }

        if (c == '\n') {
            lexer->line++;
            lexer->col = 1;
            lexer->cursor++;
            continue;
        }

        if (c == '/') {
            char next = lexer_peek_next(lexer);

            if (next == '/') {
                lexer_advance(lexer);
                lexer_advance(lexer);

                while (!lexer_is_eof(lexer) && lexer_peek(lexer) != '\n') {
                    lexer_advance(lexer);
                }
                
                continue;
            }

            if (next == '*') {
                lexer_advance(lexer);
                lexer_advance(lexer);

                int depth = 1;

                while (!lexer_is_eof(lexer) && depth > 0) {
                    char cur = lexer_peek(lexer);

                    if (cur == '\n') {
                        lexer->line++;
                        lexer->col = 0;
                    }

                    if (cur == '/' && lexer_peek_next(lexer) == '*') {
                        lexer_advance(lexer);
                        lexer_advance(lexer);
                        depth++;
                        continue;
                    }

                    if (cur == '*' && lexer_peek_next(lexer) == '/') {
                        lexer_advance(lexer);
                        lexer_advance(lexer);
                        depth--;
                        continue;
                    }

                    lexer_advance(lexer);
                }

                continue;
            }
        }

        break;
    }
}

static TokenKind check_keyword(const char* text, size_t len) {
    switch (len) {
        case 2:
            if (memcmp(text, "if", 2) == 0)  return TOK_IF;
            if (memcmp(text, "i8", 2) == 0)  return TOK_I8;
            if (memcmp(text, "u8", 2) == 0)  return TOK_U8;
            break;

        case 3:
            if (memcmp(text, "var", 3) == 0) return TOK_VAR;
            if (memcmp(text, "asm", 3) == 0) return TOK_ASM;
            if (memcmp(text, "i16", 3) == 0) return TOK_I16;
            if (memcmp(text, "i32", 3) == 0) return TOK_I32;
            if (memcmp(text, "i64", 3) == 0) return TOK_I64;
            if (memcmp(text, "u16", 3) == 0) return TOK_U16;
            if (memcmp(text, "u32", 3) == 0) return TOK_U32;
            if (memcmp(text, "u64", 3) == 0) return TOK_U64;
            break;

        case 4:
            if (memcmp(text, "proc", 4) == 0) return TOK_PROC;
            if (memcmp(text, "else", 4) == 0) return TOK_ELSE;
            if (memcmp(text, "char", 4) == 0) return TOK_CHAR;
            if (memcmp(text, "void", 4) == 0) return TOK_VOID;
            if (memcmp(text, "bool", 4) == 0) return TOK_BOOL;
            if (memcmp(text, "cast", 4) == 0) return TOK_CAST;
            break;

        case 5:
            if (memcmp(text, "while", 5) == 0) return TOK_WHILE;
            if (memcmp(text, "break", 5) == 0) return TOK_BREAK;
            if (memcmp(text, "const", 5) == 0) return TOK_CONST;
            break;

        case 6:
            if (memcmp(text, "return", 6) == 0) return TOK_RETURN;
            if (memcmp(text, "import", 6) == 0) return TOK_IMPORT;
            if (memcmp(text, "struct", 6) == 0) return TOK_STRUCT;
            if (memcmp(text, "packed", 6) == 0) return TOK_PACKED;
            break;

        case 8:
            if (memcmp(text, "continue", 8) == 0) return TOK_CONTINUE;
            break;
    }

    return TOK_IDENT;
}

static Token lexer_ident_or_keyword(Lexer* lexer, size_t start_pos, SourceLoc loc) {
    while (!lexer_is_eof(lexer) && is_ident_part(lexer_peek(lexer))) {
        lexer_advance(lexer);
    }

    size_t len = lexer->cursor - start_pos;
    const char* text = lexer->source + start_pos;
    
    TokenKind kind = check_keyword(text, len);

    return (Token){
        .kind   = kind,
        .lexeme = (StrView){ .data = text, .len = len },
        .loc    = loc
    };
}

static Token lexer_number(Lexer* lexer, size_t start_pos, SourceLoc loc) {
    char first = lexer->source[start_pos];

    if (first == '0' && !lexer_is_eof(lexer)) {
        char next = lexer_peek(lexer);

        if (next == 'x' || next == 'X') {
            lexer_advance(lexer);

            while (!lexer_is_eof(lexer)) {
                char c = lexer_peek(lexer);

                if (is_hex_digit(c) || c == '_') {
                    lexer_advance(lexer);
                } else {
                    break;
                }
            }

            size_t len = lexer->cursor - start_pos;

            return (Token){
                .kind   = TOK_INT_LIT,
                .lexeme = (StrView){ .data = lexer->source + start_pos, .len = len },
                .loc    = loc
            };
        }

        if (next == 'b' || next == 'B') {
            lexer_advance(lexer);

            while (!lexer_is_eof(lexer)) {
                char c = lexer_peek(lexer);

                if (c == '0' || c == '1' || c == '_') {
                    lexer_advance(lexer);
                } else {
                    break;
                }
            }

            size_t len = lexer->cursor - start_pos;

            return (Token){
                .kind   = TOK_INT_LIT,
                .lexeme = (StrView){ .data = lexer->source + start_pos, .len = len },
                .loc    = loc
            };
        }
    }

    while (!lexer_is_eof(lexer)) {
        char c = lexer_peek(lexer);

        if (is_digit(c) || c == '_') {
            lexer_advance(lexer);
        } else {
            break;
        }
    }

    size_t len = lexer->cursor - start_pos;

    return (Token){
        .kind   = TOK_INT_LIT,
        .lexeme = (StrView){ .data = lexer->source + start_pos, .len = len },
        .loc    = loc
    };
}

static Token lexer_string(Lexer* lexer, size_t start_pos, SourceLoc loc) {
    while (!lexer_is_eof(lexer) && lexer_peek(lexer) != '"') {
        char c = lexer_peek(lexer);

        if (c == '\n') {
            lexer->line++;
            lexer->col = 0;
        }

        if (c == '\\' && lexer->cursor + 1 < lexer->source_len) {
            lexer_advance(lexer);
        }

        lexer_advance(lexer);
    }

    if (lexer_is_eof(lexer)) {
        return (Token){
            .kind   = TOK_ERROR,
            .lexeme = (StrView){ .data = "Unterminated string literal", .len = 27 },
            .loc    = loc
        };
    }

    lexer_advance(lexer);

    size_t len = lexer->cursor - start_pos;

    return (Token){
        .kind   = TOK_STRING_LIT,
        .lexeme = (StrView){ .data = lexer->source + start_pos, .len = len },
        .loc    = loc
    };
}

void lexer_init(Lexer* lexer, const char* source, size_t source_len, const char* filename) {
    lexer->source     = source;
    lexer->source_len = source_len;
    lexer->filename   = filename;
    lexer->cursor     = 0;
    lexer->line       = 1;
    lexer->col        = 1;
}

Token lexer_next_token(Lexer* lexer) {
    lexer_skip_whitespace_and_comments(lexer);

    SourceLoc loc = {
        .filename = lexer->filename,
        .line     = lexer->line,
        .col      = lexer->col
    };

    if (lexer_is_eof(lexer)) {
        return (Token){
            .kind   = TOK_EOF,
            .lexeme = (StrView){ .data = "", .len = 0 },
            .loc    = loc
        };
    }

    size_t start_pos = lexer->cursor;
    char c = lexer_advance(lexer);

    if (is_ident_start(c)) {
        return lexer_ident_or_keyword(lexer, start_pos, loc);
    }

    if (is_digit(c)) {
        return lexer_number(lexer, start_pos, loc);
    }

    if (c == '"') {
        return lexer_string(lexer, start_pos, loc);
    }

    switch (c) {
        case '(': return (Token){ .kind = TOK_LPAREN,    .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case ')': return (Token){ .kind = TOK_RPAREN,    .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case '{': return (Token){ .kind = TOK_LBRACE,    .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case '}': return (Token){ .kind = TOK_RBRACE,    .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case '[': return (Token){ .kind = TOK_LBRACKET,  .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case ']': return (Token){ .kind = TOK_RBRACKET,  .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case ':': return (Token){ .kind = TOK_COLON,     .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case ';': return (Token){ .kind = TOK_SEMICOLON, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case ',': return (Token){ .kind = TOK_COMMA,     .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case '.': return (Token){ .kind = TOK_DOT,       .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case '*': return (Token){ .kind = TOK_STAR,      .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case '/': return (Token){ .kind = TOK_SLASH,     .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case '%': return (Token){ .kind = TOK_PERCENT,   .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
        case '~': return (Token){ .kind = TOK_TILDE,     .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };

        case '&':
            if (lexer_match(lexer, '&')) {
                return (Token){ .kind = TOK_AMP_AMP,  .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            if (lexer_match(lexer, '=')) {
                return (Token){ .kind = TOK_AMP_EQ,   .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            return (Token){ .kind = TOK_AMP,      .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };

        case '|':
            if (lexer_match(lexer, '|')) {
                return (Token){ .kind = TOK_PIPE_PIPE, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            if (lexer_match(lexer, '=')) {
                return (Token){ .kind = TOK_PIPE_EQ,  .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            return (Token){ .kind = TOK_PIPE,     .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };

        case '^':
            if (lexer_match(lexer, '=')) {
                return (Token){ .kind = TOK_CARET_EQ, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            return (Token){ .kind = TOK_CARET,    .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };

        case '<':
            if (lexer_match(lexer, '<')) {
                if (lexer_match(lexer, '=')) {
                    return (Token){ .kind = TOK_SHL_EQ, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 3 }, .loc = loc };
                }
                return (Token){ .kind = TOK_SHL, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            if (lexer_match(lexer, '=')) {
                return (Token){ .kind = TOK_LESS_EQ, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            return (Token){ .kind = TOK_LESS, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };

        case '>':
            if (lexer_match(lexer, '>')) {
                if (lexer_match(lexer, '=')) {
                    return (Token){ .kind = TOK_SHR_EQ, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 3 }, .loc = loc };
                }
                return (Token){ .kind = TOK_SHR, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            if (lexer_match(lexer, '=')) {
                return (Token){ .kind = TOK_GREATER_EQ, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            return (Token){ .kind = TOK_GREATER, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };

        case '-':
            if (lexer_match(lexer, '>')) {
                return (Token){ .kind = TOK_ARROW,    .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            if (lexer_match(lexer, '=')) {
                return (Token){ .kind = TOK_MINUS_EQ, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            return (Token){ .kind = TOK_MINUS, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };

        case '+':
            if (lexer_match(lexer, '=')) {
                return (Token){ .kind = TOK_PLUS_EQ, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            return (Token){ .kind = TOK_PLUS, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };

        case '=':
            if (lexer_match(lexer, '=')) {
                return (Token){ .kind = TOK_EQ_EQ, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            return (Token){ .kind = TOK_EQ, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };

        case '!':
            if (lexer_match(lexer, '=')) {
                return (Token){ .kind = TOK_BANG_EQ, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 2 }, .loc = loc };
            }
            return (Token){ .kind = TOK_BANG, .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 }, .loc = loc };
    }

    return (Token){
        .kind   = TOK_ERROR,
        .lexeme = (StrView){ .data = lexer->source + start_pos, .len = 1 },
        .loc    = loc
    };
}

const char* token_kind_to_str(TokenKind kind) {
    switch (kind) {
        case TOK_EOF:        return "<EOF>";
        case TOK_ERROR:      return "<ERROR>";
        case TOK_IDENT:      return "identifier";
        case TOK_INT_LIT:    return "int_literal";
        case TOK_STRING_LIT: return "string_literal";
        case TOK_PROC:       return "proc";
        case TOK_VAR:        return "var";
        case TOK_CONST:      return "const";
        case TOK_RETURN:     return "return";
        case TOK_IF:         return "if";
        case TOK_ELSE:       return "else";
        case TOK_WHILE:      return "while";
        case TOK_BREAK:      return "break";
        case TOK_CONTINUE:   return "continue";
        case TOK_CAST:       return "cast";
        case TOK_IMPORT:     return "import";
        case TOK_ASM:        return "asm";
        case TOK_STRUCT:     return "struct";
        case TOK_PACKED:     return "packed";
        case TOK_BOOL:       return "bool";
        case TOK_CHAR:       return "char";
        case TOK_VOID:       return "void";
        case TOK_I8:         return "i8";
        case TOK_I16:        return "i16";
        case TOK_I32:        return "i32";
        case TOK_I64:        return "i64";
        case TOK_U8:         return "u8";
        case TOK_U16:        return "u16";
        case TOK_U32:        return "u32";
        case TOK_U64:        return "u64";
        case TOK_ARROW:      return "->";
        case TOK_LPAREN:     return "(";
        case TOK_RPAREN:     return ")";
        case TOK_LBRACE:     return "{";
        case TOK_RBRACE:     return "}";
        case TOK_LBRACKET:   return "[";
        case TOK_RBRACKET:   return "]";
        case TOK_COLON:      return ":";
        case TOK_SEMICOLON:  return ";";
        case TOK_COMMA:      return ",";
        case TOK_DOT:        return ".";
        case TOK_STAR:       return "*";
        case TOK_SLASH:      return "/";
        case TOK_PERCENT:    return "%";
        case TOK_PLUS:       return "+";
        case TOK_MINUS:      return "-";
        case TOK_AMP:        return "&";
        case TOK_PIPE:       return "|";
        case TOK_CARET:      return "^";
        case TOK_TILDE:      return "~";
        case TOK_SHL:        return "<<";
        case TOK_SHR:        return ">>";
        case TOK_EQ:         return "=";
        case TOK_PLUS_EQ:    return "+=";
        case TOK_MINUS_EQ:   return "-=";
        case TOK_AMP_EQ:     return "&=";
        case TOK_PIPE_EQ:    return "|=";
        case TOK_CARET_EQ:   return "^=";
        case TOK_SHL_EQ:     return "<<=";
        case TOK_SHR_EQ:     return ">>=";
        case TOK_BANG:       return "!";
        case TOK_AMP_AMP:    return "&&";
        case TOK_PIPE_PIPE:  return "||";
        case TOK_EQ_EQ:      return "==";
        case TOK_BANG_EQ:    return "!=";
        case TOK_LESS:       return "<";
        case TOK_LESS_EQ:    return "<=";
        case TOK_GREATER:    return ">";
        case TOK_GREATER_EQ: return ">=";
    }

    return "<UNKNOWN>";
}