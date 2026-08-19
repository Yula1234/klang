#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#   include <io.h>
#   define diag_is_atty(f) _isatty(_fileno(f))
#else
#   include <unistd.h>
#   define diag_is_atty(f) isatty(fileno(f))
#endif

#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_RED     "\033[1;31m"
#define ANSI_YELLOW  "\033[1;33m"
#define ANSI_CYAN    "\033[1;36m"
#define ANSI_BLUE    "\033[1;34m"

static int get_digit_count(uint32_t val) {
    int digits = 1;
    while (val >= 10) {
        val /= 10;
        digits++;
    }
    return digits;
}

void diag_report_valist(DiagKind kind, SourceLoc loc, const char* fmt, va_list args) {
    FILE* out = stderr;
    bool use_color = diag_is_atty(out);

    const char* kind_str   = "error";
    const char* kind_color = ANSI_RED;

    if (kind == DIAG_WARNING) {
        kind_str   = "warning";
        kind_color = ANSI_YELLOW;
    } else if (kind == DIAG_NOTE) {
        kind_str   = "note";
        kind_color = ANSI_CYAN;
    }

    if (use_color) {
        fprintf(out, "%s%s: %s", kind_color, kind_str, ANSI_BOLD);
    } else {
        fprintf(out, "%s: ", kind_str);
    }

    vfprintf(out, fmt, args);

    if (use_color) {
        fprintf(out, "%s\n", ANSI_RESET);
    } else {
        fprintf(out, "\n");
    }

    if (!loc.filename) {
        return;
    }

    int gutter_width = get_digit_count(loc.line);
    if (gutter_width < 2) {
        gutter_width = 2;
    }

    if (use_color) {
        fprintf(out, "%s%*s--> %s%s:%u:%u\n", ANSI_BLUE, gutter_width, "", ANSI_RESET, loc.filename, loc.line, loc.col);
        fprintf(out, "%s%*s |\n", ANSI_BLUE, gutter_width, "");
        fprintf(out, "%*u | %s", gutter_width, loc.line, ANSI_RESET);
    } else {
        fprintf(out, "%*s--> %s:%u:%u\n", gutter_width, "", loc.filename, loc.line, loc.col);
        fprintf(out, "%*s |\n", gutter_width, "");
        fprintf(out, "%*u | ", gutter_width, loc.line);
    }

    if (!loc.line_start) {
        if (use_color) {
            fprintf(out, "%s%*s |\n%s", ANSI_BLUE, gutter_width, "", ANSI_RESET);
        } else {
            fprintf(out, "%*s |\n", gutter_width, "");
        }
        return;
    }

    size_t line_len = 0;
    while (loc.line_start[line_len] != '\0' &&
           loc.line_start[line_len] != '\n' &&
           loc.line_start[line_len] != '\r') {
        line_len++;
    }

    size_t visual_col = 0;
    size_t caret_offset = 0;
    size_t target_col = (loc.col > 0) ? (loc.col - 1) : 0;

    for (size_t i = 0; i < line_len; ++i) {
        char c = loc.line_start[i];

        if (i == target_col) {
            caret_offset = visual_col;
        }

        if (c == '\t') {
            size_t tab_stop = 4 - (visual_col % 4);
            for (size_t t = 0; t < tab_stop; ++t) {
                fputc(' ', out);
                visual_col++;
            }
        } else {
            fputc(c, out);
            visual_col++;
        }
    }

    if (target_col >= line_len) {
        caret_offset = visual_col;
    }

    fputc('\n', out);

    if (use_color) {
        fprintf(out, "%s%*s | %s", ANSI_BLUE, gutter_width, "", kind_color);
    } else {
        fprintf(out, "%*s | ", gutter_width, "");
    }

    for (size_t i = 0; i < caret_offset; ++i) {
        fputc(' ', out);
    }

    uint32_t underline_len = (loc.len > 0) ? loc.len : 1;

    for (uint32_t i = 0; i < underline_len; ++i) {
        fputc('^', out);
    }

    if (use_color) {
        fprintf(out, "%s\n", ANSI_RESET);
        fprintf(out, "%s%*s |\n%s", ANSI_BLUE, gutter_width, "", ANSI_RESET);
    } else {
        fprintf(out, "\n");
        fprintf(out, "%*s |\n", gutter_width, "");
    }
}

void diag_report(DiagKind kind, SourceLoc loc, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diag_report_valist(kind, loc, fmt, args);
    va_end(args);
}
