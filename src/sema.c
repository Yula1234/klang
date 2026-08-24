#include "sema.h"

#include "eval.h"
#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

static void sema_error(Sema* sema, SourceLoc loc, const char* fmt, ...) {
    sema->had_error = true;

    va_list args;
    va_start(args, fmt);
    diag_report_valist(DIAG_ERROR, loc, fmt, args);
    va_end(args);
}

static void scope_push(Sema* sema) {
    Scope* scope = ARENA_NEW_ZERO(sema->arena, Scope);

    scope->parent  = sema->current_scope;
    scope->entries = NULL;

    sema->current_scope = scope;
}

static void scope_pop(Sema* sema) {
    assert(sema->current_scope != NULL);

    sema->current_scope = sema->current_scope->parent;
}

static Symbol* scope_lookup_current(const Sema* sema, StrView name) {
    if (!sema->current_scope) {
        return NULL;
    }

    for (ScopeEntry* e = sema->current_scope->entries; e != NULL; e = e->next) {
        if (strview_equals(e->symbol->name, name)) {
            return e->symbol;
        }
    }

    return NULL;
}

static Symbol* scope_lookup(const Sema* sema, StrView name) {
    for (Scope* s = sema->current_scope; s != NULL; s = s->parent) {
        for (ScopeEntry* e = s->entries; e != NULL; e = e->next) {
            if (strview_equals(e->symbol->name, name)) {
                return e->symbol;
            }
        }
    }

    return NULL;
}

static Symbol* scope_define_symbol(Sema* sema, SymbolKind kind, StrView name, Type* type, SourceLoc loc) {
    Symbol* existing = scope_lookup_current(sema, name);

    if (existing) {
        sema_error(sema, loc, "redefinition of identifier '%.*s'", (int)name.len, name.data);
        return existing;
    }

    Symbol* sym = ARENA_NEW_ZERO(sema->arena, Symbol);

    sym->kind       = kind;
    sym->name       = name;
    sym->type       = type;
    sym->loc        = loc;
    sym->is_defined = true;

    ScopeEntry* entry = ARENA_NEW_ZERO(sema->arena, ScopeEntry);

    entry->symbol = sym;
    entry->next   = sema->current_scope->entries;

    sema->current_scope->entries = entry;

    return sym;
}

static void scope_add(Sema* sema, Symbol* sym) {
    if (!sym) {
        return;
    }

    ScopeEntry* entry = ARENA_NEW_ZERO(sema->arena, ScopeEntry);

    entry->symbol = sym;
    entry->next   = sema->global_scope->entries;

    sema->global_scope->entries = entry;
}

static inline bool type_is_byte_like(const Type* type) {
    if (!type) {
        return false;
    }

    return type->kind == TYPE_CHAR || type->kind == TYPE_U8;
}

static bool types_are_compatible(const Type* expected, const Type* actual) {
    if (!expected || !actual) {
        return false;
    }

    if (type_equals(expected, actual)) {
        return true;
    }

    if (expected->kind == TYPE_NULL) {
        return type_is_pointer(actual) || actual->kind == TYPE_FUNC || actual->kind == TYPE_NULL;
    }

    if (actual->kind == TYPE_NULL) {
        return type_is_pointer(expected) || expected->kind == TYPE_FUNC || expected->kind == TYPE_NULL;
    }

    if (expected->kind == TYPE_DISTINCT || actual->kind == TYPE_DISTINCT) {
        return false;
    }

    if (expected->kind == TYPE_UNION || actual->kind == TYPE_UNION) {
        return false;
    }

    if (type_is_integer(expected) && type_is_integer(actual)) {
        return true;
    }

    if (type_is_pointer(expected) && type_is_pointer(actual)) {
        if (expected->ptr.base->kind == TYPE_VOID || actual->ptr.base->kind == TYPE_VOID) {
            return true;
        }

        if (type_is_byte_like(expected->ptr.base) && type_is_byte_like(actual->ptr.base)) {
            return true;
        }

        return type_equals(expected->ptr.base, actual->ptr.base);
    }

    if (expected->kind == TYPE_FUNC && actual->kind == TYPE_PTR && actual->ptr.base->kind == TYPE_VOID) {
        return true;
    }

    if (actual->kind == TYPE_FUNC && expected->kind == TYPE_PTR && expected->ptr.base->kind == TYPE_VOID) {
        return true;
    }

    if (expected->kind == TYPE_PTR && actual->kind == TYPE_STRUCT) {
        return type_equals(expected->ptr.base, actual);
    }

    if (expected->kind == TYPE_STRUCT && actual->kind == TYPE_PTR) {
        return type_equals(expected, actual->ptr.base);
    }

    if (expected->kind == TYPE_SLICE && actual->kind == TYPE_SLICE) {
        if (type_is_byte_like(expected->slice.elem_type) && type_is_byte_like(actual->slice.elem_type)) {
            return true;
        }

        return type_equals(expected->slice.elem_type, actual->slice.elem_type);
    }

    if (expected->kind == TYPE_SLICE && actual->kind == TYPE_ARRAY) {
        if (type_is_byte_like(expected->slice.elem_type) && type_is_byte_like(actual->array.elem_type)) {
            return true;
        }

        return type_equals(expected->slice.elem_type, actual->array.elem_type);
    }

    if (expected->kind == TYPE_SLICE && type_is_byte_like(expected->slice.elem_type) &&
        actual->kind == TYPE_PTR && type_is_byte_like(actual->ptr.base)) {
        return true;
    }

    if (expected->kind == TYPE_TUPLE && actual->kind == TYPE_TUPLE) {
        if (expected->tuple.count != actual->tuple.count) {
            return false;
        }

        for (size_t i = 0; i < expected->tuple.count; ++i) {
            if (!types_are_compatible(expected->tuple.elements[i], actual->tuple.elements[i])) {
                return false;
            }
        }

        return true;
    }

    if (expected->kind == TYPE_BOOL && (type_is_integer(actual) || type_is_pointer(actual) || actual->kind == TYPE_FUNC || actual->kind == TYPE_NULL)) {
        return true;
    }

    if (actual->kind == TYPE_BOOL && (type_is_integer(expected) || expected->kind == TYPE_FUNC)) {
        return true;
    }

    return false;
}

static StrView sema_mangle_generic_name(Arena* arena, StrView base, Type** args, size_t count) {
    char buf[512];
    size_t offset = snprintf(buf, sizeof(buf), "%.*s__", (int)base.len, base.data);

    for (size_t i = 0; i < count; ++i) {
        const char* ts = type_to_str(args[i], arena);
        size_t t_len = strlen(ts);

        for (size_t c = 0; c < t_len && offset + 1 < sizeof(buf); ++c) {
            char ch = ts[c];

            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
                buf[offset++] = ch;
            } else {
                buf[offset++] = '_';
            }
        }

        if (i + 1 < count && offset + 1 < sizeof(buf)) {
            buf[offset++] = '_';
        }
    }

    buf[offset] = '\0';

    return (StrView){ .data = arena_strdup(arena, buf), .len = offset };
}

static bool type_unify(Type* pattern, Type* actual, Type** inferred, size_t param_count) {
    if (!pattern || !actual) {
        return false;
    }

    if (pattern->kind == TYPE_PARAM) {
        uint32_t idx = pattern->param.index;

        if (idx < param_count) {
            if (inferred[idx] == NULL) {
                inferred[idx] = actual;
                return true;
            }

            return type_equals(inferred[idx], actual);
        }

        return false;
    }

    if (pattern->kind == TYPE_PTR && actual->kind == TYPE_PTR) {
        return type_unify(pattern->ptr.base, actual->ptr.base, inferred, param_count);
    }

    if (pattern->kind == TYPE_SLICE && actual->kind == TYPE_SLICE) {
        return type_unify(pattern->slice.elem_type, actual->slice.elem_type, inferred, param_count);
    }

    if (pattern->kind == TYPE_ARRAY && actual->kind == TYPE_ARRAY) {
        if (pattern->array.count != actual->array.count) return false;
        return type_unify(pattern->array.elem_type, actual->array.elem_type, inferred, param_count);
    }

    if (pattern->kind == TYPE_TUPLE && actual->kind == TYPE_TUPLE) {
        if (pattern->tuple.count != actual->tuple.count) return false;

        for (size_t i = 0; i < pattern->tuple.count; ++i) {
            if (!type_unify(pattern->tuple.elements[i], actual->tuple.elements[i], inferred, param_count)) {
                return false;
            }
        }

        return true;
    }

    if (pattern->kind == TYPE_STRUCT && actual->kind == TYPE_STRUCT) {
        if (pattern->structure.generic_arg_count > 0 && actual->structure.generic_arg_count == pattern->structure.generic_arg_count) {
            StrView pat_name = pattern->structure.generic_template ? pattern->structure.generic_template->structure.name : pattern->structure.name;
            StrView act_name = actual->structure.generic_template ? actual->structure.generic_template->structure.name : actual->structure.name;

            if (strview_equals(pat_name, act_name)) {
                for (size_t i = 0; i < pattern->structure.generic_arg_count; ++i) {
                    if (!type_unify(pattern->structure.generic_args[i], actual->structure.generic_args[i], inferred, param_count)) {
                        return false;
                    }
                }
                return true;
            }
        }
    }

    return type_equals(pattern, actual);
}

static AstExpr* sema_clone_expr(Arena* arena, const AstExpr* expr);
static AstStmt* sema_clone_stmt(Arena* arena, const AstStmt* stmt);

static AstExpr* sema_clone_expr(Arena* arena, const AstExpr* expr) {
    if (!expr) return NULL;

    AstExpr* copy = ARENA_NEW_ZERO(arena, AstExpr);
    *copy = *expr;

    switch (expr->kind) {
        case EXPR_VAR:
            copy->var.symbol = NULL;
            break;

        case EXPR_UNARY:
            copy->unary.operand = sema_clone_expr(arena, expr->unary.operand);
            break;

        case EXPR_BINARY:
            copy->binary.lhs = sema_clone_expr(arena, expr->binary.lhs);
            copy->binary.rhs = sema_clone_expr(arena, expr->binary.rhs);
            break;

        case EXPR_CALL: {
            if (expr->call.callee_expr) {
                copy->call.callee_expr = sema_clone_expr(arena, expr->call.callee_expr);
            }
            if (expr->call.arg_count > 0) {
                copy->call.args = ARENA_NEW_ARRAY(arena, AstExpr*, expr->call.arg_count);
                for (size_t i = 0; i < expr->call.arg_count; ++i) {
                    copy->call.args[i] = sema_clone_expr(arena, expr->call.args[i]);
                }
            }
            break;
        }

        case EXPR_INDEX:
            copy->index.ptr   = sema_clone_expr(arena, expr->index.ptr);
            copy->index.index = sema_clone_expr(arena, expr->index.index);
            break;

        case EXPR_MEMBER:
            copy->member.target = sema_clone_expr(arena, expr->member.target);
            break;

        case EXPR_CAST:
            copy->cast.expr = sema_clone_expr(arena, expr->cast.expr);
            break;

        case EXPR_TUPLE: {
            if (expr->tuple.count > 0) {
                copy->tuple.elements = ARENA_NEW_ARRAY(arena, AstExpr*, expr->tuple.count);
                for (size_t i = 0; i < expr->tuple.count; ++i) {
                    copy->tuple.elements[i] = sema_clone_expr(arena, expr->tuple.elements[i]);
                }
            }
            break;
        }

        case EXPR_STRUCT_LIT: {
            if (expr->struct_lit.field_count > 0) {
                copy->struct_lit.field_names  = ARENA_NEW_ARRAY(arena, StrView, expr->struct_lit.field_count);
                copy->struct_lit.field_values = ARENA_NEW_ARRAY(arena, AstExpr*, expr->struct_lit.field_count);
                for (size_t i = 0; i < expr->struct_lit.field_count; ++i) {
                    copy->struct_lit.field_names[i]  = expr->struct_lit.field_names[i];
                    copy->struct_lit.field_values[i] = sema_clone_expr(arena, expr->struct_lit.field_values[i]);
                }
            }
            break;
        }

        case EXPR_ARRAY_LIT: {
            if (expr->array_lit.count > 0) {
                copy->array_lit.elements = ARENA_NEW_ARRAY(arena, AstExpr*, expr->array_lit.count);
                for (size_t i = 0; i < expr->array_lit.count; ++i) {
                    copy->array_lit.elements[i] = sema_clone_expr(arena, expr->array_lit.elements[i]);
                }
            }
            break;
        }

        case EXPR_SLICE: {
            copy->slice.target = sema_clone_expr(arena, expr->slice.target);
            if (expr->slice.start) copy->slice.start = sema_clone_expr(arena, expr->slice.start);
            if (expr->slice.end)   copy->slice.end   = sema_clone_expr(arena, expr->slice.end);
            break;
        }

        case EXPR_ALLOCA: {
            copy->alloca_expr.count_expr = sema_clone_expr(arena, expr->alloca_expr.count_expr);
            break;
        }

        case EXPR_ASM: {
            if (expr->inline_asm.input_count > 0) {
                copy->inline_asm.inputs = ARENA_NEW_ARRAY(arena, AsmOperand, expr->inline_asm.input_count);
                for (size_t i = 0; i < expr->inline_asm.input_count; ++i) {
                    copy->inline_asm.inputs[i] = expr->inline_asm.inputs[i];
                    if (expr->inline_asm.inputs[i].expr) {
                        copy->inline_asm.inputs[i].expr = sema_clone_expr(arena, expr->inline_asm.inputs[i].expr);
                    }
                }
            }
            if (expr->inline_asm.output_count > 0) {
                copy->inline_asm.outputs = ARENA_NEW_ARRAY(arena, AsmOperand, expr->inline_asm.output_count);
                for (size_t i = 0; i < expr->inline_asm.output_count; ++i) {
                    copy->inline_asm.outputs[i] = expr->inline_asm.outputs[i];
                    if (expr->inline_asm.outputs[i].expr) {
                        copy->inline_asm.outputs[i].expr = sema_clone_expr(arena, expr->inline_asm.outputs[i].expr);
                    }
                }
            }
            break;
        }

        default:
            break;
    }

    return copy;
}

