#ifndef KLANG_ABI_H
#define KLANG_ABI_H

#include <stdbool.h>
#include <stddef.h>

#include "x86.h"
#include "type.h"

#define KLANG_ABI_GP_ARG_COUNT             6
#define KLANG_ABI_GP_SLOT_SIZE             8
#define KLANG_ABI_GP_REG_SAVE_SIZE         (KLANG_ABI_GP_ARG_COUNT * KLANG_ABI_GP_SLOT_SIZE)
#define KLANG_ABI_FIRST_STACK_ARG_OFFSET   16

size_t abi_function_hidden_gp_arg_count(const Type* return_type);

size_t abi_function_fixed_gp_arg_count(
    const Type* return_type,
    size_t source_param_count
);

size_t abi_va_gp_offset(size_t fixed_gp_arg_count);

const char* abi_gp_arg_reg_name(size_t index);

X86Reg abi_gp_arg_reg(size_t index);

#endif
