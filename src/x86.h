#ifndef KLANG_X86_H
#define KLANG_X86_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum X86Reg {
    REG_NONE = 0,
    REG_RAX,
    REG_RCX,
    REG_RDX,
    REG_RBX,
    REG_RSI,
    REG_RDI,
    REG_RSP,
    REG_RBP,
    REG_R8,
    REG_R9,
    REG_R10,
    REG_R11,
    REG_R12,
    REG_R13,
    REG_R14,
    REG_R15,
    REG_COUNT
} X86Reg;

const char* reg_name(X86Reg reg, size_t byte_size);

bool reg_is_callee_saved(X86Reg reg);

X86Reg parse_reg_name(StrView name, size_t* out_byte_size);

#ifdef __cplusplus
}
#endif

#endif