static AstStmt* sema_clone_stmt(Arena* arena, const AstStmt* stmt) {
    if (!stmt) return NULL;

    AstStmt* copy = ARENA_NEW_ZERO(arena, AstStmt);
    *copy = *stmt;

    switch (stmt->kind) {
        case STMT_BLOCK: {
            if (stmt->block.count > 0) {
                copy->block.stmts = ARENA_NEW_ARRAY(arena, AstStmt*, stmt->block.count);
                for (size_t i = 0; i < stmt->block.count; ++i) {
                    copy->block.stmts[i] = sema_clone_stmt(arena, stmt->block.stmts[i]);
                }
            }
            break;
        }

        case STMT_VAR_DECL:
            if (stmt->var_decl.init_expr) {
                copy->var_decl.init_expr = sema_clone_expr(arena, stmt->var_decl.init_expr);
            }
            copy->var_decl.symbol = NULL;
            break;

        case STMT_DESTRUCTURE_DECL:
            if (stmt->destructure_decl.init_expr) {
                copy->destructure_decl.init_expr = sema_clone_expr(arena, stmt->destructure_decl.init_expr);
            }
            if (stmt->destructure_decl.count > 0) {
                copy->destructure_decl.symbols = ARENA_NEW_ARRAY_ZERO(arena, Symbol*, stmt->destructure_decl.count);
            }
            break;

        case STMT_ASSIGN:
            copy->assign.target = sema_clone_expr(arena, stmt->assign.target);
            copy->assign.value  = sema_clone_expr(arena, stmt->assign.value);
            break;

        case STMT_DESTRUCTURE_ASSIGN:
            if (stmt->destructure_assign.count > 0) {
                copy->destructure_assign.targets = ARENA_NEW_ARRAY(arena, AstExpr*, stmt->destructure_assign.count);
                for (size_t i = 0; i < stmt->destructure_assign.count; ++i) {
                    copy->destructure_assign.targets[i] = sema_clone_expr(arena, stmt->destructure_assign.targets[i]);
                }
            }
            copy->destructure_assign.value = sema_clone_expr(arena, stmt->destructure_assign.value);
            break;

        case STMT_COMPOUND_ASSIGN:
            copy->compound_assign.target = sema_clone_expr(arena, stmt->compound_assign.target);
            copy->compound_assign.value  = sema_clone_expr(arena, stmt->compound_assign.value);
            break;

        case STMT_RETURN:
            if (stmt->return_stmt.expr) {
                copy->return_stmt.expr = sema_clone_expr(arena, stmt->return_stmt.expr);
            }
            break;

        case STMT_IF:
            copy->if_stmt.cond        = sema_clone_expr(arena, stmt->if_stmt.cond);
            copy->if_stmt.then_branch = sema_clone_stmt(arena, stmt->if_stmt.then_branch);
            if (stmt->if_stmt.else_branch) {
                copy->if_stmt.else_branch = sema_clone_stmt(arena, stmt->if_stmt.else_branch);
            }
            break;

        case STMT_WHILE:
            copy->while_stmt.cond = sema_clone_expr(arena, stmt->while_stmt.cond);
            copy->while_stmt.body = sema_clone_stmt(arena, stmt->while_stmt.body);
            break;

        case STMT_FOR:
            if (stmt->for_stmt.init) copy->for_stmt.init = sema_clone_stmt(arena, stmt->for_stmt.init);
            if (stmt->for_stmt.cond) copy->for_stmt.cond = sema_clone_expr(arena, stmt->for_stmt.cond);
            if (stmt->for_stmt.step) copy->for_stmt.step = sema_clone_stmt(arena, stmt->for_stmt.step);
            copy->for_stmt.body = sema_clone_stmt(arena, stmt->for_stmt.body);
            break;

        case STMT_SWITCH:
            copy->switch_stmt.cond = sema_clone_expr(arena, stmt->switch_stmt.cond);
            if (stmt->switch_stmt.case_count > 0) {
                copy->switch_stmt.cases = ARENA_NEW_ARRAY(arena, AstSwitchCase, stmt->switch_stmt.case_count);
                for (size_t i = 0; i < stmt->switch_stmt.case_count; ++i) {
                    copy->switch_stmt.cases[i] = stmt->switch_stmt.cases[i];
                    if (stmt->switch_stmt.cases[i].pattern_count > 0) {
                        copy->switch_stmt.cases[i].patterns = ARENA_NEW_ARRAY(arena, AstCasePattern, stmt->switch_stmt.cases[i].pattern_count);
                        for (size_t p = 0; p < stmt->switch_stmt.cases[i].pattern_count; ++p) {
                            copy->switch_stmt.cases[i].patterns[p] = stmt->switch_stmt.cases[i].patterns[p];
                            copy->switch_stmt.cases[i].patterns[p].val_start = sema_clone_expr(arena, stmt->switch_stmt.cases[i].patterns[p].val_start);
                            if (stmt->switch_stmt.cases[i].patterns[p].is_range) {
                                copy->switch_stmt.cases[i].patterns[p].val_end = sema_clone_expr(arena, stmt->switch_stmt.cases[i].patterns[p].val_end);
                            }
                        }
                    }
                    if (stmt->switch_stmt.cases[i].stmt_count > 0) {
                        copy->switch_stmt.cases[i].stmts = ARENA_NEW_ARRAY(arena, AstStmt*, stmt->switch_stmt.cases[i].stmt_count);
                        for (size_t s = 0; s < stmt->switch_stmt.cases[i].stmt_count; ++s) {
                            copy->switch_stmt.cases[i].stmts[s] = sema_clone_stmt(arena, stmt->switch_stmt.cases[i].stmts[s]);
                        }
                    }
                }
            }
            break;

        case STMT_DEFER:
            copy->defer_stmt.stmt = sema_clone_stmt(arena, stmt->defer_stmt.stmt);
            break;

        case STMT_EXPR:
            copy->expr_stmt.expr = sema_clone_expr(arena, stmt->expr_stmt.expr);
            break;

        default:
            break;
    }

    return copy;
}

static Type* sema_bind_type_params(Arena* arena, Type* type, const TypeParamInfo* params, size_t param_count, uint32_t depth) {
    if (!type || !params || param_count == 0) {
        return type;
    }

    switch (type->kind) {
        case TYPE_STRUCT: {
            if (type->structure.generic_arg_count > 0) {
                bool changed = false;
                Type** new_args = ARENA_NEW_ARRAY(arena, Type*, type->structure.generic_arg_count);

                for (size_t i = 0; i < type->structure.generic_arg_count; ++i) {
                    new_args[i] = sema_bind_type_params(arena, type->structure.generic_args[i], params, param_count, depth);

                    if (new_args[i] != type->structure.generic_args[i]) {
                        changed = true;
                    }
                }

                if (changed) {
                    Type* copy = ARENA_NEW_ZERO(arena, Type);
                    *copy = *type;
                    copy->structure.generic_args = new_args;
                    return copy;
                }

                return type;
            }

            for (size_t i = 0; i < param_count; ++i) {
                if (strview_equals(params[i].name, type->structure.name)) {
                    return type_param_create(arena, depth, (uint32_t)i, params[i].name, NULL);
                }
            }

            return type;
        }

        case TYPE_PTR: {
            Type* base = sema_bind_type_params(arena, type->ptr.base, params, param_count, depth);

            if (base != type->ptr.base) {
                return type_ptr(arena, base);
            }

            return type;
        }

        case TYPE_ARRAY: {
            Type* elem = sema_bind_type_params(arena, type->array.elem_type, params, param_count, depth);

            if (elem != type->array.elem_type) {
                return type_array_create(arena, elem, type->array.count);
            }

            return type;
        }

        case TYPE_SLICE: {
            Type* elem = sema_bind_type_params(arena, type->slice.elem_type, params, param_count, depth);

            if (elem != type->slice.elem_type) {
                return type_slice_create(arena, elem);
            }

            return type;
        }

        case TYPE_TUPLE: {
            bool changed = false;
            Type** new_elems = ARENA_NEW_ARRAY(arena, Type*, type->tuple.count);

            for (size_t i = 0; i < type->tuple.count; ++i) {
                new_elems[i] = sema_bind_type_params(arena, type->tuple.elements[i], params, param_count, depth);

                if (new_elems[i] != type->tuple.elements[i]) {
                    changed = true;
                }
            }

            if (changed) {
                return type_tuple_create(arena, new_elems, type->tuple.count);
            }

            return type;
        }

        case TYPE_FUNC: {
            Type* ret = sema_bind_type_params(arena, type->func.return_type, params, param_count, depth);
            bool changed = (ret != type->func.return_type);

            Type** new_params = ARENA_NEW_ARRAY(arena, Type*, type->func.param_count);

            for (size_t i = 0; i < type->func.param_count; ++i) {
                new_params[i] = sema_bind_type_params(arena, type->func.param_types[i], params, param_count, depth);

                if (new_params[i] != type->func.param_types[i]) {
                    changed = true;
                }
            }

            if (changed) {
                return type_func_create(arena, ret, new_params, type->func.param_count, type->func.is_variadic);
            }

            return type;
        }

        default:
            return type;
    }
}

static bool expr_is_lvalue(const AstExpr* expr) {
    if (!expr) {
        return false;
    }

    switch (expr->kind) {
        case EXPR_VAR:
            return expr->var.symbol != NULL && expr->var.symbol->kind != SYM_CONST;

        case EXPR_UNARY:
            return expr->unary.op == TOK_STAR;

        case EXPR_INDEX:
        case EXPR_MEMBER:
            return true;

        default:
            return false;
    }
}

