#ifndef KLANG_PROCESS_H
#define KLANG_PROCESS_H

#include <stdbool.h>
#include <stddef.h>

#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ProcessArgs {
    const char** args;
    size_t       count;
    size_t       capacity;
} ProcessArgs;

void process_args_init(ProcessArgs* p_args, Arena* arena);

void process_args_add(ProcessArgs* p_args, Arena* arena, const char* arg);

int  process_exec(const ProcessArgs* p_args, bool verbose);

bool process_find_executable(const char* name);

#ifdef __cplusplus
}
#endif

#endif