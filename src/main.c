#include "lexer.h"
#include "arena.h"
#include "type.h"
#include "ast.h"
#include "parser.h"
#include "sema.h"
#include "ir.h"
#include "codegen.h"
#include "regalloc.h"
#include "mem2reg.h"
#include "ir_opt.h"
#include "peephole.h"
#include "out_of_ssa.h"
#include "sroa.h"
#include "sccp.h"
#include "gvn.h"
#include "licm.h"
#include "cfg_opt.h"
#include "stack_color.h"
#include "driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define KLANG_VERSION "0.1.0"

static void print_version(void) {
    printf("klang version %s (Kernel Language Compiler)\n", KLANG_VERSION);
}

static void print_help(const char* prog_name) {
    printf("Usage: %s <input.kl> [options]\n\n", prog_name);
    printf("Compilation Stage Control:\n");
    printf("  -c                   Compile and assemble, but do not link (emit .o)\n");
    printf("  -S                   Compile only, do not assemble or link (emit .asm)\n");
    printf("  -shared              Create a shared library\n");
    printf("  -static              Produce a statically linked executable\n");
    printf("  --dump-ir            Print Intermediate Representation (3AC) to stdout\n\n");
    printf("Options:\n");
    printf("  -o <file>            Specify output file name\n");
    printf("  -I <dir>             Add directory to module search path\n");
    printf("  -l<lib>              Pass library to linker\n");
    printf("  -L<dir>              Add library search directory\n");
    printf("  -Wl,<arg>            Pass comma-separated argument(s) to linker\n");
    printf("  -Wa,<arg>            Pass comma-separated argument(s) to assembler\n");
    printf("  -no-pie              Do not produce position-independent executable (default)\n");
    printf("  -pie                 Produce position-independent executable\n");
    printf("  -save-temps          Do not delete intermediate .asm and .o files\n");
    printf("  -v, --verbose        Print commands executed by driver\n");
    printf("  --fasm-mem <MB>      Set memory limit for FASM in MB (default: 512)\n");
    printf("  --version            Display compiler version information\n");
    printf("  -h, --help           Display this help message\n\n");
}

static void split_and_add_comma_separated(Arena* arena, const char*** arr, size_t* count, size_t* cap, const char* str) {
    const char* start = str;

    while (*start != '\0') {
        const char* comma = strchr(start, ',');
        size_t len = comma ? (size_t)(comma - start) : strlen(start);

        if (len > 0) {
            const char* item = arena_strndup(arena, start, len);
            ARENA_DA_PUSH(arena, *arr, *count, *cap, item);
        }

        if (!comma) {
            break;
        }

        start = comma + 1;
    }
}

static DriverConfig parse_args(int argc, char* argv[], Arena* arena) {
    DriverConfig config = {
        .input_path         = NULL,
        .output_path        = NULL,
        .include_dir        = NULL,
        .phase              = PHASE_EXE,
        .dump_ir            = false,
        .save_temps         = false,
        .verbose            = false,
        .no_pie             = true,
        .static_link        = false,
        .fasm_memory_mb     = 512,
        .libs               = NULL,
        .lib_count          = 0,
        .lib_cap            = 0,
        .lib_dirs           = NULL,
        .lib_dir_count      = 0,
        .lib_dir_cap        = 0,
        .linker_extra_args  = NULL,
        .linker_extra_count = 0,
        .linker_extra_cap   = 0,
        .asm_extra_args     = NULL,
        .asm_extra_count    = 0,
        .asm_extra_cap      = 0
    };

    if (argc < 2) {
        print_help(argv[0]);
        exit(EXIT_FAILURE);
    }

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_help(argv[0]);
            exit(EXIT_SUCCESS);
        }

        if (strcmp(arg, "--version") == 0) {
            print_version();
            exit(EXIT_SUCCESS);
        }

        if (strcmp(arg, "--dump-ir") == 0) {
            config.dump_ir = true;
            continue;
        }

        if (strcmp(arg, "-S") == 0) {
            config.phase = PHASE_ASM;
            continue;
        }

        if (strcmp(arg, "-c") == 0) {
            config.phase = PHASE_OBJ;
            continue;
        }

        if (strcmp(arg, "-shared") == 0) {
            config.phase = PHASE_SHARED;
            continue;
        }

        if (strcmp(arg, "-static") == 0) {
            config.static_link = true;
            continue;
        }

        if (strcmp(arg, "-no-pie") == 0) {
            config.no_pie = true;
            continue;
        }

        if (strcmp(arg, "-pie") == 0) {
            config.no_pie = false;
            continue;
        }

        if (strcmp(arg, "-save-temps") == 0 || strcmp(arg, "--save-temps") == 0) {
            config.save_temps = true;
            continue;
        }

        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            config.verbose = true;
            continue;
        }

        if (strcmp(arg, "--fasm-mem") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: missing memory size after '--fasm-mem'\n");
                exit(EXIT_FAILURE);
            }
            config.fasm_memory_mb = (size_t)strtoul(argv[++i], NULL, 10);
            continue;
        }

        if (strncmp(arg, "--fasm-mem=", 11) == 0 && strlen(arg) > 11) {
            config.fasm_memory_mb = (size_t)strtoul(arg + 11, NULL, 10);
            continue;
        }

        if (strcmp(arg, "-I") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: missing directory after '-I'\n");
                exit(EXIT_FAILURE);
            }
            config.include_dir = argv[++i];
            continue;
        }

        if (strncmp(arg, "-I", 2) == 0 && strlen(arg) > 2) {
            config.include_dir = arg + 2;
            continue;
        }

        if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: missing output filename after '-o'\n");
                exit(EXIT_FAILURE);
            }
            config.output_path = argv[++i];
            continue;
        }

        if (strncmp(arg, "-l", 2) == 0 && strlen(arg) > 2) {
            const char* lib_name = arg + 2;
            ARENA_DA_PUSH(arena, config.libs, config.lib_count, config.lib_cap, lib_name);
            continue;
        }

        if (strcmp(arg, "-l") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: missing library name after '-l'\n");
                exit(EXIT_FAILURE);
            }
            const char* lib_name = argv[++i];
            ARENA_DA_PUSH(arena, config.libs, config.lib_count, config.lib_cap, lib_name);
            continue;
        }

        if (strncmp(arg, "-L", 2) == 0 && strlen(arg) > 2) {
            const char* dir = arg + 2;
            ARENA_DA_PUSH(arena, config.lib_dirs, config.lib_dir_count, config.lib_dir_cap, dir);
            continue;
        }

        if (strcmp(arg, "-L") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: missing directory after '-L'\n");
                exit(EXIT_FAILURE);
            }
            const char* dir = argv[++i];
            ARENA_DA_PUSH(arena, config.lib_dirs, config.lib_dir_count, config.lib_dir_cap, dir);
            continue;
        }

        if (strncmp(arg, "-Wl,", 4) == 0 && strlen(arg) > 4) {
            split_and_add_comma_separated(arena, &config.linker_extra_args, &config.linker_extra_count, &config.linker_extra_cap, arg + 4);
            continue;
        }

        if (strncmp(arg, "-Wa,", 4) == 0 && strlen(arg) > 4) {
            split_and_add_comma_separated(arena, &config.asm_extra_args, &config.asm_extra_count, &config.asm_extra_cap, arg + 4);
            continue;
        }

        if (arg[0] == '-') {
            fprintf(stderr, "error: unrecognized command-line option '%s'\n", arg);
            exit(EXIT_FAILURE);
        }

        if (config.input_path != NULL) {
            fprintf(stderr, "error: multiple input files are not supported ('%s' and '%s')\n",
                    config.input_path, arg);
            exit(EXIT_FAILURE);
        }

        config.input_path = arg;
    }

    if (config.input_path == NULL) {
        fprintf(stderr, "error: no input file specified\n");
        exit(EXIT_FAILURE);
    }

    return config;
}