static Type* sema_resolve_type(Sema* sema, Type* type) {
    if (!type) {
        return NULL;
    }

    if (type->kind == TYPE_PARAM) {
        if (sema->current_subst_env) {
            return type_subst(sema->arena, type, sema->current_subst_env);
        }
        return type;
    }

    if (type->kind == TYPE_DISTINCT) {
        Type* res_base = sema_resolve_type(sema, type->distinct_type.base);
        type->distinct_type.base = res_base;
        type->size  = res_base->size;
        type->align = res_base->align;
        return type;
    }

    if (type->kind == TYPE_PTR) {
        Type* resolved_base = sema_resolve_type(sema, type->ptr.base);

        if (resolved_base != type->ptr.base) {
            return type_ptr(sema->arena, resolved_base);
        }

        return type;
    }

    if (type->kind == TYPE_ARRAY) {
        Type* resolved_elem = sema_resolve_type(sema, type->array.elem_type);

        if (resolved_elem != type->array.elem_type) {
            return type_array_create(sema->arena, resolved_elem, type->array.count);
        }

        return type;
    }

    if (type->kind == TYPE_SLICE) {
        Type* resolved_elem = sema_resolve_type(sema, type->slice.elem_type);

        if (resolved_elem != type->slice.elem_type) {
            return type_slice_create(sema->arena, resolved_elem);
        }

        return type;
    }

    if (type->kind == TYPE_TUPLE) {
        Type** res_elements = ARENA_NEW_ARRAY(sema->arena, Type*, type->tuple.count);

        for (size_t i = 0; i < type->tuple.count; ++i) {
            res_elements[i] = sema_resolve_type(sema, type->tuple.elements[i]);
        }

        return type_tuple_create(sema->arena, res_elements, type->tuple.count);
    }

    if (type->kind == TYPE_STRUCT) {
        if (type->structure.generic_arg_count > 0) {
            StrView template_name = type->structure.generic_template
                                        ? type->structure.generic_template->structure.name
                                        : type->structure.name;

            Type** concrete_args = ARENA_NEW_ARRAY(sema->arena, Type*, type->structure.generic_arg_count);

            for (size_t i = 0; i < type->structure.generic_arg_count; ++i) {
                concrete_args[i] = sema_resolve_type(sema, type->structure.generic_args[i]);
            }

            for (StructInstanceCache* c = sema->struct_instances; c != NULL; c = c->next) {
                if (strview_equals(c->template_type->structure.name, template_name) &&
                    c->arg_count == type->structure.generic_arg_count) {

                    bool match = true;

                    for (size_t a = 0; a < c->arg_count; ++a) {
                        if (!type_equals(c->args[a], concrete_args[a])) {
                            match = false;
                            break;
                        }
                    }

                    if (match) {
                        return c->instantiated_type;
                    }
                }
            }

            GenericStructTemplate* templ = NULL;

            for (GenericStructTemplate* gt = sema->generic_struct_templates; gt != NULL; gt = gt->next) {
                if (strview_equals(gt->name, template_name)) {
                    templ = gt;
                    break;
                }
            }

            if (!templ) {
                sema_error(sema, (SourceLoc){0}, "unknown generic struct '%.*s'",
                           (int)template_name.len, template_name.data);
                return type_primitive(TYPE_I64);
            }

            AstStructDef* s_def = templ->def;

            if (s_def->generic_param_count != type->structure.generic_arg_count) {
                sema_error(sema, (SourceLoc){0}, "generic struct '%.*s' expects %zu type arguments, got %zu",
                           (int)s_def->name.len, s_def->name.data,
                           s_def->generic_param_count, type->structure.generic_arg_count);
                return type_primitive(TYPE_I64);
            }

            StrView mangled_name = sema_mangle_generic_name(sema->arena, s_def->name, concrete_args, type->structure.generic_arg_count);

            Type* inst_type = ARENA_NEW_ZERO(sema->arena, Type);
            inst_type->kind                        = TYPE_STRUCT;
            inst_type->structure.name             = mangled_name;
            inst_type->structure.generic_template = s_def->type;
            inst_type->structure.generic_args     = concrete_args;
            inst_type->structure.generic_arg_count = type->structure.generic_arg_count;
            inst_type->size                       = 8;
            inst_type->align                      = 8;

            StructInstanceCache* cache_node = ARENA_NEW_ZERO(sema->arena, StructInstanceCache);
            cache_node->template_type      = s_def->type;
            cache_node->args               = concrete_args;
            cache_node->arg_count          = type->structure.generic_arg_count;
            cache_node->instantiated_type  = inst_type;
            cache_node->next               = sema->struct_instances;

            sema->struct_instances = cache_node;

            TypeSubstEnv env = {
                .depth          = 0,
                .concrete_types = concrete_args,
                .count          = type->structure.generic_arg_count,
                .parent         = NULL
            };

            StructField* inst_fields = ARENA_NEW_ARRAY(sema->arena, StructField, s_def->field_count);

            for (size_t f = 0; f < s_def->field_count; ++f) {
                inst_fields[f].name                = s_def->fields[f].name;
                inst_fields[f].default_value       = s_def->fields[f].default_value;
                inst_fields[f].offset              = 0;
                inst_fields[f].has_explicit_offset = s_def->fields[f].has_explicit_offset; 
                inst_fields[f].explicit_offset     = s_def->fields[f].explicit_offset;     

                Type* substituted = type_subst(sema->arena, s_def->fields[f].type, &env);
                inst_fields[f].type = sema_resolve_type(sema, substituted);
            }

            type_struct_init(inst_type, mangled_name, inst_fields, s_def->field_count, s_def->is_packed);

            return inst_type;
        }

        if (sema->current_subst_env) {
            if (sema->current_proc && sema->current_proc->generic_template) {
                const AstProc* gt = sema->current_proc->generic_template;

                for (size_t i = 0; i < gt->generic_param_count; ++i) {
                    if (strview_equals(gt->generic_params[i].name, type->structure.name)) {
                        if (i < sema->current_subst_env->count) {
                            return sema->current_subst_env->concrete_types[i];
                        }
                    }
                }
            }

            for (const TypeSubstEnv* env = sema->current_subst_env; env != NULL; env = env->parent) {
                for (size_t i = 0; i < env->count; ++i) {
                    if (env->concrete_types[i] && env->concrete_types[i]->kind == TYPE_PARAM) {
                        if (strview_equals(env->concrete_types[i]->param.name, type->structure.name)) {
                            return env->concrete_types[i];
                        }
                    }
                }
            }
        }

        for (StructTypeEntry* e = sema->struct_registry; e != NULL; e = e->next) {
            if (strview_equals(e->name, type->structure.name)) {
                return e->type;
            }
        }

        for (StructInstanceCache* c = sema->struct_instances; c != NULL; c = c->next) {
            if (strview_equals(c->instantiated_type->structure.name, type->structure.name)) {
                return c->instantiated_type;
            }
        }

        for (UnionTypeEntry* e = sema->union_registry; e != NULL; e = e->next) {
            if (strview_equals(e->name, type->structure.name)) {
                return e->type;
            }
        }

        for (EnumTypeEntry* e = sema->enum_registry; e != NULL; e = e->next) {
            if (strview_equals(e->name, type->structure.name)) {
                return e->type;
            }
        }

        for (TypeAliasEntry* e = sema->alias_registry; e != NULL; e = e->next) {
            if (strview_equals(e->name, type->structure.name)) {
                if (e->is_resolving) {
                    sema_error(sema, (SourceLoc){0}, "cyclic type alias definition '%.*s'", (int)e->name.len, e->name.data);
                    return type_primitive(TYPE_I64);
                }

                if (!e->type) {
                    return type;
                }

                e->is_resolving = true;
                Type* resolved = sema_resolve_type(sema, e->type);
                e->is_resolving = false;

                if (e->is_distinct) {
                    return type_distinct_create(sema->arena, e->name, resolved);
                }

                return resolved;
            }
        }

        sema_error(sema, type->loc, "unknown type '%.*s'", (int)type->structure.name.len, type->structure.name.data);
    }

    if (type->kind == TYPE_FUNC) {
        Type* res_ret = sema_resolve_type(sema, type->func.return_type);
        Type** res_params = ARENA_NEW_ARRAY(sema->arena, Type*, type->func.param_count);

        for (size_t i = 0; i < type->func.param_count; ++i) {
            res_params[i] = sema_resolve_type(sema, type->func.param_types[i]);
        }

        return type_func_create(sema->arena, res_ret, res_params, type->func.param_count, type->func.is_variadic);
    }

    return type;
}

static Type* sema_resolve_type(Sema* sema, Type* type);
static Type* sema_analyze_expr(Sema* sema, AstExpr* expr, Type* expected_type);
static void  sema_analyze_stmt(Sema* sema, AstStmt* stmt);
static void  sema_analyze_block(Sema* sema, AstStmt* block_stmt);
static void  sema_analyze_proc_body(Sema* sema, AstProc* proc);

static Symbol* sema_get_or_instantiate_generic_proc(Sema* sema, AstProc* template_proc, Type** concrete_args, size_t arg_count, SourceLoc loc) {
    StrView mangled_name = sema_mangle_generic_name(sema->arena, template_proc->name, concrete_args, arg_count);

    for (ProcInstanceCache* c = sema->proc_instances; c != NULL; c = c->next) {
        if (strview_equals(c->def_template->name, template_proc->name) && c->arg_count == arg_count) {
            bool match = true;
            for (size_t a = 0; a < c->arg_count; ++a) {
                if (!type_equals(c->args[a], concrete_args[a])) {
                    match = false;
                    break;
                }
            }

            if (match) {
                Symbol* existing_sym = scope_lookup(sema, mangled_name);
                if (existing_sym) {
                    return existing_sym;
                }
            }
        }
    }

    AstProc* inst_proc = ARENA_NEW_ZERO(sema->arena, AstProc);
    inst_proc->name                = mangled_name;
    inst_proc->method_struct       = template_proc->method_struct;
    inst_proc->generic_params      = NULL;
    inst_proc->generic_param_count = 0;
    inst_proc->is_generic          = false;
    inst_proc->params              = ARENA_NEW_ARRAY(sema->arena, AstParam, template_proc->param_count);
    inst_proc->param_count         = template_proc->param_count;

    TypeSubstEnv env = {
        .depth          = (template_proc->method_struct.len > 0) ? 1 : 0,
        .concrete_types = concrete_args,
        .count          = arg_count,
        .parent         = NULL
    };

    const TypeSubstEnv* prev_env = sema->current_subst_env;
    sema->current_subst_env = &env;

    for (size_t p = 0; p < template_proc->param_count; ++p) {
        inst_proc->params[p].name = template_proc->params[p].name;
        inst_proc->params[p].loc  = template_proc->params[p].loc;
        inst_proc->params[p].type = sema_resolve_type(sema, template_proc->params[p].type);
    }

    inst_proc->return_type      = sema_resolve_type(sema, template_proc->return_type);
    inst_proc->body             = sema_clone_stmt(sema->arena, template_proc->body);
    inst_proc->loc              = loc;
    inst_proc->attrs            = template_proc->attrs;
    inst_proc->generic_template = template_proc;

    Type** param_types = ARENA_NEW_ARRAY(sema->arena, Type*, template_proc->param_count);
    for (size_t p = 0; p < template_proc->param_count; ++p) {
        param_types[p] = inst_proc->params[p].type;
    }

    Type* proc_type = type_func_create(sema->arena, inst_proc->return_type, param_types, inst_proc->param_count, inst_proc->is_variadic);

    Symbol* inst_sym = ARENA_NEW_ZERO(sema->arena, Symbol);
    inst_sym->kind       = SYM_PROC;
    inst_sym->name       = inst_proc->name;
    inst_sym->type       = proc_type;
    inst_sym->loc        = inst_proc->loc;
    inst_sym->is_defined = true;
    inst_sym->is_extern  = inst_proc->attrs.is_extern;
    inst_sym->attrs      = inst_proc->attrs;
    inst_sym->proc_decl  = inst_proc;

    inst_proc->symbol = inst_sym;

    scope_add(sema, inst_sym);

    ProcInstanceCache* cache_node = ARENA_NEW_ZERO(sema->arena, ProcInstanceCache);
    cache_node->def_template      = template_proc;
    cache_node->args              = concrete_args;
    cache_node->arg_count         = arg_count;
    cache_node->instantiated_proc = inst_proc;
    cache_node->next              = sema->proc_instances;

    sema->proc_instances = cache_node;

    if (sema->current_program) {
        ARENA_DA_PUSH(sema->arena, sema->current_program->procs,
                      sema->current_program->proc_count,
                      sema->current_program->proc_cap, inst_proc);
    }

    sema_analyze_proc_body(sema, inst_proc);

    sema->current_subst_env = prev_env;

    return inst_sym;
}

