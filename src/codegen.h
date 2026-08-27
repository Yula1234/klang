#ifndef KLANG_CODEGEN_H
#define KLANG_CODEGEN_H

#include <stdbool.h>
#include <stdio.h>

#include "ir.h"

#ifdef __cplusplus
extern "C" {
#endif

void codegen_emit_fasm(const IRModule* module, FILE* out);

bool codegen_generate_file(const IRModule* module, const char* output_path);

#ifdef __cplusplus
}
#endif

#endif // KLANG_CODEGEN_H