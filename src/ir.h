#ifndef KLANG_IR_H
#define KLANG_IR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "lexer.h"
#include "type.h"
#include "ast.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IRBlock    IRBlock;
typedef struct IRFunction IRFunction;
typedef struct IRModule   IRModule;

typedef enum IROperandKind {
    IR_OP_NONE = 0,
    IR_OP_CONST,   
    IR_OP_VREG,    
    IR_OP_STACK,   
    IR_OP_STR,     
    IR_OP_BLOCK    
} IROperandKind;

typedef struct IROperand {
    IROperandKind kind;
    size_t        byte_size;

    union {
        int64_t     int_val;      
        uint32_t    vreg_id;      
        int32_t     stack_offset; 
        uint32_t    str_id;       
        IRBlock*    block;        
    };
} IROperand;

typedef enum IROpcode {
    IR_NOP = 0,

    IR_LOAD,        
    IR_STORE,       
    IR_LOAD_STACK,  
    IR_STORE_STACK, 
    IR_ADDR_STACK,  
    IR_GLOBAL_STR,  

    IR_ADD, 
    IR_SUB, 
    IR_MUL, 
    IR_DIV, 
    IR_MOD, 
    IR_NEG, 
    IR_AND, 
    IR_OR,  
    IR_XOR, 
    IR_SHL, 
    IR_SHR, 
    IR_NOT, 

    IR_CMP_EQ, 
    IR_CMP_NE, 
    IR_CMP_LT, 
    IR_CMP_LE, 
    IR_CMP_GT, 
    IR_CMP_GE, 

    IR_JMP,   
    IR_BR,    
    IR_RET,   

    IR_CALL,   
    IR_PARAM,  
    IR_INLINE_ASM
} IROpcode;

typedef struct IRInst IRInst;

struct IRInst {
    IROpcode   opcode;
    IROperand  dst;
    IROperand  src1;
    IROperand  src2;

    IROperand* extra_args;
    size_t     extra_arg_count;

    StrView    symbol_name;
    SourceLoc  loc;

    IRInst*    next;
};

struct IRBlock {
    const char* name;
    uint32_t    id;

    IRInst*     first_inst;
    IRInst*     last_inst;
    size_t      inst_count;

    bool        is_terminated;

    IRBlock*    next_block;
};

struct IRFunction {
    Arena*      arena;
    
    StrView     name;
    Type*       return_type;
    size_t      stack_frame_size;

    IRBlock*    entry_block;
    IRBlock*    current_block;

    IRBlock*    first_block;
    IRBlock*    last_block;
    size_t      block_count;

    uint32_t    next_vreg_id;
    uint32_t    next_block_id;

    IRFunction* next;
};

typedef struct IRStringConst IRStringConst;

struct IRStringConst {
    uint32_t       id;
    StrView        value;
    IRStringConst* next;
};

struct IRModule {
    Arena*         arena;

    IRFunction*    first_func;
    IRFunction*    last_func;
    size_t         func_count;

    IRStringConst* first_str;
    IRStringConst* last_str;
    size_t         str_count;
};

IRModule*   ir_module_create(Arena* arena);

IRFunction* ir_function_create(IRModule* module, StrView name, Type* return_type, size_t stack_size);

IRBlock*    ir_block_create(IRFunction* func, const char* prefix);

void        ir_block_switch(IRFunction* func, IRBlock* block);

IROperand   ir_op_none(void);
IROperand   ir_op_const(int64_t val, size_t byte_size);
IROperand   ir_op_vreg(uint32_t vreg_id, size_t byte_size);
IROperand   ir_op_stack(int32_t stack_offset, size_t byte_size);
IROperand   ir_op_str(uint32_t str_id);
IROperand   ir_op_block(IRBlock* block);

IRInst*     ir_emit_inst(IRFunction* func, IROpcode op, IROperand dst, IROperand src1, IROperand src2, SourceLoc loc);

IRModule*   ir_lower_program(Arena* arena, const AstProgram* program);

void        ir_dump_module(const IRModule* module, Arena* arena);

#ifdef __cplusplus
}
#endif

#endif // KLANG_IR_H