static Type* sema_analyze_expr(Sema* sema, AstExpr* expr, Type* expected_type) {
    if (!expr) {
        return type_primitive(TYPE_VOID);
    }

    switch (expr->kind) {
        case EXPR_INT_LIT: {
            if (expected_type && type_is_integer(expected_type)) {
                expr->type = expected_type;
            } else if (expected_type && (type_is_pointer(expected_type) || expected_type->kind == TYPE_FUNC) && expr->int_val == 0) {
                expr->type = expected_type;
            } else if (expr->type) {
                return expr->type;
            } else {
                expr->type = type_primitive(TYPE_I64);
            }

            return expr->type;
        }

        case EXPR_STRING_LIT: {
            if (expected_type && expected_type->kind == TYPE_SLICE &&
                expected_type->slice.elem_type->kind == TYPE_CHAR) {
                expr->type = expected_type;
                return expr->type;
            }

            Type* char_type = type_primitive(TYPE_CHAR);
            expr->type = type_ptr(sema->arena, char_type);

            return expr->type;
        }

        case EXPR_NULL: {
            if (expected_type && (type_is_pointer(expected_type) || expected_type->kind == TYPE_FUNC)) {
                expr->type = expected_type;
            } else {
                expr->type = type_primitive(TYPE_NULL);
            }

            return expr->type;
        }

        case EXPR_VAR: {
            Symbol* sym = scope_lookup(sema, expr->var.name);

            if (!sym) {
                sema_error(sema, expr->loc, "use of undeclared identifier '%.*s'",
                           (int)expr->var.name.len, expr->var.name.data);
                expr->type = type_primitive(TYPE_I64);
                return expr->type;
            }

            expr->var.symbol = sym;
            expr->type       = sym->type;

            return expr->type;
        }

        case EXPR_UNARY: {
            Type* op_type = sema_analyze_expr(sema, expr->unary.operand, NULL);

            if (expr->unary.op == TOK_STAR) {
                if (!type_is_pointer(op_type)) {
                    sema_error(sema, expr->loc, "cannot dereference non-pointer type '%s'",
                               type_to_str(op_type, sema->arena));
                    expr->type = type_primitive(TYPE_I64);
                    return expr->type;
                }

                expr->type = op_type->ptr.base;
                return expr->type;
            }

            if (expr->unary.op == TOK_AMP) {
                if (!expr_is_lvalue(expr->unary.operand)) {
                    sema_error(sema, expr->loc, "cannot take address of non-lvalue expression");
                }

                expr->type = type_ptr(sema->arena, op_type);
                return expr->type;
            }

            if (expr->unary.op == TOK_MINUS || expr->unary.op == TOK_PLUS || expr->unary.op == TOK_TILDE) {
                if (!type_is_integer(op_type)) {
                    sema_error(sema, expr->loc, "unary operator requires integer operand, got '%s'",
                               type_to_str(op_type, sema->arena));
                }

                expr->type = type_integer_promote(op_type);
                return expr->type;
            }

            if (expr->unary.op == TOK_BANG) {
                expr->type = type_primitive(TYPE_BOOL);
                return expr->type;
            }

            expr->type = op_type;
            return expr->type;
        }

        case EXPR_BINARY: {
            Type* lhs_type = sema_analyze_expr(sema, expr->binary.lhs, NULL);
            Type* rhs_expected = type_is_pointer(lhs_type) ? type_primitive(TYPE_I64) : lhs_type;
            Type* rhs_type = sema_analyze_expr(sema, expr->binary.rhs, rhs_expected);

            switch (expr->binary.op) {
                case TOK_AMP_AMP:
                case TOK_PIPE_PIPE: {
                    expr->type = type_primitive(TYPE_BOOL);
                    return expr->type;
                }

                case TOK_EQ_EQ:
                case TOK_BANG_EQ:
                case TOK_LESS:
                case TOK_LESS_EQ:
                case TOK_GREATER:
                case TOK_GREATER_EQ: {
                    if (!types_are_compatible(lhs_type, rhs_type)) {
                        sema_error(sema, expr->loc, "comparison between incompatible types '%s' and '%s'",
                                   type_to_str(lhs_type, sema->arena),
                                   type_to_str(rhs_type, sema->arena));
                    }

                    expr->type = type_primitive(TYPE_BOOL);
                    return expr->type;
                }

                case TOK_SHL:
                case TOK_SHR: {
                    if (!type_is_integer(lhs_type) || !type_is_integer(rhs_type)) {
                        sema_error(sema, expr->loc, "shift operator requires integer types, got '%s' and '%s'",
                                   type_to_str(lhs_type, sema->arena),
                                   type_to_str(rhs_type, sema->arena));
                    }

                    expr->type = type_integer_promote(lhs_type);
                    return expr->type;
                }

                case TOK_PLUS:
                case TOK_MINUS: {
                    if (type_is_pointer(lhs_type) && type_is_integer(rhs_type)) {
                        expr->type = lhs_type;
                        return expr->type;
                    }

                    if (!types_are_compatible(lhs_type, rhs_type)) {
                        sema_error(sema, expr->loc, "binary operator requires compatible types, got '%s' and '%s'",
                                   type_to_str(lhs_type, sema->arena),
                                   type_to_str(rhs_type, sema->arena));
                    }

                    expr->type = type_common_arithmetic(lhs_type, rhs_type);
                    return expr->type;
                }

                case TOK_STAR:
                case TOK_SLASH:
                case TOK_PERCENT:
                case TOK_AMP:
                case TOK_PIPE:
                case TOK_CARET: {
                    if (!type_is_integer(lhs_type) || !type_is_integer(rhs_type)) {
                        sema_error(sema, expr->loc, "bitwise/arithmetic operator requires integer types, got '%s' and '%s'",
                                   type_to_str(lhs_type, sema->arena),
                                   type_to_str(rhs_type, sema->arena));
                    }

                    expr->type = type_common_arithmetic(lhs_type, rhs_type);
                    return expr->type;
                }

                default:
                    break;
            }

            expr->type = lhs_type;
            return expr->type;
        }

        case EXPR_INDEX: {
            Type* ptr_type = sema_analyze_expr(sema, expr->index.ptr, NULL);
            Type* idx_type = sema_analyze_expr(sema, expr->index.index, type_primitive(TYPE_U64));

            if (ptr_type->kind == TYPE_ARRAY) {
                expr->type = ptr_type->array.elem_type;
                return expr->type;
            }

            if (ptr_type->kind == TYPE_SLICE) {
                if (!type_is_integer(idx_type)) {
                    sema_error(sema, expr->loc, "slice index must be an integer, got '%s'",
                               type_to_str(idx_type, sema->arena));
                }

                expr->type = ptr_type->slice.elem_type;
                return expr->type;
            }

            if (!type_is_pointer(ptr_type)) {
                sema_error(sema, expr->loc, "subscripted value is not an array or pointer, got '%s'",
                           type_to_str(ptr_type, sema->arena));
                expr->type = type_primitive(TYPE_I64);
                return expr->type;
            }

            if (!type_is_integer(idx_type)) {
                sema_error(sema, expr->loc, "array index must be an integer, got '%s'",
                           type_to_str(idx_type, sema->arena));
            }

            expr->type = ptr_type->ptr.base;
            return expr->type;
        }

        case EXPR_CAST: {
            expr->cast.target_type = sema_resolve_type(sema, expr->cast.target_type);
            sema_analyze_expr(sema, expr->cast.expr, expr->cast.target_type);
            expr->type = expr->cast.target_type;
            return expr->type;
        }

        case EXPR_SIZEOF: {
            Type* target = sema_resolve_type(sema, expr->size_align_of.target_type);

            if (!target) {
                sema_error(sema, expr->loc, "cannot evaluate sizeof on unknown type");
                expr->type = type_primitive(TYPE_U64);
                return expr->type;
            }

            expr->kind    = EXPR_INT_LIT;
            expr->int_val = (int64_t)target->size;
            expr->type    = type_primitive(TYPE_U64);

            return expr->type;
        }

        case EXPR_ALIGNOF: {
            Type* target = sema_resolve_type(sema, expr->size_align_of.target_type);

            if (!target) {
                sema_error(sema, expr->loc, "cannot evaluate alignof on unknown type");
                expr->type = type_primitive(TYPE_U64);
                return expr->type;
            }

            expr->kind    = EXPR_INT_LIT;
            expr->int_val = (int64_t)(target->align ? target->align : 1);
            expr->type    = type_primitive(TYPE_U64);

            return expr->type;
        }

        case EXPR_OFFSETOF: {
            Type* struct_type = sema_resolve_type(sema, expr->offset_of.struct_type);

            if (type_is_pointer(struct_type)) {
                struct_type = struct_type->ptr.base;
            }

            if (!struct_type || struct_type->kind != TYPE_STRUCT) {
                sema_error(sema, expr->loc, "offsetof requires struct type, got '%s'",
                           type_to_str(struct_type, sema->arena));
                expr->type = type_primitive(TYPE_U64);
                return expr->type;
            }

            StructField* field = type_struct_lookup_field(struct_type, expr->offset_of.field_name);

            if (!field) {
                sema_error(sema, expr->loc, "struct '%.*s' has no field named '%.*s'",
                           (int)struct_type->structure.name.len, struct_type->structure.name.data,
                           (int)expr->offset_of.field_name.len, expr->offset_of.field_name.data);
                expr->type = type_primitive(TYPE_U64);
                return expr->type;
            }

            expr->kind    = EXPR_INT_LIT;
            expr->int_val = (int64_t)field->offset;
            expr->type    = type_primitive(TYPE_U64);

            return expr->type;
        }

        case EXPR_ALLOCA: {
            Type* elem_t = sema_resolve_type(sema, expr->alloca_expr.elem_type);

            if (!elem_t || elem_t->kind == TYPE_VOID || elem_t->size == 0) {
                sema_error(sema, expr->loc, "cannot allocate zero-sized or void type with alloca");
                expr->type = type_ptr(sema->arena, type_primitive(TYPE_U8));
                return expr->type;
            }

            Type* count_t = sema_analyze_expr(sema, expr->alloca_expr.count_expr, type_primitive(TYPE_U64));

            if (!type_is_integer(count_t)) {
                sema_error(sema, expr->alloca_expr.count_expr->loc, "alloca count must be an integer, got '%s'",
                           type_to_str(count_t, sema->arena));
            }

            expr->alloca_expr.elem_type = elem_t;
            expr->type                  = type_ptr(sema->arena, elem_t);

            return expr->type;
        }

        case EXPR_ASM: {
            for (size_t i = 0; i < expr->inline_asm.input_count; ++i) {
                AsmOperand* in_op = &expr->inline_asm.inputs[i];
                size_t reg_bytes = 0;
                X86Reg r = parse_reg_name(in_op->reg_name, &reg_bytes);

                if (r == REG_NONE) {
                    sema_error(sema, in_op->loc, "unknown register \"%.*s\" in asm input",
                               (int)in_op->reg_name.len, in_op->reg_name.data);
                }

                in_op->reg       = r;
                in_op->byte_size = reg_bytes;

                Type* in_t = sema_analyze_expr(sema, in_op->expr, NULL);
                size_t val_bytes = in_t->size ? in_t->size : 8;

                if (reg_bytes > 0 && val_bytes > reg_bytes) {
                    sema_error(sema, in_op->loc, "expression of size %zu does not fit in register \"%.*s\" (%zu bytes)",
                               val_bytes, (int)in_op->reg_name.len, in_op->reg_name.data, reg_bytes);
                }
            }

            for (size_t i = 0; i < expr->inline_asm.output_count; ++i) {
                AsmOperand* out_op = &expr->inline_asm.outputs[i];
                size_t reg_bytes = 0;
                X86Reg r = parse_reg_name(out_op->reg_name, &reg_bytes);

                if (r == REG_NONE) {
                    sema_error(sema, out_op->loc, "unknown register \"%.*s\" in asm output",
                               (int)out_op->reg_name.len, out_op->reg_name.data);
                }

                out_op->reg       = r;
                out_op->byte_size = reg_bytes;

                if (out_op->expr != NULL) {
                    Type* out_t = sema_analyze_expr(sema, out_op->expr, NULL);

                    if (!expr_is_lvalue(out_op->expr)) {
                        sema_error(sema, out_op->loc, "asm output target is not a valid lvalue");
                    }

                    size_t target_bytes = out_t->size ? out_t->size : 8;

                    if (reg_bytes > 0 && target_bytes > reg_bytes) {
                        sema_error(sema, out_op->loc, "variable of size %zu cannot receive from register \"%.*s\" (%zu bytes)",
                                   target_bytes, (int)out_op->reg_name.len, out_op->reg_name.data, reg_bytes);
                    }
                }
            }

            if (expr->inline_asm.explicit_type) {
                expr->inline_asm.explicit_type = sema_resolve_type(sema, expr->inline_asm.explicit_type);
                expr->type = expr->inline_asm.explicit_type;
            } else if (expected_type && expected_type->kind != TYPE_VOID) {
                expr->type = expected_type;
            } else if (expr->inline_asm.output_count > 0 && expr->inline_asm.outputs[0].expr == NULL) {
                size_t r_size = expr->inline_asm.outputs[0].byte_size;
                expr->type = (r_size == 1) ? type_primitive(TYPE_U8) :
                             (r_size == 2) ? type_primitive(TYPE_U16) :
                             (r_size == 4) ? type_primitive(TYPE_U32) : type_primitive(TYPE_U64);
            } else {
                expr->type = type_primitive(TYPE_VOID);
            }

            return expr->type;
        }

        case EXPR_MEMBER: {
            Type* target_type = sema_analyze_expr(sema, expr->member.target, NULL);

            if (target_type && target_type->kind == TYPE_SLICE) {
                if (expr->member.field_name.len == 3 && memcmp(expr->member.field_name.data, "ptr", 3) == 0) {
                    expr->type = type_ptr(sema->arena, target_type->slice.elem_type);
                    return expr->type;
                }

                if (expr->member.field_name.len == 3 && memcmp(expr->member.field_name.data, "len", 3) == 0) {
                    expr->type = type_primitive(TYPE_U64);
                    return expr->type;
                }

                sema_error(sema, expr->loc, "slice type '%s' has no field named '%.*s' (expected 'ptr' or 'len')",
                           type_to_str(target_type, sema->arena),
                           (int)expr->member.field_name.len, expr->member.field_name.data);
                expr->type = type_primitive(TYPE_I64);
                return expr->type;
            }

            if (target_type && target_type->kind == TYPE_ENUM) {
                EnumVariant* v = type_enum_lookup_variant(target_type, expr->member.field_name);

                if (!v) {
                    sema_error(sema, expr->loc, "enum '%.*s' has no variant named '%.*s'",
                               (int)target_type->enumeration.name.len, target_type->enumeration.name.data,
                               (int)expr->member.field_name.len, expr->member.field_name.data);
                    expr->type = target_type;
                    return expr->type;
                }

                expr->kind    = EXPR_INT_LIT;
                expr->int_val = v->value;
                expr->type    = target_type;

                return expr->type;
            }

            Type* struct_type = target_type;

            if (type_is_pointer(target_type)) {
                struct_type = target_type->ptr.base;
            }

            struct_type = sema_resolve_type(sema, struct_type);

            if (!struct_type || (struct_type->kind != TYPE_STRUCT && struct_type->kind != TYPE_UNION)) {
                sema_error(sema, expr->loc, "member access on non-aggregate type '%s'",
                           type_to_str(target_type, sema->arena));
                expr->type = type_primitive(TYPE_I64);
                return expr->type;
            }

            StructField* field = type_struct_lookup_field(struct_type, expr->member.field_name);

            if (!field) {
                sema_error(sema, expr->loc, "aggregate '%.*s' has no field named '%.*s'",
                           (int)struct_type->structure.name.len, struct_type->structure.name.data,
                           (int)expr->member.field_name.len, expr->member.field_name.data);
                expr->type = type_primitive(TYPE_I64);
                return expr->type;
            }

            expr->member.field = field;
            expr->type         = field->type;
            return expr->type;
        }

        case EXPR_STRUCT_LIT: {
            Type* struct_type = NULL;

            for (StructTypeEntry* e = sema->struct_registry; e != NULL; e = e->next) {
                if (strview_equals(e->name, expr->struct_lit.struct_name)) {
                    struct_type = e->type;
                    break;
                }
            }

            if (!struct_type) {
                for (UnionTypeEntry* e = sema->union_registry; e != NULL; e = e->next) {
                    if (strview_equals(e->name, expr->struct_lit.struct_name)) {
                        struct_type = e->type;
                        break;
                    }
                }
            }

            if (!struct_type) {
                for (StructInstanceCache* c = sema->struct_instances; c != NULL; c = c->next) {
                    if (strview_equals(c->instantiated_type->structure.name, expr->struct_lit.struct_name)) {
                        struct_type = c->instantiated_type;
                        break;
                    }
                }
            }

            if (!struct_type && expr->struct_lit.type_arg_count > 0) {
                Type* generic_type = ARENA_NEW_ZERO(sema->arena, Type);
                generic_type->kind                        = TYPE_STRUCT;
                generic_type->structure.name              = expr->struct_lit.struct_name;
                generic_type->structure.generic_args      = expr->struct_lit.type_args;
                generic_type->structure.generic_arg_count = expr->struct_lit.type_arg_count;
                generic_type->size                        = 8;
                generic_type->align                       = 8;

                struct_type = sema_resolve_type(sema, generic_type);
            }

            if (!struct_type) {
                sema_error(sema, expr->loc, "unknown aggregate type '%.*s'",
                           (int)expr->struct_lit.struct_name.len, expr->struct_lit.struct_name.data);
                expr->type = type_primitive(TYPE_I64);
                return expr->type;
            }

            expr->struct_lit.struct_type = struct_type;

            for (size_t i = 0; i < expr->struct_lit.field_count; ++i) {
                StructField* f = type_struct_lookup_field(struct_type, expr->struct_lit.field_names[i]);

                if (!f) {
                    sema_error(sema, expr->loc, "aggregate '%.*s' has no field named '%.*s'",
                               (int)struct_type->structure.name.len, struct_type->structure.name.data,
                               (int)expr->struct_lit.field_names[i].len, expr->struct_lit.field_names[i].data);
                    continue;
                }

                for (size_t prev = 0; prev < i; ++prev) {
                    if (strview_equals(expr->struct_lit.field_names[prev], expr->struct_lit.field_names[i])) {
                        sema_error(sema, expr->loc, "duplicate field '%.*s' in struct literal",
                                   (int)expr->struct_lit.field_names[i].len, expr->struct_lit.field_names[i].data);
                        break;
                    }
                }

                sema_analyze_expr(sema, expr->struct_lit.field_values[i], f->type);
            }

            expr->type = struct_type;
            return expr->type;
        }

        case EXPR_ARRAY_LIT: {
            Type* expected_elem = NULL;

            if (expected_type) {
                if (expected_type->kind == TYPE_ARRAY) {
                    expected_elem = expected_type->array.elem_type;
                } else if (expected_type->kind == TYPE_SLICE) {
                    expected_elem = expected_type->slice.elem_type;
                }
            }

            if (expr->array_lit.count == 0) {
                if (!expected_elem) {
                    expected_elem = type_primitive(TYPE_VOID);
                }

                expr->type = type_array_create(sema->arena, expected_elem, 0);
                return expr->type;
            }

            Type** elem_types = ARENA_NEW_ARRAY(sema->arena, Type*, expr->array_lit.count);

            for (size_t i = 0; i < expr->array_lit.count; ++i) {
                elem_types[i] = sema_analyze_expr(sema, expr->array_lit.elements[i], expected_elem);

                if (!expected_elem && elem_types[i]->kind != TYPE_VOID) {
                    expected_elem = elem_types[i];
                }
            }

            if (!expected_elem) {
                expected_elem = type_primitive(TYPE_I64);
            }

            for (size_t i = 0; i < expr->array_lit.count; ++i) {
                if (!types_are_compatible(expected_elem, elem_types[i])) {
                    sema_error(sema, expr->array_lit.elements[i]->loc,
                               "array element %zu has type '%s', incompatible with expected element type '%s'",
                               i + 1,
                               type_to_str(elem_types[i], sema->arena),
                               type_to_str(expected_elem, sema->arena));
                }
            }

            if (expected_type && expected_type->kind == TYPE_ARRAY && expected_type->array.count != expr->array_lit.count) {
                sema_error(sema, expr->loc,
                           "array literal has %zu elements, but expected array type has %zu elements",
                           expr->array_lit.count,
                           expected_type->array.count);
            }

            expr->type = type_array_create(sema->arena, expected_elem, expr->array_lit.count);
            return expr->type;
        }

        case EXPR_SLICE: {
            Type* target_type = sema_analyze_expr(sema, expr->slice.target, NULL);
            Type* elem_type   = NULL;

            if (target_type->kind == TYPE_ARRAY) {
                elem_type = target_type->array.elem_type;
            } else if (target_type->kind == TYPE_SLICE) {
                elem_type = target_type->slice.elem_type;
            } else if (type_is_pointer(target_type)) {
                elem_type = target_type->ptr.base;

                if (!expr->slice.end) {
                    sema_error(sema, expr->loc, "cannot slice raw pointer without explicit end index");
                }
            } else {
                sema_error(sema, expr->loc, "cannot slice non-indexable type '%s'",
                           type_to_str(target_type, sema->arena));
                expr->type = type_slice_create(sema->arena, type_primitive(TYPE_VOID));
                return expr->type;
            }

            if (expr->slice.start) {
                Type* start_type = sema_analyze_expr(sema, expr->slice.start, type_primitive(TYPE_U64));

                if (!type_is_integer(start_type)) {
                    sema_error(sema, expr->slice.start->loc, "slice start index must be an integer, got '%s'",
                               type_to_str(start_type, sema->arena));
                }
            }

            if (expr->slice.end) {
                Type* end_type = sema_analyze_expr(sema, expr->slice.end, type_primitive(TYPE_U64));

                if (!type_is_integer(end_type)) {
                    sema_error(sema, expr->slice.end->loc, "slice end index must be an integer, got '%s'",
                               type_to_str(end_type, sema->arena));
                }
            }

            expr->type = type_slice_create(sema->arena, elem_type);
            return expr->type;
        }

        case EXPR_TUPLE: {
            Type** elem_types = ARENA_NEW_ARRAY(sema->arena, Type*, expr->tuple.count);

            for (size_t i = 0; i < expr->tuple.count; ++i) {
                Type* exp_elem = NULL;

                if (expected_type && expected_type->kind == TYPE_TUPLE && i < expected_type->tuple.count) {
                    exp_elem = expected_type->tuple.elements[i];
                }

                elem_types[i] = sema_analyze_expr(sema, expr->tuple.elements[i], exp_elem);
            }

            expr->type = type_tuple_create(sema->arena, elem_types, expr->tuple.count);
            return expr->type;
        }

        case EXPR_CALL: {
            if (expr->call.is_method_call) {
                assert(expr->call.arg_count > 0);

                Type* receiver_type = sema_analyze_expr(sema, expr->call.args[0], NULL);
                Type* struct_type   = receiver_type;

                if (type_is_pointer(struct_type)) {
                    struct_type = struct_type->ptr.base;
                }

                struct_type = sema_resolve_type(sema, struct_type);

                if (!struct_type || (struct_type->kind != TYPE_STRUCT && struct_type->kind != TYPE_UNION)) {
                    sema_error(sema, expr->loc, "method call on non-aggregate receiver of type '%s'",
                               type_to_str(receiver_type, sema->arena));
                    expr->type = type_primitive(TYPE_I64);
                    return expr->type;
                }

                char* mangled = arena_sprintf(sema->arena, "%.*s_%.*s",
                                              (int)struct_type->structure.name.len, struct_type->structure.name.data,
                                              (int)expr->call.callee_name.len, expr->call.callee_name.data);

                StrView mangled_view = (StrView){ .data = mangled, .len = strlen(mangled) };
                Symbol* method_sym   = scope_lookup(sema, mangled_view);

                if (method_sym && method_sym->kind == SYM_PROC) {
                    expr->call.callee_name = mangled_view;
                    expr->call.callee_sym  = method_sym;

                    Type* proc_type = method_sym->type;

                    if (expr->call.arg_count != proc_type->func.param_count) {
                        sema_error(sema, expr->loc, "procedure '%.*s' expects %zu arguments, but %zu were provided",
                                   (int)mangled_view.len, mangled_view.data,
                                   proc_type->func.param_count, expr->call.arg_count);
                    }

                    for (size_t i = 0; i < expr->call.arg_count; ++i) {
                        Type* expected_param_type = (i < proc_type->func.param_count) ? proc_type->func.param_types[i] : NULL;
                        Type* arg_type = (i == 0) ? receiver_type : sema_analyze_expr(sema, expr->call.args[i], expected_param_type);

                        if (expected_param_type && expected_param_type->kind == TYPE_SLICE && arg_type->kind == TYPE_ARRAY) {
                            expr->call.args[i] = ast_expr_slice(sema->arena, expr->call.args[i], NULL, NULL, expr->call.args[i]->loc);
                            arg_type = sema_analyze_expr(sema, expr->call.args[i], expected_param_type);
                        }

                        if (expected_param_type && !types_are_compatible(expected_param_type, arg_type)) {
                            sema_error(sema, expr->call.args[i]->loc,
                                       "argument %zu expects type '%s', but got '%s'",
                                       i + 1,
                                       type_to_str(expected_param_type, sema->arena),
                                       type_to_str(arg_type, sema->arena));
                        }
                    }

                    expr->type = proc_type->func.return_type;
                    return expr->type;
                }

                if (struct_type->structure.generic_arg_count > 0) {
                    StrView base_name = struct_type->structure.generic_template
                                            ? struct_type->structure.generic_template->structure.name
                                            : struct_type->structure.name;

                    char* generic_mangled = arena_sprintf(sema->arena, "%.*s_%.*s",
                                                          (int)base_name.len, base_name.data,
                                                          (int)expr->call.callee_name.len, expr->call.callee_name.data);

                    expr->call.callee_name     = (StrView){ .data = generic_mangled, .len = strlen(generic_mangled) };
                    expr->call.type_args       = struct_type->structure.generic_args;
                    expr->call.type_arg_count  = struct_type->structure.generic_arg_count;
                    expr->call.is_method_call  = false;
                } else {
                    StructField* field = type_struct_lookup_field(struct_type, expr->call.callee_name);

                    if (field) {
                        AstExpr* target = expr->call.args[0];
                        AstExpr* member_expr = ast_expr_member(sema->arena, target, field->name, expr->loc);
                        member_expr->member.field = field;
                        member_expr->type         = field->type;

                        size_t new_count = expr->call.arg_count - 1;
                        AstExpr** new_args = ARENA_NEW_ARRAY(sema->arena, AstExpr*, new_count);

                        for (size_t i = 0; i < new_count; ++i) {
                            new_args[i] = expr->call.args[i + 1];
                        }

                        expr->call.callee_expr    = member_expr;
                        expr->call.callee_name    = (StrView){0};
                        expr->call.callee_sym     = NULL;
                        expr->call.args           = new_args;
                        expr->call.arg_count      = new_count;
                        expr->call.is_method_call = false;
                    } else {
                        sema_error(sema, expr->loc, "aggregate '%.*s' has no method or field named '%.*s'",
                                   (int)struct_type->structure.name.len, struct_type->structure.name.data,
                                   (int)expr->call.callee_name.len, expr->call.callee_name.data);
                        expr->type = type_primitive(TYPE_I64);
                        return expr->type;
                    }
                }
            }

            Type* func_type = NULL;

            if (expr->call.callee_expr != NULL) {
                Type* callee_t = sema_analyze_expr(sema, expr->call.callee_expr, NULL);

                if (callee_t && callee_t->kind == TYPE_PTR && callee_t->ptr.base->kind == TYPE_FUNC) {
                    callee_t = callee_t->ptr.base;
                }

                if (!callee_t || callee_t->kind != TYPE_FUNC) {
                    sema_error(sema, expr->loc, "called object is not a procedure or procedure pointer");
                    expr->type = type_primitive(TYPE_I64);
                    return expr->type;
                }

                func_type = callee_t;
            } else {
                Symbol* sym = scope_lookup(sema, expr->call.callee_name);

                if (!sym) {
                    GenericProcTemplate* templ = NULL;

                    for (GenericProcTemplate* gt = sema->generic_proc_templates; gt != NULL; gt = gt->next) {
                        if (strview_equals(gt->name, expr->call.callee_name)) {
                            templ = gt;
                            break;
                        }
                    }

                    if (templ) {
                        AstProc* template_proc = templ->def;
                        Type** concrete_args   = NULL;

                        if (expr->call.type_arg_count > 0) {
                            if (template_proc->generic_param_count != expr->call.type_arg_count) {
                                sema_error(sema, expr->loc, "generic procedure '%.*s' expects %zu type arguments, got %zu",
                                           (int)template_proc->name.len, template_proc->name.data,
                                           template_proc->generic_param_count, expr->call.type_arg_count);
                                expr->type = type_primitive(TYPE_I64);
                                return expr->type;
                            }

                            concrete_args = ARENA_NEW_ARRAY(sema->arena, Type*, expr->call.type_arg_count);
                            for (size_t i = 0; i < expr->call.type_arg_count; ++i) {
                                concrete_args[i] = sema_resolve_type(sema, expr->call.type_args[i]);
                            }
                        } else {
                            Type** inferred = ARENA_NEW_ARRAY_ZERO(sema->arena, Type*, template_proc->generic_param_count);

                            if (expected_type && template_proc->return_type) {
                                type_unify(template_proc->return_type, expected_type, inferred, template_proc->generic_param_count);
                            }

                            for (size_t i = 0; i < expr->call.arg_count && i < template_proc->param_count; ++i) {
                                Type* param_pattern = template_proc->params[i].type;
                                Type* exp_arg_type  = NULL;

                                if (param_pattern->kind == TYPE_PARAM && param_pattern->param.index < template_proc->generic_param_count) {
                                    exp_arg_type = inferred[param_pattern->param.index];
                                }

                                Type* arg_type = sema_analyze_expr(sema, expr->call.args[i], exp_arg_type);

                                if (param_pattern->kind == TYPE_SLICE && arg_type->kind == TYPE_ARRAY) {
                                    expr->call.args[i] = ast_expr_slice(sema->arena, expr->call.args[i], NULL, NULL, expr->call.args[i]->loc);
                                    arg_type = type_slice_create(sema->arena, arg_type->array.elem_type);
                                    expr->call.args[i]->type = arg_type;
                                }

                                type_unify(param_pattern, arg_type, inferred, template_proc->generic_param_count);
                            }

                            for (size_t i = 0; i < template_proc->generic_param_count; ++i) {
                                if (!inferred[i]) {
                                    sema_error(sema, expr->loc, "could not infer type arguments for generic procedure '%.*s'",
                                               (int)template_proc->name.len, template_proc->name.data);
                                    expr->type = type_primitive(TYPE_I64);
                                    return expr->type;
                                }
                            }

                            concrete_args = inferred;
                        }

                        sym = sema_get_or_instantiate_generic_proc(sema, template_proc, concrete_args, template_proc->generic_param_count, expr->loc);
                        if (sym) {
                            expr->call.callee_name = sym->name;
                            expr->call.callee_sym  = sym;
                            func_type              = sym->type;
                        }
                    }

                    if (!sym) {
                        sema_error(sema, expr->loc, "call to undeclared identifier '%.*s'",
                                   (int)expr->call.callee_name.len, expr->call.callee_name.data);
                        expr->type = type_primitive(TYPE_I64);
                        return expr->type;
                    }
                }

                if (sym->kind == SYM_PROC) {
                    expr->call.callee_sym = sym;
                    func_type = sym->type;
                } else if (sym->type && (sym->type->kind == TYPE_FUNC ||
                          (sym->type->kind == TYPE_PTR && sym->type->ptr.base->kind == TYPE_FUNC))) {

                    AstExpr* var_expr = ast_expr_var(sema->arena, sym->name, expr->loc);
                    var_expr->var.symbol = sym;
                    var_expr->type       = sym->type;

                    expr->call.callee_expr = var_expr;
                    expr->call.callee_name = (StrView){0};

                    func_type = (sym->type->kind == TYPE_FUNC) ? sym->type : sym->type->ptr.base;
                } else {
                    sema_error(sema, expr->loc, "'%.*s' is not callable",
                               (int)expr->call.callee_name.len, expr->call.callee_name.data);
                    expr->type = type_primitive(TYPE_I64);
                    return expr->type;
                }
            }

            if (func_type->func.is_variadic) {
                if (expr->call.arg_count < func_type->func.param_count) {
                    sema_error(sema, expr->loc, "variadic call expects at least %zu arguments, but %zu were provided",
                               func_type->func.param_count, expr->call.arg_count);
                }
            } else {
                if (expr->call.arg_count != func_type->func.param_count) {
                    sema_error(sema, expr->loc, "call expects %zu arguments, but %zu were provided",
                               func_type->func.param_count, expr->call.arg_count);
                }
            }

            for (size_t i = 0; i < expr->call.arg_count; ++i) {
                Type* expected_param_type = (i < func_type->func.param_count) ? func_type->func.param_types[i] : NULL;
                Type* arg_type = sema_analyze_expr(sema, expr->call.args[i], expected_param_type);

                if (expected_param_type && expected_param_type->kind == TYPE_SLICE && arg_type->kind == TYPE_ARRAY) {
                    expr->call.args[i] = ast_expr_slice(sema->arena, expr->call.args[i], NULL, NULL, expr->call.args[i]->loc);
                    arg_type = sema_analyze_expr(sema, expr->call.args[i], expected_param_type);
                }

                if (expected_param_type && !types_are_compatible(expected_param_type, arg_type)) {
                    sema_error(sema, expr->call.args[i]->loc,
                               "argument %zu expects type '%s', but got '%s'",
                               i + 1,
                               type_to_str(expected_param_type, sema->arena),
                               type_to_str(arg_type, sema->arena));
                }
            }

            expr->type = func_type->func.return_type;
            return expr->type;
        }

        case EXPR_VA_START: {
            if (!sema->current_proc || !sema->current_proc->is_variadic) {
                sema_error(sema, expr->loc, "va_start can only be used inside variadic procedures");
            }

            sema_analyze_expr(sema, expr->va_op.valist_expr, NULL);
            expr->type = type_primitive(TYPE_VOID);
            return expr->type;
        }

        case EXPR_VA_ARG: {
            expr->va_op.target_type = sema_resolve_type(sema, expr->va_op.target_type);
            sema_analyze_expr(sema, expr->va_op.valist_expr, NULL);
            expr->type = expr->va_op.target_type;
            return expr->type;
        }

        case EXPR_VA_END: {
            sema_analyze_expr(sema, expr->va_op.valist_expr, NULL);
            expr->type = type_primitive(TYPE_VOID);
            return expr->type;
        }

        case EXPR_VA_COPY: {
            sema_analyze_expr(sema, expr->va_op.valist_expr, NULL);
            sema_analyze_expr(sema, expr->va_op.src_valist_expr, NULL);
            expr->type = type_primitive(TYPE_VOID);
            return expr->type;
        }
    }

    return type_primitive(TYPE_VOID);
}

