#include "x86.h"

#include <string.h>
#include <assert.h>

const char* reg_name(X86Reg reg, size_t byte_size) {
    if (byte_size == 0) {
        byte_size = 8;
    }

    switch (reg) {
        case REG_RAX:
            if (byte_size == 1) return "al";
            if (byte_size == 2) return "ax";
            if (byte_size == 4) return "eax";
            return "rax";

        case REG_RCX:
            if (byte_size == 1) return "cl";
            if (byte_size == 2) return "cx";
            if (byte_size == 4) return "ecx";
            return "rcx";

        case REG_RDX:
            if (byte_size == 1) return "dl";
            if (byte_size == 2) return "dx";
            if (byte_size == 4) return "edx";
            return "rdx";

        case REG_RBX:
            if (byte_size == 1) return "bl";
            if (byte_size == 2) return "bx";
            if (byte_size == 4) return "ebx";
            return "rbx";

        case REG_RSI:
            if (byte_size == 1) return "sil";
            if (byte_size == 2) return "si";
            if (byte_size == 4) return "esi";
            return "rsi";

        case REG_RDI:
            if (byte_size == 1) return "dil";
            if (byte_size == 2) return "di";
            if (byte_size == 4) return "edi";
            return "rdi";

        case REG_RSP:
            if (byte_size == 1) return "spl";
            if (byte_size == 2) return "sp";
            if (byte_size == 4) return "esp";
            return "rsp";

        case REG_RBP:
            if (byte_size == 1) return "bpl";
            if (byte_size == 2) return "bp";
            if (byte_size == 4) return "ebp";
            return "rbp";

        case REG_R8:
            if (byte_size == 1) return "r8b";
            if (byte_size == 2) return "r8w";
            if (byte_size == 4) return "r8d";
            return "r8";

        case REG_R9:
            if (byte_size == 1) return "r9b";
            if (byte_size == 2) return "r9w";
            if (byte_size == 4) return "r9d";
            return "r9";

        case REG_R10:
            if (byte_size == 1) return "r10b";
            if (byte_size == 2) return "r10w";
            if (byte_size == 4) return "r10d";
            return "r10";

        case REG_R11:
            if (byte_size == 1) return "r11b";
            if (byte_size == 2) return "r11w";
            if (byte_size == 4) return "r11d";
            return "r11";

        case REG_R12:
            if (byte_size == 1) return "r12b";
            if (byte_size == 2) return "r12w";
            if (byte_size == 4) return "r12d";
            return "r12";

        case REG_R13:
            if (byte_size == 1) return "r13b";
            if (byte_size == 2) return "r13w";
            if (byte_size == 4) return "r13d";
            return "r13";

        case REG_R14:
            if (byte_size == 1) return "r14b";
            if (byte_size == 2) return "r14w";
            if (byte_size == 4) return "r14d";
            return "r14";

        case REG_R15:
            if (byte_size == 1) return "r15b";
            if (byte_size == 2) return "r15w";
            if (byte_size == 4) return "r15d";
            return "r15";

        default:
            return "unknown_reg";
    }
}

bool reg_is_callee_saved(X86Reg reg) {
    return (
        reg == REG_RBX ||
        reg == REG_R12 ||
        reg == REG_R13 ||
        reg == REG_R14 ||
        reg == REG_R15
    );
}

