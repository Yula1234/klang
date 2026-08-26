#ifndef KLANG_DRIVER_H
#define KLANG_DRIVER_H

#include <stdbool.h>
#include <stddef.h>

#include "arena.h"
#include "ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum OutputPhase {
    PHASE_EXE,
    PHASE_OBJ,
    PHASE_ASM,
    PHASE_SHARED
} OutputPhase;

typedef struct DriverConfig {
    const char*  input_path;
    const char*  output_path;
    const char*  include_dir;

    OutputPhase  phase;
    bool         dump_ir;
    bool         save_temps;
    bool         verbose;
    bool         no_pie;
    bool         static_link;

    size_t       fasm_memory_mb;

    const char** libs;
    size_t       lib_count;
    size_t       lib_cap;

    const char** lib_dirs;
    size_t       lib_dir_count;
    size_t       lib_dir_cap;

    const char** linker_extra_args;
    size_t       linker_extra_count;
    size_t       linker_extra_cap;

    const char** asm_extra_args;
    size_t       asm_extra_count;
    size_t       asm_extra_cap;
} DriverConfig;

int driver_run_pipeline(Arena* arena, const DriverConfig* config, IRModule* ir_module);

#ifdef __cplusplus
}
#endif

#endif