static void sema_analyze_stmt(Sema* sema, AstStmt* stmt);

static void sema_analyze_block(Sema* sema, AstStmt* block_stmt) {
    scope_push(sema);

    for (size_t i = 0; i < block_stmt->block.count; ++i) {
        sema_analyze_stmt(sema, block_stmt->block.stmts[i]);
    }

    scope_pop(sema);
}

static void sema_analyze_stmt(Sema* sema, AstStmt* stmt) {
    if (!stmt) {
        return;
    }

    switch (stmt->kind) {
        case STMT_BLOCK: {
            sema_analyze_block(sema, stmt);
            break;
        }

        case STMT_VAR_DECL: {
            Type* declared = sema_resolve_type(sema, stmt->var_decl.declared_type);
            stmt->var_decl.declared_type = declared;

            Type* init_type = NULL;

            if (stmt->var_decl.init_expr) {
                init_type = sema_analyze_expr(sema, stmt->var_decl.init_expr, declared);

                if (declared && declared->kind == TYPE_SLICE && init_type->kind == TYPE_ARRAY) {
                    stmt->var_decl.init_expr = ast_expr_slice(sema->arena, stmt->var_decl.init_expr, NULL, NULL, stmt->loc);
                    init_type = sema_analyze_expr(sema, stmt->var_decl.init_expr, declared);
                }

                if (declared && !types_are_compatible(declared, init_type)) {
                    sema_error(sema, stmt->loc, "variable '%.*s' declared with type '%s', but initialized with '%s'",
                               (int)stmt->var_decl.name.len, stmt->var_decl.name.data,
                               type_to_str(declared, sema->arena),
                               type_to_str(init_type, sema->arena));
                }
            }

            Type* final_type = declared ? declared : init_type;

            if (!final_type || final_type->kind == TYPE_VOID) {
                sema_error(sema, stmt->loc, "variable '%.*s' cannot have type void",
                           (int)stmt->var_decl.name.len, stmt->var_decl.name.data);
                final_type = type_primitive(TYPE_I64);
            }

            Symbol* sym = scope_define_symbol(sema, SYM_VAR, stmt->var_decl.name, final_type, stmt->loc);
            stmt->var_decl.symbol = sym;
            break;
        }

        case STMT_ASSIGN: {
            Type* target_type = sema_analyze_expr(sema, stmt->assign.target, NULL);
            Type* value_type  = sema_analyze_expr(sema, stmt->assign.value, target_type);

            if (target_type->kind == TYPE_SLICE && value_type->kind == TYPE_ARRAY) {
                stmt->assign.value = ast_expr_slice(sema->arena, stmt->assign.value, NULL, NULL, stmt->loc);
                value_type = sema_analyze_expr(sema, stmt->assign.value, target_type);
            }

            if (!expr_is_lvalue(stmt->assign.target)) {
                sema_error(sema, stmt->loc, "assignment target is not a valid lvalue");
            }

            if (!types_are_compatible(target_type, value_type)) {
                sema_error(sema, stmt->loc, "cannot assign type '%s' to target of type '%s'",
                           type_to_str(value_type, sema->arena),
                           type_to_str(target_type, sema->arena));
            }
            break;
        }

        case STMT_COMPOUND_ASSIGN: {
            Type* target_type = sema_analyze_expr(sema, stmt->compound_assign.target, NULL);
            Type* value_type  = sema_analyze_expr(sema, stmt->compound_assign.value, target_type);

            if (!expr_is_lvalue(stmt->compound_assign.target)) {
                sema_error(sema, stmt->loc, "compound assignment target is not a valid lvalue");
            }

            if (type_is_pointer(target_type) && type_is_integer(value_type)) {
                break;
            }

            if (!types_are_compatible(target_type, value_type)) {
                sema_error(sema, stmt->loc, "incompatible types in compound assignment: '%s' and '%s'",
                           type_to_str(target_type, sema->arena),
                           type_to_str(value_type, sema->arena));
            }
            break;
        }

        case STMT_DESTRUCTURE_DECL: {
            Type* init_type = sema_analyze_expr(sema, stmt->destructure_decl.init_expr, NULL);

            if (!init_type || init_type->kind != TYPE_TUPLE) {
                sema_error(sema, stmt->loc, "cannot destructure non-tuple type '%s'",
                           type_to_str(init_type, sema->arena));
                return;
            }

            if (stmt->destructure_decl.count != init_type->tuple.count) {
                sema_error(sema, stmt->loc, "destructuring expects %zu variables, but tuple has %zu elements",
                           stmt->destructure_decl.count, init_type->tuple.count);
                return;
            }

            for (size_t i = 0; i < stmt->destructure_decl.count; ++i) {
                StrView name = stmt->destructure_decl.names[i];

                if (name.len == 1 && name.data[0] == '_') {
                    stmt->destructure_decl.symbols[i] = NULL;
                    continue;
                }

                Type* declared = sema_resolve_type(sema, stmt->destructure_decl.declared_types[i]);
                Type* elem_t   = init_type->tuple.elements[i];

                if (declared && !types_are_compatible(declared, elem_t)) {
                    sema_error(sema, stmt->loc, "variable '%.*s' declared with type '%s', but tuple element has type '%s'",
                               (int)name.len, name.data,
                               type_to_str(declared, sema->arena),
                               type_to_str(elem_t, sema->arena));
                }

                Type* final_t = declared ? declared : elem_t;

                if (!final_t || final_t->kind == TYPE_VOID) {
                    sema_error(sema, stmt->loc, "variable '%.*s' cannot have type void",
                               (int)name.len, name.data);
                    final_t = type_primitive(TYPE_I64);
                }

                Symbol* sym = scope_define_symbol(sema, SYM_VAR, name, final_t, stmt->loc);
                stmt->destructure_decl.symbols[i] = sym;
            }

            break;
        }

        case STMT_DESTRUCTURE_ASSIGN: {
            Type* value_type = sema_analyze_expr(sema, stmt->destructure_assign.value, NULL);

            if (!value_type || value_type->kind != TYPE_TUPLE) {
                sema_error(sema, stmt->loc, "cannot destructure non-tuple type '%s' in assignment",
                           type_to_str(value_type, sema->arena));
                return;
            }

            if (stmt->destructure_assign.count != value_type->tuple.count) {
                sema_error(sema, stmt->loc, "destructuring assignment expects %zu targets, but tuple has %zu elements",
                           stmt->destructure_assign.count, value_type->tuple.count);
                return;
            }

            for (size_t i = 0; i < stmt->destructure_assign.count; ++i) {
                AstExpr* target = stmt->destructure_assign.targets[i];

                if (target->kind == EXPR_VAR && target->var.name.len == 1 && target->var.name.data[0] == '_') {
                    continue;
                }

                Type* target_t = sema_analyze_expr(sema, target, NULL);
                Type* elem_t   = value_type->tuple.elements[i];

                if (!expr_is_lvalue(target)) {
                    sema_error(sema, target->loc, "destructuring assignment target is not a valid lvalue");
                }

                if (!types_are_compatible(target_t, elem_t)) {
                    sema_error(sema, target->loc, "cannot assign tuple element of type '%s' to target of type '%s'",
                               type_to_str(elem_t, sema->arena),
                               type_to_str(target_t, sema->arena));
                }
            }

            break;
        }

        case STMT_RETURN: {
            Type* expected = sema->current_proc ? sema->current_proc->return_type : type_primitive(TYPE_VOID);

            if (stmt->return_stmt.expr) {
                Type* actual = sema_analyze_expr(sema, stmt->return_stmt.expr, expected);

                if (!types_are_compatible(expected, actual)) {
                    if (sema->current_proc) {
                        sema_error(sema, stmt->loc, "procedure '%.*s' expects return type '%s', but got '%s'",
                                   (int)sema->current_proc->name.len, sema->current_proc->name.data,
                                   type_to_str(expected, sema->arena),
                                   type_to_str(actual, sema->arena));
                    } else {
                        sema_error(sema, stmt->loc, "return statement outside of procedure");
                    }
                }
            } else {
                if (expected->kind != TYPE_VOID) {
                    if (sema->current_proc) {
                        sema_error(sema, stmt->loc, "procedure '%.*s' must return a value of type '%s'",
                                   (int)sema->current_proc->name.len, sema->current_proc->name.data,
                                   type_to_str(expected, sema->arena));
                    } else {
                        sema_error(sema, stmt->loc, "return statement outside of procedure");
                    }
                }
            }
            break;
        }

        case STMT_IF: {
            sema_analyze_expr(sema, stmt->if_stmt.cond, type_primitive(TYPE_BOOL));
            sema_analyze_stmt(sema, stmt->if_stmt.then_branch);

            if (stmt->if_stmt.else_branch) {
                sema_analyze_stmt(sema, stmt->if_stmt.else_branch);
            }
            break;
        }

        case STMT_BREAK: {
            if (sema->loop_depth == 0) {
                sema_error(sema, stmt->loc, "'break' statement outside of loop");
            }
            break;
        }

        case STMT_CONTINUE: {
            if (sema->loop_depth == 0) {
                sema_error(sema, stmt->loc, "'continue' statement outside of loop");
            }
            break;
        }

        case STMT_DEFER: {
            sema_analyze_stmt(sema, stmt->defer_stmt.stmt);
            break;
        }

        case STMT_WHILE: {
            sema_analyze_expr(sema, stmt->while_stmt.cond, type_primitive(TYPE_BOOL));

            sema->loop_depth++;
            sema_analyze_stmt(sema, stmt->while_stmt.body);
            sema->loop_depth--;
            break;
        }

        case STMT_FOR: {
            scope_push(sema);

            if (stmt->for_stmt.init) {
                sema_analyze_stmt(sema, stmt->for_stmt.init);
            }

            if (stmt->for_stmt.cond) {
                sema_analyze_expr(sema, stmt->for_stmt.cond, type_primitive(TYPE_BOOL));
            }

            if (stmt->for_stmt.step) {
                sema_analyze_stmt(sema, stmt->for_stmt.step);
            }

            sema->loop_depth++;
            sema_analyze_stmt(sema, stmt->for_stmt.body);
            sema->loop_depth--;

            scope_pop(sema);
            break;
        }

        case STMT_SWITCH: {
            Type* cond_type = sema_analyze_expr(sema, stmt->switch_stmt.cond, NULL);

            if (!type_is_integer(cond_type) && cond_type->kind != TYPE_ENUM && cond_type->kind != TYPE_CHAR && cond_type->kind != TYPE_BOOL) {
                sema_error(sema, stmt->loc, "switch condition must be an integer, char, or enum, got '%s'",
                           type_to_str(cond_type, sema->arena));
            }

            bool has_default = false;

            for (size_t i = 0; i < stmt->switch_stmt.case_count; ++i) {
                AstSwitchCase* c = &stmt->switch_stmt.cases[i];

                if (c->is_default) {
                    if (has_default) {
                        sema_error(sema, c->loc, "multiple 'default' labels in switch");
                    }
                    has_default = true;
                } else {
                    for (size_t p = 0; p < c->pattern_count; ++p) {
                        AstCasePattern* pat = &c->patterns[p];

                        sema_analyze_expr(sema, pat->val_start, cond_type);

                        int64_t start_val = 0;
                        if (!eval_expr_const_int(sema, pat->val_start, &start_val)) {
                            sema_error(sema, pat->val_start->loc, "case value must be a constant expression");
                            continue;
                        }

                        pat->const_start = start_val;
                        pat->const_end   = start_val;

                        if (pat->is_range) {
                            sema_analyze_expr(sema, pat->val_end, cond_type);

                            int64_t end_val = 0;
                            if (!eval_expr_const_int(sema, pat->val_end, &end_val)) {
                                sema_error(sema, pat->val_end->loc, "case range end must be a constant expression");
                                continue;
                            }

                            if (start_val > end_val) {
                                sema_error(sema, pat->loc, "inverted case range: start (%lld) is greater than end (%lld)",
                                           (long long)start_val, (long long)end_val);
                            }

                            pat->const_end = end_val;
                        }

                        for (size_t prev_i = 0; prev_i <= i; ++prev_i) {
                            AstSwitchCase* prev_c = &stmt->switch_stmt.cases[prev_i];
                            if (prev_c->is_default) continue;

                            size_t max_p = (prev_i == i) ? p : prev_c->pattern_count;

                            for (size_t prev_p = 0; prev_p < max_p; ++prev_p) {
                                AstCasePattern* prev_pat = &prev_c->patterns[prev_p];

                                int64_t low = (pat->const_start > prev_pat->const_start) ? pat->const_start : prev_pat->const_start;
                                int64_t high = (pat->const_end < prev_pat->const_end) ? pat->const_end : prev_pat->const_end;

                                if (low <= high) {
                                    sema_error(sema, pat->loc, "duplicate or overlapping case value/range [%lld..%lld]",
                                               (long long)pat->const_start, (long long)pat->const_end);
                                }
                            }
                        }
                    }
                }

                scope_push(sema);

                for (size_t s = 0; s < c->stmt_count; ++s) {
                    sema_analyze_stmt(sema, c->stmts[s]);
                }

                scope_pop(sema);
            }
            break;
        }

        case STMT_EXPR: {
            sema_analyze_expr(sema, stmt->expr_stmt.expr, NULL);
            break;
        }
    }
}