X86Reg parse_reg_name(StrView name, size_t* out_byte_size) {
    if (name.len == 0 || name.data == NULL) {
        return REG_NONE;
    }

    if (name.len == 2) {
        if (memcmp(name.data, "al", 2) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_RAX;
        }

        if (memcmp(name.data, "cl", 2) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_RCX;
        }

        if (memcmp(name.data, "dl", 2) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_RDX;
        }

        if (memcmp(name.data, "bl", 2) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_RBX;
        }

        if (memcmp(name.data, "ah", 2) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_RAX;
        }

        if (memcmp(name.data, "ch", 2) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_RCX;
        }

        if (memcmp(name.data, "dh", 2) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_RDX;
        }

        if (memcmp(name.data, "bh", 2) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_RBX;
        }

        if (memcmp(name.data, "ax", 2) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_RAX;
        }

        if (memcmp(name.data, "cx", 2) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_RCX;
        }

        if (memcmp(name.data, "dx", 2) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_RDX;
        }

        if (memcmp(name.data, "bx", 2) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_RBX;
        }

        if (memcmp(name.data, "si", 2) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_RSI;
        }

        if (memcmp(name.data, "di", 2) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_RDI;
        }

        if (memcmp(name.data, "bp", 2) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_RBP;
        }

        if (memcmp(name.data, "sp", 2) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_RSP;
        }

        if (memcmp(name.data, "r8", 2) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_R8;
        }

        if (memcmp(name.data, "r9", 2) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_R9;
        }

        if (memcmp(name.data, "r10", 2) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_R10;
        }

        if (memcmp(name.data, "r11", 2) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_R11;
        }

        if (memcmp(name.data, "r12", 2) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_R12;
        }

        if (memcmp(name.data, "r13", 2) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_R13;
        }

        if (memcmp(name.data, "r14", 2) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_R14;
        }

        if (memcmp(name.data, "r15", 2) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_R15;
        }
    }

    if (name.len == 3) {
        if (memcmp(name.data, "eax", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_RAX;
        }

        if (memcmp(name.data, "ecx", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_RCX;
        }

        if (memcmp(name.data, "edx", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_RDX;
        }

        if (memcmp(name.data, "ebx", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_RBX;
        }

        if (memcmp(name.data, "esi", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_RSI;
        }

        if (memcmp(name.data, "edi", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_RDI;
        }

        if (memcmp(name.data, "ebp", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_RBP;
        }

        if (memcmp(name.data, "esp", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_RSP;
        }

        if (memcmp(name.data, "r8d", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_R8;
        }

        if (memcmp(name.data, "r9d", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_R9;
        }

        if (memcmp(name.data, "r10d", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_R10;
        }

        if (memcmp(name.data, "r11d", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_R11;
        }

        if (memcmp(name.data, "r12d", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_R12;
        }

        if (memcmp(name.data, "r13d", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_R13;
        }

        if (memcmp(name.data, "r14d", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_R14;
        }

        if (memcmp(name.data, "r15d", 3) == 0) {
            if (out_byte_size) *out_byte_size = 4;
            return REG_R15;
        }

        if (memcmp(name.data, "r8b", 3) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_R8;
        }

        if (memcmp(name.data, "r9b", 3) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_R9;
        }

        if (memcmp(name.data, "r10b", 3) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_R10;
        }

        if (memcmp(name.data, "r11b", 3) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_R11;
        }

        if (memcmp(name.data, "r12b", 3) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_R12;
        }

        if (memcmp(name.data, "r13b", 3) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_R13;
        }

        if (memcmp(name.data, "r14b", 3) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_R14;
        }

        if (memcmp(name.data, "r15b", 3) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_R15;
        }

        if (memcmp(name.data, "r8w", 3) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_R8;
        }

        if (memcmp(name.data, "r9w", 3) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_R9;
        }

        if (memcmp(name.data, "r10w", 3) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_R10;
        }

        if (memcmp(name.data, "r11w", 3) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_R11;
        }

        if (memcmp(name.data, "r12w", 3) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_R12;
        }

        if (memcmp(name.data, "r13w", 3) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_R13;
        }

        if (memcmp(name.data, "r14w", 3) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_R14;
        }

        if (memcmp(name.data, "r15w", 3) == 0) {
            if (out_byte_size) *out_byte_size = 2;
            return REG_R15;
        }
    }

    if (name.len == 4) {
        if (memcmp(name.data, "sil", 3) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_RSI;
        }

        if (memcmp(name.data, "dil", 3) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_RDI;
        }

        if (memcmp(name.data, "bpl", 3) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_RBP;
        }

        if (memcmp(name.data, "spl", 3) == 0) {
            if (out_byte_size) *out_byte_size = 1;
            return REG_RSP;
        }
    }

    if (name.len == 3) {
        if (memcmp(name.data, "rax", 3) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_RAX;
        }

        if (memcmp(name.data, "rcx", 3) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_RCX;
        }

        if (memcmp(name.data, "rdx", 3) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_RDX;
        }

        if (memcmp(name.data, "rbx", 3) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_RBX;
        }

        if (memcmp(name.data, "rsi", 3) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_RSI;
        }

        if (memcmp(name.data, "rdi", 3) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_RDI;
        }

        if (memcmp(name.data, "rbp", 3) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_RBP;
        }

        if (memcmp(name.data, "rsp", 3) == 0) {
            if (out_byte_size) *out_byte_size = 8;
            return REG_RSP;
        }
    }

    return REG_NONE;
}
