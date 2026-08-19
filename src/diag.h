#ifndef KLANG_DIAG_H
#define KLANG_DIAG_H

#include <stdbool.h>
#include <stdarg.h>

#include "lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DiagKind {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE
} DiagKind;

void diag_report(DiagKind kind, SourceLoc loc, const char* fmt, ...);
void diag_report_valist(DiagKind kind, SourceLoc loc, const char* fmt, va_list args);

#ifdef __cplusplus
}
#endif

#endif