static void sema_analyze_proc_body(Sema* sema, AstProc* proc) {
    if (!proc) {
        return;
    }

    AstProc* saved_proc       = sema->current_proc;
    uint32_t saved_loop_depth = sema->loop_depth;
    Scope*   saved_scope      = sema->current_scope;

    sema->current_proc  = proc;
    sema->loop_depth    = 0;
    sema->current_scope = sema->global_scope;

    scope_push(sema);

    for (size_t i = 0; i < proc->param_count; ++i) {
        AstParam* p = &proc->params[i];
        p->type = sema_resolve_type(sema, p->type);

        Symbol* sym = scope_define_symbol(sema, SYM_PARAM, p->name, p->type, p->loc);
        p->symbol   = sym;
    }

    if (proc->body) {
        if (proc->body->kind == STMT_BLOCK) {
            for (size_t i = 0; i < proc->body->block.count; ++i) {
                sema_analyze_stmt(sema, proc->body->block.stmts[i]);
            }
        } else {
            sema_analyze_stmt(sema, proc->body);
        }
    }

    scope_pop(sema);

    sema->current_proc  = saved_proc;
    sema->loop_depth    = saved_loop_depth;
    sema->current_scope = saved_scope;
}

void sema_init(Sema* sema, Arena* arena) {
    memset(sema, 0, sizeof(Sema));
    
    sema->arena           = arena;
    sema->global_scope    = ARENA_NEW_ZERO(arena, Scope);
    sema->current_scope   = sema->global_scope;
    sema->struct_registry = NULL;
    sema->union_registry  = NULL;
    sema->enum_registry   = NULL;
    sema->alias_registry  = NULL;
    sema->current_proc    = NULL;
    sema->loop_depth      = 0;
    sema->had_error       = false;

    Symbol* sym_true = scope_define_symbol(sema, SYM_CONST, (StrView){ .data = "true", .len = 4 }, type_primitive(TYPE_BOOL), (SourceLoc){ .filename = NULL, .line_start = NULL, .line = 0, .col = 0, .len = 0 });
    sym_true->const_val = 1;

    Symbol* sym_false = scope_define_symbol(sema, SYM_CONST, (StrView){ .data = "false", .len = 5 }, type_primitive(TYPE_BOOL), (SourceLoc){ .filename = NULL, .line_start = NULL, .line = 0, .col = 0, .len = 0 });
    sym_false->const_val = 0;

    TypeAliasEntry* valist_entry = ARENA_NEW_ZERO(arena, TypeAliasEntry);
    valist_entry->name        = (StrView){ .data = "VaList", .len = 6 };
    valist_entry->type        = type_primitive(TYPE_VALIST);
    valist_entry->is_distinct = false;
    valist_entry->next        = sema->alias_registry;
    sema->alias_registry      = valist_entry;

    scope_define_symbol(sema, SYM_TYPE_ALIAS, (StrView){ .data = "VaList", .len = 6 }, type_primitive(TYPE_VALIST), (SourceLoc){0});
}

