#include "sema.h"

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

static bool strview_equals(StrView a, StrView b) {
    if (a.len != b.len) {
        return false;
    }

    return memcmp(a.data, b.data, a.len) == 0;
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

static bool types_are_compatible(const Type* expected, const Type* actual) {
    if (!expected || !actual) {
        return false;
    }

    if (type_equals(expected, actual)) {
        return true;
    }

    if (type_is_integer(expected) && type_is_integer(actual)) {
        return true;
    }

    if (type_is_pointer(expected) && type_is_pointer(actual)) {
        if (expected->ptr.base->kind == TYPE_VOID || actual->ptr.base->kind == TYPE_VOID) {
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

    if (expected->kind == TYPE_BOOL && (type_is_integer(actual) || type_is_pointer(actual) || actual->kind == TYPE_FUNC)) {
        return true;
    }

    if (actual->kind == TYPE_BOOL && (type_is_integer(expected) || expected->kind == TYPE_FUNC)) {
        return true;
    }

    return false;
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

    if (type->kind == TYPE_STRUCT) {
        for (StructTypeEntry* e = sema->struct_registry; e != NULL; e = e->next) {
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
                    sema_error(sema, (SourceLoc){ .filename = "<sema>", .line_start = NULL, .line = 0, .col = 0, .len = 0 },
                               "cyclic type alias definition '%.*s'", (int)e->name.len, e->name.data);
                    return type_primitive(TYPE_I64);
                }

                if (!e->type) {
                    return type;
                }

                e->is_resolving = true;
                Type* resolved = sema_resolve_type(sema, e->type);
                e->is_resolving = false;

                return resolved;
            }
        }

        sema_error(sema, (SourceLoc){ .filename = "<sema>", .line_start = NULL, .line = 0, .col = 0, .len = 0 },
                   "unknown type '%.*s'", (int)type->structure.name.len, type->structure.name.data);
    }

    if (type->kind == TYPE_FUNC) {
        Type* res_ret = sema_resolve_type(sema, type->func.return_type);
        Type** res_params = ARENA_NEW_ARRAY(sema->arena, Type*, type->func.param_count);

        for (size_t i = 0; i < type->func.param_count; ++i) {
            res_params[i] = sema_resolve_type(sema, type->func.param_types[i]);
        }

        return type_func_create(sema->arena, res_ret, res_params, type->func.param_count);
    }

    return type;
}

static Type* sema_analyze_expr(Sema* sema, AstExpr* expr, Type* expected_type);

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
            } else {
                expr->type = type_primitive(TYPE_I64);
            }

            return expr->type;
        }

        case EXPR_STRING_LIT: {
            Type* char_type = type_primitive(TYPE_CHAR);
            expr->type = type_ptr(sema->arena, char_type);

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

        case EXPR_ASM: {
            if (expr->inline_asm.explicit_type) {
                expr->inline_asm.explicit_type = sema_resolve_type(sema, expr->inline_asm.explicit_type);
                expr->type = expr->inline_asm.explicit_type;
            } else if (expected_type && expected_type->kind != TYPE_VOID) {
                expr->type = expected_type;
            } else {
                expr->type = type_primitive(TYPE_U64);
            }

            return expr->type;
        }

        case EXPR_MEMBER: {
            Type* target_type = sema_analyze_expr(sema, expr->member.target, NULL);

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

            if (!struct_type || struct_type->kind != TYPE_STRUCT) {
                sema_error(sema, expr->loc, "member access on non-struct type '%s'",
                           type_to_str(target_type, sema->arena));
                expr->type = type_primitive(TYPE_I64);
                return expr->type;
            }

            StructField* field = type_struct_lookup_field(struct_type, expr->member.field_name);

            if (!field) {
                sema_error(sema, expr->loc, "struct '%.*s' has no field named '%.*s'",
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
                sema_error(sema, expr->loc, "unknown struct type '%.*s'",
                           (int)expr->struct_lit.struct_name.len, expr->struct_lit.struct_name.data);
                expr->type = type_primitive(TYPE_I64);
                return expr->type;
            }

            expr->struct_lit.struct_type = struct_type;

            for (size_t i = 0; i < expr->struct_lit.field_count; ++i) {
                StructField* f = type_struct_lookup_field(struct_type, expr->struct_lit.field_names[i]);

                if (!f) {
                    sema_error(sema, expr->loc, "struct '%.*s' has no field named '%.*s'",
                               (int)struct_type->structure.name.len, struct_type->structure.name.data,
                               (int)expr->struct_lit.field_names[i].len, expr->struct_lit.field_names[i].data);
                    continue;
                }

                sema_analyze_expr(sema, expr->struct_lit.field_values[i], f->type);
            }

            expr->type = struct_type;
            return expr->type;
        }

        case EXPR_CALL: {
            if (expr->call.is_method_call) {
                assert(expr->call.arg_count > 0);

                Type* receiver_type = sema_analyze_expr(sema, expr->call.args[0], NULL);
                Type* struct_type = receiver_type;

                if (type_is_pointer(struct_type)) {
                    struct_type = struct_type->ptr.base;
                }

                struct_type = sema_resolve_type(sema, struct_type);

                if (!struct_type || struct_type->kind != TYPE_STRUCT) {
                    sema_error(sema, expr->loc, "method call on non-struct receiver of type '%s'",
                               type_to_str(receiver_type, sema->arena));
                    expr->type = type_primitive(TYPE_I64);
                    return expr->type;
                }

                char* mangled = arena_sprintf(sema->arena, "%.*s_%.*s",
                                              (int)struct_type->structure.name.len, struct_type->structure.name.data,
                                              (int)expr->call.callee_name.len, expr->call.callee_name.data);

                StrView mangled_view = (StrView){ .data = mangled, .len = strlen(mangled) };
                Symbol* method_sym = scope_lookup(sema, mangled_view);

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
                    sema_error(sema, expr->loc, "struct '%.*s' has no method or field named '%.*s'",
                               (int)struct_type->structure.name.len, struct_type->structure.name.data,
                               (int)expr->call.callee_name.len, expr->call.callee_name.data);
                    expr->type = type_primitive(TYPE_I64);
                    return expr->type;
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
                    sema_error(sema, expr->loc, "call to undeclared identifier '%.*s'",
                               (int)expr->call.callee_name.len, expr->call.callee_name.data);
                    expr->type = type_primitive(TYPE_I64);
                    return expr->type;
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

            if (expr->call.arg_count != func_type->func.param_count) {
                sema_error(sema, expr->loc, "call expects %zu arguments, but %zu were provided",
                           func_type->func.param_count, expr->call.arg_count);
            }

            for (size_t i = 0; i < expr->call.arg_count; ++i) {
                Type* expected_param_type = (i < func_type->func.param_count) ? func_type->func.param_types[i] : NULL;
                Type* arg_type = sema_analyze_expr(sema, expr->call.args[i], expected_param_type);

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

        case STMT_RETURN: {
            Type* expected = sema->current_proc ? sema->current_proc->return_type : type_primitive(TYPE_VOID);

            if (stmt->return_stmt.expr) {
                Type* actual = sema_analyze_expr(sema, stmt->return_stmt.expr, expected);

                if (!types_are_compatible(expected, actual)) {
                    sema_error(sema, stmt->loc, "procedure '%.*s' expects return type '%s', but got '%s'",
                               (int)sema->current_proc->name.len, sema->current_proc->name.data,
                               type_to_str(expected, sema->arena),
                               type_to_str(actual, sema->arena));
                }
            } else {
                if (expected->kind != TYPE_VOID) {
                    sema_error(sema, stmt->loc, "procedure '%.*s' must return a value of type '%s'",
                               (int)sema->current_proc->name.len, sema->current_proc->name.data,
                               type_to_str(expected, sema->arena));
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
                    c->const_values = ARENA_NEW_ARRAY(sema->arena, int64_t, c->value_count);

                    for (size_t v = 0; v < c->value_count; ++v) {
                        sema_analyze_expr(sema, c->values[v], cond_type);

                        if (c->values[v]->kind != EXPR_INT_LIT) {
                            sema_error(sema, c->values[v]->loc, "case value must be a constant integer or enum variant");
                            c->const_values[v] = 0;
                            continue;
                        }

                        c->const_values[v] = c->values[v]->int_val;

                        for (size_t prev_i = 0; prev_i <= i; ++prev_i) {
                            AstSwitchCase* prev_c = &stmt->switch_stmt.cases[prev_i];
                            if (prev_c->is_default) continue;

                            size_t max_v = (prev_i == i) ? v : prev_c->value_count;

                            for (size_t prev_v = 0; prev_v < max_v; ++prev_v) {
                                if (prev_c->const_values[prev_v] == c->const_values[v]) {
                                    sema_error(sema, c->values[v]->loc, "duplicate case value '%lld' in switch",
                                               (long long)c->const_values[v]);
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
            if (stmt->expr_stmt.expr->kind == EXPR_ASM && !stmt->expr_stmt.expr->inline_asm.explicit_type) {
                stmt->expr_stmt.expr->type = type_primitive(TYPE_VOID);
            } else {
                sema_analyze_expr(sema, stmt->expr_stmt.expr, NULL);
            }
            break;
        }
    }
}

static void sema_analyze_proc_body(Sema* sema, AstProc* proc) {
    sema->current_proc = proc;

    scope_push(sema);

    for (size_t i = 0; i < proc->param_count; ++i) {
        AstParam* p = &proc->params[i];
        p->type = sema_resolve_type(sema, p->type);

        scope_define_symbol(sema, SYM_PARAM, p->name, p->type, p->loc);
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

    sema->current_proc = NULL;
}

void sema_init(Sema* sema, Arena* arena) {
    sema->arena           = arena;
    sema->global_scope    = ARENA_NEW_ZERO(arena, Scope);
    sema->current_scope   = sema->global_scope;
    sema->struct_registry = NULL;
    sema->enum_registry   = NULL;
    sema->alias_registry  = NULL;
    sema->current_proc    = NULL;
    sema->loop_depth      = 0;
    sema->had_error       = false;

    Symbol* sym_true = scope_define_symbol(sema, SYM_CONST, (StrView){ .data = "true", .len = 4 }, type_primitive(TYPE_BOOL), (SourceLoc){ .filename = NULL, .line_start = NULL, .line = 0, .col = 0, .len = 0 });
    sym_true->const_val = 1;

    Symbol* sym_false = scope_define_symbol(sema, SYM_CONST, (StrView){ .data = "false", .len = 5 }, type_primitive(TYPE_BOOL), (SourceLoc){ .filename = NULL, .line_start = NULL, .line = 0, .col = 0, .len = 0 });
    sym_false->const_val = 0;
}

bool sema_analyze_program(Sema* sema, AstProgram* program) {
    if (!program) {
        return false;
    }

    for (size_t i = 0; i < program->const_count; ++i) {
        AstConstDef* c = program->consts[i];
        c->type = sema_resolve_type(sema, c->type);

        Symbol* sym = scope_define_symbol(sema, SYM_CONST, c->name, c->type, c->loc);
        sym->const_val = c->val;
        c->symbol = sym;
    }
    
    for (size_t i = 0; i < program->typedef_count; ++i) {
        AstTypeDef* td = program->typedefs[i];

        TypeAliasEntry* entry = ARENA_NEW_ZERO(sema->arena, TypeAliasEntry);
        entry->name         = td->name;
        entry->type         = td->target_type;
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
            target_entry->type = td->target_type;
        }

        Symbol* sym = scope_define_symbol(sema, SYM_TYPE_ALIAS, td->name, td->target_type, td->loc);
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

                if (vardef->explicit_value->kind == EXPR_INT_LIT) {
                    current_val = vardef->explicit_value->int_val;
                } else {
                    sema_error(sema, vardef->loc, "enum variant value must be an integer literal");
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

        StructTypeEntry* entry = ARENA_NEW_ZERO(sema->arena, StructTypeEntry);
        entry->name = s->name;
        entry->type = s->type;
        entry->next = sema->struct_registry;

        sema->struct_registry = entry;
    }

    for (size_t i = 0; i < program->struct_count; ++i) {
        AstStructDef* s = program->structs[i];

        for (size_t f = 0; f < s->field_count; ++f) {
            s->fields[f].type = sema_resolve_type(sema, s->fields[f].type);
        }

        type_struct_init(s->type, s->name, s->fields, s->field_count, s->is_packed);
    }

    for (size_t i = 0; i < program->global_count; ++i) {
        AstGlobalVarDef* g = program->globals[i];
        g->type = sema_resolve_type(sema, g->type);

        if (!g->type && g->init_expr) {
            g->type = sema_analyze_expr(sema, g->init_expr, NULL);
        }

        Symbol* sym = scope_define_symbol(sema, SYM_GLOBAL_VAR, g->name, g->type, g->loc);
        g->symbol = sym;
    }

    for (size_t i = 0; i < program->proc_count; ++i) {
        AstProc* proc = program->procs[i];
        proc->return_type = sema_resolve_type(sema, proc->return_type);

        Type** param_types = ARENA_NEW_ARRAY(sema->arena, Type*, proc->param_count);

        for (size_t p = 0; p < proc->param_count; ++p) {
            proc->params[p].type = sema_resolve_type(sema, proc->params[p].type);
            param_types[p] = proc->params[p].type;
        }

        Type* proc_type = type_func_create(sema->arena, proc->return_type, param_types, proc->param_count);
        Symbol* sym = scope_define_symbol(sema, SYM_PROC, proc->name, proc_type, proc->loc);
        proc->symbol = sym;
    }

    if (sema->had_error) {
        return false;
    }

    for (size_t i = 0; i < program->proc_count; ++i) {
        sema_analyze_proc_body(sema, program->procs[i]);
    }

    return !sema->had_error;
}