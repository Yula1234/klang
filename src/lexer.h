#ifndef KLANG_LEXER_H
#define KLANG_LEXER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StrView {
    const char* data;
    size_t      len;
} StrView;

typedef struct SourceLoc {
    const char* filename;
    uint32_t    line;
    uint32_t    col;
} SourceLoc;

typedef enum TokenKind {
    TOK_EOF = 0,
    TOK_ERROR,

    TOK_IDENT,
    TOK_INT_LIT,
    TOK_STRING_LIT,
    TOK_PROC,
    TOK_VAR,
    TOK_RETURN,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_CAST,
    TOK_IMPORT,
    TOK_ASM,

    TOK_BOOL,
    TOK_CHAR,
    TOK_VOID,
    TOK_I8,
    TOK_I16,
    TOK_I32,
    TOK_I64,
    TOK_U8,
    TOK_U16,
    TOK_U32,
    TOK_U64,

    TOK_ARROW,     
    TOK_LPAREN,    
    TOK_RPAREN,    
    TOK_LBRACE,    
    TOK_RBRACE,   
    TOK_LBRACKET,
    TOK_RBRACKET, 
    TOK_COLON,     
    TOK_SEMICOLON, 
    TOK_COMMA,

    TOK_AMP,   
    TOK_PIPE,  
    TOK_CARET, 
    TOK_TILDE, 
    TOK_SHL,   
    TOK_SHR,   

    TOK_AMP_EQ,
    TOK_PIPE_EQ,
    TOK_CARET_EQ,
    TOK_SHL_EQ,
    TOK_SHR_EQ,

    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_PLUS,
    TOK_MINUS,
    TOK_EQ,
    TOK_PLUS_EQ,
    TOK_MINUS_EQ,

    TOK_BANG,
    TOK_AMP_AMP,
    TOK_PIPE_PIPE,
    TOK_EQ_EQ,
    TOK_BANG_EQ,
    TOK_LESS,
    TOK_LESS_EQ,
    TOK_GREATER,
    TOK_GREATER_EQ
} TokenKind;

typedef struct Token {
    TokenKind kind;
    StrView   lexeme;
    SourceLoc loc;
} Token;

typedef struct Lexer {
    const char* source;
    size_t      source_len;
    const char* filename;
    size_t      cursor;
    uint32_t    line;
    uint32_t    col;
} Lexer;

void        lexer_init(Lexer* lexer, const char* source, size_t source_len, const char* filename);

Token       lexer_next_token(Lexer* lexer);

const char* token_kind_to_str(TokenKind kind);

#ifdef __cplusplus
}
#endif

#endif // KLANG_LEXER_H