static Type* sema_resolve_struct_layout(Sema* sema, Type* struct_type, AstStructDef** structs, size_t struct_count) {
    if (!struct_type || struct_type->kind != TYPE_STRUCT) {
        return struct_type;
    }

    StructTypeEntry* entry = NULL;

    for (StructTypeEntry* e = sema->struct_registry; e != NULL; e = e->next) {
        if (strview_equals(e->name, struct_type->structure.name)) {
            entry = e;
            break;
        }
    }

    if (!entry) {
        return struct_type;
    }

    if (entry->is_resolved) {
        return entry->type;
    }

    if (entry->is_resolving) {
        sema_error(sema, (SourceLoc){ .filename = "<sema>", .line_start = NULL, .line = 0, .col = 0, .len = 0 },
                   "cyclic struct dependency: struct '%.*s' cannot contain itself by value",
                   (int)entry->name.len, entry->name.data);
        return entry->type;
    }

    AstStructDef* s_def = NULL;

    for (size_t i = 0; i < struct_count; ++i) {
        if (strview_equals(structs[i]->name, entry->name)) {
            s_def = structs[i];
            break;
        }
    }

    if (!s_def) {
        return entry->type;
    }

    entry->is_resolving = true;

    for (size_t f = 0; f < s_def->field_count; ++f) {
        s_def->fields[f].type = sema_resolve_type(sema, s_def->fields[f].type);

        Type* inner = s_def->fields[f].type;

        while (inner && inner->kind == TYPE_ARRAY) {
            inner = inner->array.elem_type;
        }

        if (inner && inner->kind == TYPE_STRUCT) {
            sema_resolve_struct_layout(sema, inner, structs, struct_count);
        }

        if (s_def->fields[f].default_value) {
            sema_analyze_expr(sema, s_def->fields[f].default_value, s_def->fields[f].type);

            if (!types_are_compatible(s_def->fields[f].type, s_def->fields[f].default_value->type)) {
                sema_error(sema, s_def->fields[f].default_value->loc,
                           "default value type '%s' is incompatible with field type '%s'",
                           type_to_str(s_def->fields[f].default_value->type, sema->arena),
                           type_to_str(s_def->fields[f].type, sema->arena));
            }
        }
    }

    type_struct_init(s_def->type, s_def->name, s_def->fields, s_def->field_count, s_def->is_packed);

    entry->is_resolving = false;
    entry->is_resolved  = true;

    return entry->type;
}

