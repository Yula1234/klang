#ifndef KLANG_PARSER_H
#define KLANG_PARSER_H

#include <stdbool.h>

#include "lexer.h"
#include "ast.h"
#include "arena.h"
#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Parser {
    Lexer*   lexer;
    Arena*   arena;
    Token    current;
    Token    prev;
    bool     had_error;
} Parser;

void        parser_init(Parser* parser, Lexer* lexer, Arena* arena);

AstProgram* parse_program(Parser* parser);

#ifdef __cplusplus
}
#endif

#endif // KLANG_PARSER_H