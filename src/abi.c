#include "abi.h"

#include <assert.h>

static const char* const ABI_GP_ARG_REG_NAMES[KLANG_ABI_GP_ARG_COUNT] = {
    "rdi",
    "rsi",
    "rdx",
    "rcx",
    "r8",
    "r9"
};

static const X86Reg ABI_GP_ARG_REGS[KLANG_ABI_GP_ARG_COUNT] = {
    REG_RDI,
    REG_RSI,
    REG_RDX,
    REG_RCX,
    REG_R8,
    REG_R9
};

size_t abi_function_hidden_gp_arg_count(const Type* return_type) {
    return type_requires_sret(return_type) ? 1 : 0;
}

size_t abi_function_fixed_gp_arg_count(
    const Type* return_type,
    size_t source_param_count
) {
    return abi_function_hidden_gp_arg_count(return_type) + source_param_count;
}

size_t abi_va_gp_offset(size_t fixed_gp_arg_count) {
    size_t register_arg_count = fixed_gp_arg_count;

    if (register_arg_count > KLANG_ABI_GP_ARG_COUNT) {
        register_arg_count = KLANG_ABI_GP_ARG_COUNT;
    }

    return register_arg_count * KLANG_ABI_GP_SLOT_SIZE;
}

const char* abi_gp_arg_reg_name(size_t index) {
    assert(index < KLANG_ABI_GP_ARG_COUNT);

    return ABI_GP_ARG_REG_NAMES[index];
}

X86Reg abi_gp_arg_reg(size_t index) {
    assert(index < KLANG_ABI_GP_ARG_COUNT);

    return ABI_GP_ARG_REGS[index];
}
