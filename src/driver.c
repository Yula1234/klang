#include "driver.h"
#include "process.h"
#include "codegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* extract_file_stem(Arena* arena, const char* path) {
    if (!path) {
        return "output";
    }

    const char* last_slash = strrchr(path, '/');

#if defined(_WIN32) || defined(_WIN64)
    const char* last_bslash = strrchr(path, '\\');
    if (!last_slash || (last_bslash && last_bslash > last_slash)) {
        last_slash = last_bslash;
    }
#endif

    const char* filename = last_slash ? (last_slash + 1) : path;
    const char* dot      = strrchr(filename, '.');

    if (!dot || dot == filename) {
        return arena_strdup(arena, filename);
    }

    return arena_strndup(arena, filename, (size_t)(dot - filename));
}

static const char* resolve_default_output_name(Arena* arena, const DriverConfig* config) {
    if (config->output_path != NULL) {
        return config->output_path;
    }

    const char* stem = extract_file_stem(arena, config->input_path);

    switch (config->phase) {
        case PHASE_ASM:
            return arena_sprintf(arena, "%s.asm", stem);

        case PHASE_OBJ:
            return arena_sprintf(arena, "%s.o", stem);

        case PHASE_SHARED:
            return arena_sprintf(arena, "%s.so", stem);

        case PHASE_EXE:
        default:
            return arena_sprintf(arena, "%s", stem);
    }
}

static int run_assembler(Arena* arena, const DriverConfig* config, const char* asm_path, const char* obj_path) {
    if (!process_find_executable("fasm")) {
        fprintf(stderr, "klang: error: 'fasm' assembler not found in PATH.\n");
        return -1;
    }

    ProcessArgs p_args;
    process_args_init(&p_args, arena);

    process_args_add(&p_args, arena, "fasm");

    size_t mem_kb = (config->fasm_memory_mb > 0) ? (config->fasm_memory_mb * 1024) : 524288;
    const char* mem_str = arena_sprintf(arena, "%zu", mem_kb);

    process_args_add(&p_args, arena, "-m");
    process_args_add(&p_args, arena, mem_str);

    for (size_t i = 0; i < config->asm_extra_count; ++i) {
        process_args_add(&p_args, arena, config->asm_extra_args[i]);
    }

    process_args_add(&p_args, arena, asm_path);
    process_args_add(&p_args, arena, obj_path);

    return process_exec(&p_args, config->verbose);
}

static int run_linker(Arena* arena, const DriverConfig* config, const char* obj_path, const char* out_bin) {
    if (!process_find_executable("gcc")) {
        fprintf(stderr, "klang: error: 'gcc' linker driver not found in PATH.\n");
        return -1;
    }

    ProcessArgs p_args;
    process_args_init(&p_args, arena);

    process_args_add(&p_args, arena, "gcc");
    process_args_add(&p_args, arena, obj_path);

    if (config->phase == PHASE_SHARED) {
        process_args_add(&p_args, arena, "-shared");
    }

    if (config->static_link) {
        process_args_add(&p_args, arena, "-static");
    }

    if (config->no_pie) {
        process_args_add(&p_args, arena, "-no-pie");
    }

    for (size_t i = 0; i < config->lib_dir_count; ++i) {
        const char* dir_arg = arena_sprintf(arena, "-L%s", config->lib_dirs[i]);
        process_args_add(&p_args, arena, dir_arg);
    }

    for (size_t i = 0; i < config->lib_count; ++i) {
        const char* lib_arg = arena_sprintf(arena, "-l%s", config->libs[i]);
        process_args_add(&p_args, arena, lib_arg);
    }

    for (size_t i = 0; i < config->linker_extra_count; ++i) {
        process_args_add(&p_args, arena, config->linker_extra_args[i]);
    }

    process_args_add(&p_args, arena, "-o");
    process_args_add(&p_args, arena, out_bin);

    return process_exec(&p_args, config->verbose);
}

int driver_run_pipeline(Arena* arena, const DriverConfig* config, IRModule* ir_module) {
    const char* final_out = resolve_default_output_name(arena, config);
    const char* stem      = extract_file_stem(arena, config->input_path);

    const char* asm_path = NULL;
    const char* obj_path = NULL;

    bool cleanup_asm = false;
    bool cleanup_obj = false;

    if (config->phase == PHASE_ASM) {
        asm_path = final_out;
    } else {
        if (config->save_temps) {
            asm_path = arena_sprintf(arena, "%s.asm", stem);
        } else {
            asm_path = arena_sprintf(arena, "%s.tmp.%d.asm", stem, (int)getpid());
            cleanup_asm = true;
        }
    }

    if (!codegen_generate_file(ir_module, asm_path)) {
        fprintf(stderr, "klang: error: failed to write assembly to '%s'\n", asm_path);
        return 1;
    }

    if (config->phase == PHASE_ASM) {
        return 0;
    }

    if (config->phase == PHASE_OBJ) {
        obj_path = final_out;
    } else {
        if (config->save_temps) {
            obj_path = arena_sprintf(arena, "%s.o", stem);
        } else {
            obj_path = arena_sprintf(arena, "%s.tmp.%d.o", stem, (int)getpid());
            cleanup_obj = true;
        }
    }

    int fasm_res = run_assembler(arena, config, asm_path, obj_path);

    if (cleanup_asm) {
        unlink(asm_path);
    }

    if (fasm_res != 0) {
        fprintf(stderr, "klang: error: assembler failed with exit code %d\n", fasm_res);
        if (cleanup_obj) {
            unlink(obj_path);
        }
        return fasm_res;
    }

    if (config->phase == PHASE_OBJ) {
        return 0;
    }

    int link_res = run_linker(arena, config, obj_path, final_out);

    if (cleanup_obj) {
        unlink(obj_path);
    }

    if (link_res != 0) {
        fprintf(stderr, "klang: error: linker failed with exit code %d\n", link_res);
        return link_res;
    }

    return 0;
}