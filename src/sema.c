#include "sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

static void sema_error(Sema* sema, SourceLoc loc, const char* fmt, ...) {
    sema->had_error = true;

    fprintf(stderr, "%s:%u:%u: semantic error: ", loc.filename, loc.line, loc.col);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
}

static void scope_push(Sema* sema) {
    Scope* scope = ARENA_NEW_ZERO(sema->arena, Scope);

    scope->parent = sema->current_scope;
    scope->entries = NULL;

    sema->current_scope = scope;
}

static void scope_pop(Sema* sema) {
    assert(sema->current_scope != NULL && "Scope underflow");

    sema->current_scope = sema->current_scope->parent;
}

static bool strview_equals(StrView a, StrView b) {
    if (a.len != b.len) {
        return false;
    }

    return memcmp(a.data, b.data, a.len) == 0;
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

    sym->kind         = kind;
    sym->name         = name;
    sym->type         = type;
    sym->is_defined   = true;
    sym->stack_offset = 0;

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

    if (type_is_pointer(expected) && type_is_integer(actual)) {
        return true;
    }

    if (expected->kind == TYPE_BOOL && type_is_pointer(actual)) {
        return true;
    }

    if (expected->kind == TYPE_PTR && actual->kind == TYPE_STRUCT && type_equals(expected->ptr.base, actual)) {
        return true;
    }

    return false;
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

    if (type->kind == TYPE_STRUCT && type->structure.field_count == 0) {
        for (StructTypeEntry* e = sema->struct_registry; e != NULL; e = e->next) {
            if (strview_equals(e->name, type->structure.name)) {
                return e->type;
            }
        }
        sema_error(sema, (SourceLoc){ .filename = "<sema>", .line = 0, .col = 0 },
                   "unknown struct type '%.*s'", (int)type->structure.name.len, type->structure.name.data);
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
            if (expected_type && (type_is_integer(expected_type) || type_is_pointer(expected_type))) {
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
            Type* rhs_type = sema_analyze_expr(sema, expr->binary.rhs, lhs_type);

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

            if (!type_is_pointer(ptr_type)) {
                sema_error(sema, expr->loc, "subscripted value is not a pointer, got '%s'",
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
            Symbol* callee_sym = scope_lookup(sema, expr->call.callee_name);

            if (!callee_sym) {
                sema_error(sema, expr->loc, "call to undeclared procedure '%.*s'", 
                           (int)expr->call.callee_name.len, expr->call.callee_name.data);
                expr->type = type_primitive(TYPE_I64);
                return expr->type;
            }

            if (callee_sym->kind != SYM_PROC) {
                sema_error(sema, expr->loc, "'%.*s' is not a procedure", 
                           (int)expr->call.callee_name.len, expr->call.callee_name.data);
                expr->type = type_primitive(TYPE_I64);
                return expr->type;
            }

            expr->call.callee_sym = callee_sym;
            AstProc* proc_decl = (AstProc*)callee_sym->type;

            if (expr->call.arg_count != proc_decl->param_count) {
                sema_error(sema, expr->loc, "procedure '%.*s' expects %zu arguments, but %zu were provided",
                           (int)expr->call.callee_name.len, expr->call.callee_name.data,
                           proc_decl->param_count, expr->call.arg_count);
            }

            for (size_t i = 0; i < expr->call.arg_count; ++i) {
                Type* expected_param_type = (i < proc_decl->param_count) ? proc_decl->params[i].type : NULL;
                Type* arg_type = sema_analyze_expr(sema, expr->call.args[i], expected_param_type);

                if (expected_param_type && !types_are_compatible(expected_param_type, arg_type)) {
                    sema_error(sema, expr->call.args[i]->loc, 
                               "argument %zu expects type '%s', but got '%s'",
                               i + 1,
                               type_to_str(expected_param_type, sema->arena),
                               type_to_str(arg_type, sema->arena));
                }
            }

            expr->type = proc_decl->return_type;
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

            size_t var_size = (final_type && final_type->size) ? final_type->size : 8;
            size_t alloc_size = (var_size + 7) & ~7;
            sema->current_stack_offset -= (int32_t)alloc_size;

            Symbol* sym = scope_define_symbol(sema, SYM_VAR, stmt->var_decl.name, final_type, stmt->loc);
            sym->stack_offset = sema->current_stack_offset;

            stmt->var_decl.symbol = sym;
            break;
        }

        case STMT_ASSIGN: {
            Type* target_type = sema_analyze_expr(sema, stmt->assign.target, NULL);
            Type* value_type  = sema_analyze_expr(sema, stmt->assign.value, target_type);

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
                sema_error(sema, stmt->loc, "'break' statement not in loop statement");
            }
            break;
        }

        case STMT_CONTINUE: {
            if (sema->loop_depth == 0) {
                sema_error(sema, stmt->loc, "'continue' statement not in loop statement");
            }
            break;
        }

        case STMT_WHILE: {
            sema_analyze_expr(sema, stmt->while_stmt.cond, type_primitive(TYPE_BOOL));

            sema->loop_depth++;
            sema_analyze_stmt(sema, stmt->while_stmt.body);
            sema->loop_depth--;
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
    sema->current_proc         = proc;
    sema->current_stack_offset = 0;

    proc->return_type = sema_resolve_type(sema, proc->return_type);

    scope_push(sema);

    for (size_t i = 0; i < proc->param_count; ++i) {
        AstParam* p = &proc->params[i];
        p->type = sema_resolve_type(sema, p->type);

        sema->current_stack_offset -= 8;

        Symbol* sym = scope_define_symbol(sema, SYM_PARAM, p->name, p->type, p->loc);
        sym->stack_offset = sema->current_stack_offset;
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

    size_t total_stack = (size_t)(-sema->current_stack_offset);
    proc->stack_frame_size = (total_stack + 15) & ~15;

    scope_pop(sema);

    sema->current_proc = NULL;
}

void sema_init(Sema* sema, Arena* arena) {
    sema->arena                = arena;
    sema->global_scope         = ARENA_NEW_ZERO(arena, Scope);
    sema->current_scope        = sema->global_scope;
    sema->struct_registry      = NULL;
    sema->current_proc         = NULL;
    sema->current_stack_offset = 0;
    sema->loop_depth           = 0;
    sema->had_error            = false;
}

bool sema_analyze_program(Sema* sema, AstProgram* program) {
    if (!program) {
        return false;
    }

    for (size_t i = 0; i < program->struct_count; ++i) {
        AstStructDef* s = program->structs[i];

        StructTypeEntry* entry = ARENA_NEW_ZERO(sema->arena, StructTypeEntry);
        entry->name = s->name;
        entry->type = s->type;
        entry->next = sema->struct_registry;

        sema->struct_registry = entry;
    }

    for (size_t i = 0; i < program->proc_count; ++i) {
        AstProc* proc = program->procs[i];

        Symbol* sym = scope_define_symbol(sema, SYM_PROC, proc->name, (Type*)proc, proc->loc);
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