static char* read_entire_file(const char* path, size_t* out_size) {
    FILE* file = fopen(path, "rb");

    if (!file) {
        fprintf(stderr, "error: cannot open input file '%s'\n", path);
        exit(EXIT_FAILURE);
    }

    fseek(file, 0, SEEK_END);
    size_t size = (size_t)ftell(file);
    fseek(file, 0, SEEK_SET);

    char* buffer = (char*)malloc(size + 1);

    if (!buffer) {
        fclose(file);
        fprintf(stderr, "fatal error: out of memory reading file '%s'\n", path);
        abort();
    }

    size_t read_bytes = fread(buffer, 1, size, file);
    buffer[read_bytes] = '\0';

    fclose(file);

    if (out_size) {
        *out_size = read_bytes;
    }

    return buffer;
}

int main(int argc, char* argv[]) {
    Arena arena;
    arena_init(&arena, 4 * 1024 * 1024);

    DriverConfig config = parse_args(argc, argv, &arena);

    size_t source_len = 0;
    char* source = read_entire_file(config.input_path, &source_len);

    Lexer lexer;
    lexer_init(&lexer, source, source_len, config.input_path);

    Parser parser;
    parser_init(&parser, &lexer, &arena, config.include_dir);

    AstProgram* program = parse_program(&parser);

    if (parser.had_error) {
        fprintf(stderr, "error: compilation terminated due to parse errors.\n");
        arena_destroy(&arena);
        free(source);
        return 1;
    }

    Sema sema;
    sema_init(&sema, &arena);

    bool sema_ok = sema_analyze_program(&sema, program);

    if (!sema_ok) {
        fprintf(stderr, "error: compilation terminated due to semantic errors.\n");
        arena_destroy(&arena);
        free(source);
        return 1;
    }

    IRModule* ir_module = ir_lower_program(&arena, program);

    if (!ir_module) {
        fprintf(stderr, "fatal error: failed to lower AST to IR.\n");
        arena_destroy(&arena);
        free(source);
        return 1;
    }

    sroa_run_on_module(&arena, ir_module);

    mem2reg_run_on_module(&arena, ir_module);
    
    sccp_run_on_module(&arena, ir_module);
    
    gvn_run_on_module(&arena, ir_module);
    
    licm_run_on_module(&arena, ir_module);
    
    cfg_opt_run_on_module(&arena, ir_module);
    
    ir_opt_run_on_module(&arena, ir_module);
    
    out_of_ssa_run_on_module(&arena, ir_module);
    
    stack_color_run_on_module(&arena, ir_module);
    
    regalloc_run_on_module(&arena, ir_module);
    
    peephole_run_on_module(&arena, ir_module);

    if (config.dump_ir) {
        ir_dump_module(ir_module, &arena);
        arena_destroy(&arena);
        free(source);
        return 0;
    }

    int driver_status = driver_run_pipeline(&arena, &config, ir_module);

    arena_destroy(&arena);
    free(source);

    return driver_status;
}