static Type* sema_resolve_union_layout(Sema* sema, Type* union_type, AstUnionDef** unions, size_t union_count) {
    if (!union_type || union_type->kind != TYPE_UNION) {
        return union_type;
    }

    UnionTypeEntry* entry = NULL;

    for (UnionTypeEntry* e = sema->union_registry; e != NULL; e = e->next) {
        if (strview_equals(e->name, union_type->structure.name)) {
            entry = e;
            break;
        }
    }

    if (!entry) {
        return union_type;
    }

    if (entry->is_resolved) {
        return entry->type;
    }

    if (entry->is_resolving) {
        sema_error(sema, (SourceLoc){ .filename = "<sema>", .line_start = NULL, .line = 0, .col = 0, .len = 0 },
                   "cyclic union dependency: union '%.*s' cannot contain itself by value",
                   (int)entry->name.len, entry->name.data);
        return entry->type;
    }

    AstUnionDef* u_def = NULL;

    for (size_t i = 0; i < union_count; ++i) {
        if (strview_equals(unions[i]->name, entry->name)) {
            u_def = unions[i];
            break;
        }
    }

    if (!u_def) {
        return entry->type;
    }

    entry->is_resolving = true;

    for (size_t f = 0; f < u_def->field_count; ++f) {
        u_def->fields[f].type = sema_resolve_type(sema, u_def->fields[f].type);

        Type* inner = u_def->fields[f].type;

        while (inner && inner->kind == TYPE_ARRAY) {
            inner = inner->array.elem_type;
        }

        if (inner && inner->kind == TYPE_UNION) {
            sema_resolve_union_layout(sema, inner, unions, union_count);
        }

        if (u_def->fields[f].default_value) {
            sema_analyze_expr(sema, u_def->fields[f].default_value, u_def->fields[f].type);
        }
    }

    type_union_init(u_def->type, u_def->name, u_def->fields, u_def->field_count);

    entry->is_resolving = false;
    entry->is_resolved  = true;

    return entry->type;
}

bool sema_analyze_program(Sema* sema, AstProgram* program) {
    if (!program) {
        return false;
    }

    sema->current_program = program;

    for (size_t i = 0; i < program->const_count; ++i) {
        AstConstDef* c = program->consts[i];
        c->type = sema_resolve_type(sema, c->type);

        sema_analyze_expr(sema, c->init_expr, c->type);

        int64_t evaluated_val = 0;
        if (!eval_expr_const_int(sema, c->init_expr, &evaluated_val)) {
            sema_error(sema, c->loc, "initializer for 'const %.*s' is not a constant expression",
                       (int)c->name.len, c->name.data);
        }

        c->val = evaluated_val;

        Symbol* sym = scope_define_symbol(sema, SYM_CONST, c->name, c->type, c->loc);
        sym->const_val = c->val;
        c->symbol = sym;
    }
    
    for (size_t i = 0; i < program->typedef_count; ++i) {
        AstTypeDef* td = program->typedefs[i];

        TypeAliasEntry* entry = ARENA_NEW_ZERO(sema->arena, TypeAliasEntry);
        entry->name         = td->name;
        entry->type         = td->target_type;
        entry->is_distinct  = td->is_distinct;
        entry->is_resolving = false;
        entry->next         = sema->alias_registry;

        sema->alias_registry = entry;
    }

    for (size_t i = 0; i < program->typedef_count; ++i) {
        AstTypeDef* td = program->typedefs[i];

        TypeAliasEntry* target_entry = NULL;

        for (TypeAliasEntry* e = sema->alias_registry; e != NULL; e = e->next) {
            if (strview_equals(e->name, td->name)) {
                target_entry = e;
                break;
            }
        }

        if (target_entry) {
            target_entry->is_resolving = true;
            td->target_type = sema_resolve_type(sema, td->target_type);
            target_entry->is_resolving = false;

            if (td->is_distinct) {
                target_entry->type = type_distinct_create(sema->arena, td->name, td->target_type);
            } else {
                target_entry->type = td->target_type;
            }
        }

        Symbol* sym = scope_define_symbol(sema, SYM_TYPE_ALIAS, td->name, target_entry ? target_entry->type : td->target_type, td->loc);
        td->symbol = sym;
    }

    for (size_t i = 0; i < program->enum_count; ++i) {
        AstEnumDef* e = program->enums[i];

        EnumTypeEntry* entry = ARENA_NEW_ZERO(sema->arena, EnumTypeEntry);
        entry->name = e->name;
        entry->type = e->type;
        entry->next = sema->enum_registry;

        sema->enum_registry = entry;
    }

    for (size_t i = 0; i < program->enum_count; ++i) {
        AstEnumDef* e = program->enums[i];

        Type* base_type = sema_resolve_type(sema, e->underlying_type);
        if (!base_type) {
            base_type = type_primitive(TYPE_U32);
        }

        EnumVariant* evaluated_variants = ARENA_NEW_ARRAY_ZERO(sema->arena, EnumVariant, e->variant_count);
        int64_t current_val = 0;

        for (size_t v = 0; v < e->variant_count; ++v) {
            AstEnumVariantDef* vardef = &e->variants[v];

            if (vardef->explicit_value) {
                sema_analyze_expr(sema, vardef->explicit_value, base_type);

                int64_t explicit_int = 0;
                if (eval_expr_const_int(sema, vardef->explicit_value, &explicit_int)) {
                    current_val = explicit_int;
                } else {
                    sema_error(sema, vardef->loc, "enum variant value must be a constant expression");
                }
            }

            evaluated_variants[v].name  = vardef->name;
            evaluated_variants[v].value = current_val;

            current_val += 1;
        }

        e->type = type_enum_create(sema->arena, e->name, base_type, evaluated_variants, e->variant_count);

        for (EnumTypeEntry* entry = sema->enum_registry; entry != NULL; entry = entry->next) {
            if (strview_equals(entry->name, e->name)) {
                entry->type = e->type;
                break;
            }
        }

        Symbol* sym = scope_define_symbol(sema, SYM_ENUM, e->name, e->type, e->loc);
        e->symbol = sym;
    }

    for (size_t i = 0; i < program->struct_count; ++i) {
        AstStructDef* s = program->structs[i];

        if (s->is_generic) {
            for (size_t f = 0; f < s->field_count; ++f) {
                s->fields[f].type = sema_bind_type_params(sema->arena, s->fields[f].type,
                                                          s->generic_params, s->generic_param_count, 0);
            }

            GenericStructTemplate* templ = ARENA_NEW_ZERO(sema->arena, GenericStructTemplate);
            templ->name = s->name;
            templ->def  = s;
            templ->next = sema->generic_struct_templates;

            sema->generic_struct_templates = templ;
        } else {
            StructTypeEntry* entry = ARENA_NEW_ZERO(sema->arena, StructTypeEntry);
            entry->name         = s->name;
            entry->type         = s->type;
            entry->is_resolving = false;
            entry->is_resolved  = false;
            entry->next         = sema->struct_registry;

            sema->struct_registry = entry;
        }
    }

    for (size_t i = 0; i < program->struct_count; ++i) {
        AstStructDef* s = program->structs[i];
        if (!s->is_generic) {
            sema_resolve_struct_layout(sema, s->type, program->structs, program->struct_count);
        }
    }

    for (size_t i = 0; i < program->union_count; ++i) {
        AstUnionDef* u = program->unions[i];

        UnionTypeEntry* entry = ARENA_NEW_ZERO(sema->arena, UnionTypeEntry);
        entry->name         = u->name;
        entry->type         = u->type;
        entry->is_resolving = false;
        entry->is_resolved  = false;
        entry->next         = sema->union_registry;

        sema->union_registry = entry;
    }

    for (size_t i = 0; i < program->union_count; ++i) {
        AstUnionDef* u = program->unions[i];
        sema_resolve_union_layout(sema, u->type, program->unions, program->union_count);
    }

    for (size_t i = 0; i < program->global_count; ++i) {
        AstGlobalVarDef* g = program->globals[i];
        g->type = sema_resolve_type(sema, g->type);

        if (!g->type && g->init_expr) {
            g->type = sema_analyze_expr(sema, g->init_expr, NULL);
        }

        if (g->attrs.custom_align > 0) {
            if ((g->attrs.custom_align & (g->attrs.custom_align - 1)) != 0) {
                sema_error(sema, g->loc, "alignment must be a power of two");
            }
        }

        if (g->attrs.is_extern && g->init_expr != NULL) {
            sema_error(sema, g->loc, "extern variable cannot have an initializer");
        }

        Symbol* sym = scope_define_symbol(sema, SYM_GLOBAL_VAR, g->name, g->type, g->loc);
        sym->is_extern = g->attrs.is_extern;
        sym->attrs = g->attrs;
        g->symbol = sym;
    }

    for (size_t i = 0; i < program->proc_count; ++i) {
        AstProc* proc = program->procs[i];

        if (proc->is_generic) {
            uint32_t depth = (proc->method_struct.len > 0) ? 1 : 0;

            proc->return_type = sema_bind_type_params(sema->arena, proc->return_type,
                                                      proc->generic_params, proc->generic_param_count, depth);

            for (size_t p = 0; p < proc->param_count; ++p) {
                proc->params[p].type = sema_bind_type_params(sema->arena, proc->params[p].type,
                                                             proc->generic_params, proc->generic_param_count, depth);
            }

            GenericProcTemplate* templ = ARENA_NEW_ZERO(sema->arena, GenericProcTemplate);
            templ->name = proc->name;
            templ->def  = proc;
            templ->next = sema->generic_proc_templates;

            sema->generic_proc_templates = templ;
        } else {
            proc->return_type = sema_resolve_type(sema, proc->return_type);

            if (proc->attrs.custom_align > 0) {
                if ((proc->attrs.custom_align & (proc->attrs.custom_align - 1)) != 0) {
                    sema_error(sema, proc->loc, "function alignment must be a power of two");
                }
            }

            Type** param_types = ARENA_NEW_ARRAY(sema->arena, Type*, proc->param_count);

            for (size_t p = 0; p < proc->param_count; ++p) {
                proc->params[p].type = sema_resolve_type(sema, proc->params[p].type);
                param_types[p] = proc->params[p].type;
            }

            Type* proc_type = type_func_create(sema->arena, proc->return_type, param_types, proc->param_count, proc->is_variadic);
            Symbol* sym = scope_define_symbol(sema, SYM_PROC, proc->name, proc_type, proc->loc);
            sym->is_extern = proc->attrs.is_extern;
            sym->attrs = proc->attrs;
            sym->proc_decl = proc;
            proc->symbol = sym;
        }
    }

    if (sema->had_error) {
        return false;
    }

    size_t initial_proc_count = program->proc_count;

    for (size_t i = 0; i < initial_proc_count; ++i) {
        if (!program->procs[i]->is_generic) {
            sema_analyze_proc_body(sema, program->procs[i]);
        }
    }

    return !sema->had_error;
}