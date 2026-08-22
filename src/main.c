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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define KLANG_VERSION "0.1.0"

typedef struct Config {
    const char* input_path;
    const char* output_path;
    const char* include_dir;
    bool        dump_ir;
} Config;

static void print_version(void) {
    printf("klang version %s (Kernel Language Compiler)\n", KLANG_VERSION);
}

static void print_help(const char* prog_name) {
    printf("Usage: %s <input.kl> [options]\n\n", prog_name);
    printf("Options:\n");
    printf("  -o <file>        Specify output assembly file (default: output.asm)\n");
    printf("  -I <dir>         Add directory to module search path\n");
    printf("  --dump-ir        Print Intermediate Representation (3AC) to stdout\n");
    printf("  --version        Display compiler version information\n");
    printf("  -h, --help       Display this help message\n\n");
}

static Config parse_args(int argc, char* argv[]) {
    Config config = {
        .input_path   = NULL,
        .output_path  = "output.asm",
        .include_dir  = NULL,
        .dump_ir      = false,
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
    Config config = parse_args(argc, argv);

    size_t source_len = 0;
    char* source = read_entire_file(config.input_path, &source_len);

    Arena arena;
    arena_init(&arena, 4 * 1024 * 1024);

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

    regalloc_run_on_module(&arena, ir_module);

    peephole_run_on_module(&arena, ir_module);

    if (config.dump_ir) {
        ir_dump_module(ir_module, &arena);
    }

    bool codegen_ok = codegen_generate_file(ir_module, config.output_path);

    if (!codegen_ok) {
        fprintf(stderr, "error: failed to write output assembly to '%s'\n", config.output_path);
        arena_destroy(&arena);
        free(source);
        return 1;
    }

    printf("Successfully generated: %s\n", config.output_path);

    arena_destroy(&arena);
    free(source);

    return 0;
}