#include "compiler.h"
#include "lisp.h"

// Only x86-64 is supported currently
#include "impl/x86_64.h"

#define COMPILE_MEM_SIZE 4096

struct CompiledFunction
{
    void* memory;
    Object* name;
    struct CompiledFunction* next;
};

typedef struct CompiledFunction CompiledFunction;

CompiledFunction* compiled_functions = NULL;

typedef bool (*CompileFunc)(Object* scope, Object* name, Object* self, Object* params, Object* body);

// The declarations for builtins that we know of and can compile
Object* builtin_if(Object* scope, Object* args);
Object* builtin_less(Object* scope, Object* args);
Object* builtin_add(Object* scope, Object* args);
Object* builtin_sub(Object* scope, Object* args);
Object* builtin_eq(Object* scope, Object* args);
Object* builtin_car(Object* scope, Object* args);
Object* builtin_cdr(Object* scope, Object* args);

bool is_supported_builtin(Function fn)
{
    return fn == builtin_if
        || fn == builtin_less
        || fn == builtin_add
        || fn == builtin_sub
        || fn == builtin_eq
        || fn == builtin_car
        || fn == builtin_cdr;

}

bool compile_expr(uint8_t** mem, Object* self, Object* params, Object* body);

bool is_parameter(Object* params, Object* value)
{
    for (Object* p = params; get_type(p) == TYPE_CELL; p = cdr(p))
    {
        if (car(p) == value)
        {
            return true;
        }
    }

    return false;
}

Object* resolve_one_symbol(Object* scope, Object* name, Object* self, Object* params, Object* sym)
{
    if (is_parameter(params, sym))
    {
        debug("Symbol '%s' is a parameter of the function, not a builtin function", get_symbol(sym));
        return sym;
    }
    else if (sym == name)
    {
        debug("Symbol '%s' points to the function itself, resolving immediately", get_symbol(sym));
        return self;
    }

    Object* val = symbol_lookup(scope, sym);

    if (val == Undefined)
    {
        error("Undefined symbol: %s", get_symbol(sym));
    }
    else
    {
        debug("Symbol '%s' found, resolving immediately.", get_symbol(sym));
    }

    return val;
}

bool resolve_symbols(Object* scope, Object* name, Object* self, Object* params, Object* body)
{
    assert(get_type(body) == TYPE_CELL);

    for (; body != Nil; body = cdr(body))
    {
        Object* val = car(body);
        int type = get_type(val);

        if (type == TYPE_SYMBOL)
        {
            val = resolve_one_symbol(scope, name, self, params, val);

            if (val == Undefined)
            {
                return false;
            }
            else
            {
                get_obj(body)->car = val;
            }
        }
        else if (type == TYPE_CELL)
        {
            if (!resolve_symbols(scope, name, self, params, val))
            {
                return false;
            }
        }
    }

    return true;
}

bool valid_for_compile(Object* self, Object* params, Object* body)
{
    int type = get_type(body);
    if (type == TYPE_NUMBER || type == TYPE_CONST)
    {
        debug("Constant expression, trivial to implement");
        debug_print(body);
        return true;
    }
    else if (type == TYPE_SYMBOL && is_parameter(params, body))
    {
        debug("Body refers to one of the parameters, trivial to implement");
        debug_print(body);
        return true;
    }
    else if (type != TYPE_CELL)
    {
        error("Cannot compile, function body is not a list or a constant:");
        printf("%d\n", type);
        debug_print(body);
        return false;
    }

    Object* func = car(body);

    if (func == self)
    {
        debug("Self-recursive function");
    }
    else if (get_type(func) != TYPE_BUILTIN)
    {
        error("Not a builtin, too complex");
        print(body);
        return false;
    }
    else if (!is_supported_builtin(get_obj(func)->fn))
    {
        error("Builtin not supported, too complex");
        print(body);
        return false;
    }

    assert(get_type(car(body)) == TYPE_BUILTIN || func == self);
    debug("Builtin function or self-recursion, checking all arguments");
    debug_print(body);

    for (body = cdr(body); get_type(body) == TYPE_CELL; body = cdr(body))
    {
        if (!valid_for_compile(self, params, car(body)))
        {
            return false;
        }
    }

    return true;
}

void jit_free()
{
    while (compiled_functions)
    {
        CompiledFunction* comp = compiled_functions;
        compiled_functions = compiled_functions->next;
        munmap(comp->memory, COMPILE_MEM_SIZE);
        free(comp);
    }
}

bool compile_argument(uint8_t** mem, Object* params, Object* arg)
{
    uint8_t i = 0;

    while (params != Nil)
    {
        if (car(params) == arg)
        {
            break;
        }

        i++;
        params = cdr(params);
    }

    if (params == Nil)
    {
        assert(arg != Nil);
        error("Unknown parameter.");
        return false;
    }

    EMIT_MOV64_REG_OFF8(REG_RET, REG_ARGS, i * OBJ_SIZE);
    return true;
}

bool compile_immediate(uint8_t** mem, Object* arg)
{
    EMIT_MOV64_REG_IMM64(REG_RET, (intptr_t)arg);
    return true;
}

bool compile_add(uint8_t** mem, Object* self, Object* params, Object* args)
{
    EMIT_MOV64_PTR_IMM32(REG_STACK, 0);
    EMIT_ADD64_IMM8(REG_STACK, OBJ_SIZE);

    for (; args != Nil; args = cdr(args))
    {
        compile_expr(mem, self, params, car(args));
        EMIT_ADD64_OFF8_REG(REG_STACK, REG_RET, -8);
    }

    EMIT_ADD64_IMM8(REG_STACK, -OBJ_SIZE);
    EMIT_MOV64_REG_PTR(REG_RET, REG_STACK);
    return true;
}

bool compile_sub(uint8_t** mem, Object* self, Object* params, Object* args)
{
    if (length(args) == 1)
    {
        compile_expr(mem, self, params, car(args));
        EMIT_NEG64(REG_RET);
    }
    else
    {
        assert(length(args) > 1);
        compile_expr(mem, self, params, car(args));
        EMIT_MOV64_PTR_REG(REG_STACK, REG_RET);
        EMIT_ADD64_IMM8(REG_STACK, OBJ_SIZE);

        for (args = cdr(args); args != Nil; args = cdr(args))
        {
            compile_expr(mem, self, params, car(args));
            EMIT_SUB64_OFF8_REG(REG_STACK, REG_RET, -8);
        }

        EMIT_ADD64_IMM8(REG_STACK, -OBJ_SIZE);
        EMIT_MOV64_REG_PTR(REG_RET, REG_STACK);
    }
    return true;
}

bool compile_less(uint8_t** mem, Object* self, Object* params, Object* args)
{
    EMIT_ADD64_IMM8(REG_STACK, OBJ_SIZE);

    compile_expr(mem, self, params, car(args));
    EMIT_SAR64_IMM8(REG_RET, 2);
    EMIT_MOV64_OFF8_REG(REG_STACK, REG_RET, -8);
    compile_expr(mem, self, params, car(cdr(args)));
    EMIT_SAR64_IMM8(REG_RET, 2);

    EMIT_ADD64_IMM8(REG_STACK, -OBJ_SIZE);
    EMIT_CMP64_REG_PTR(REG_RET, REG_STACK);
    EMIT_MOV64_REG_IMM64(REG_RET, (intptr_t)True);
    EMIT_JL_OFF8();
    uint8_t* jump_start = *mem;

    EMIT_MOV64_REG_IMM64(REG_RET, (intptr_t)Nil);
    uint8_t* jump_end = *mem;
    PATCH_JMP8(jump_start - 1, jump_end - jump_start);

    return true;
}

bool compile_eq(uint8_t** mem, Object* self, Object* params, Object* args)
{
    EMIT_ADD64_IMM8(REG_STACK, OBJ_SIZE);

    compile_expr(mem, self, params, car(args));
    EMIT_MOV64_OFF8_REG(REG_STACK, REG_RET, -8);
    compile_expr(mem, self, params, car(cdr(args)));

    EMIT_ADD64_IMM8(REG_STACK, -OBJ_SIZE);
    EMIT_CMP64_REG_PTR(REG_RET, REG_STACK);
    EMIT_MOV64_REG_IMM64(REG_RET, (intptr_t)True);
    EMIT_JE_OFF8();
    uint8_t* jump_start = *mem;

    EMIT_MOV64_REG_IMM64(REG_RET, (intptr_t)Nil);
    uint8_t* jump_end = *mem;
    PATCH_JMP8(jump_start - 1, jump_end - jump_start);

    return true;
}

bool compile_if(uint8_t** mem, Object* self, Object* params, Object* args)
{
    compile_expr(mem, self, params, car(args));
    EMIT_CMP64_REG_IMM8(REG_RET, (intptr_t)Nil);
    EMIT_JE_OFF8();
    uint8_t* jump_to_false = *mem;
    compile_expr(mem, self, params, car(cdr(args)));
    EMIT_JMP_OFF8();
    uint8_t* jump_to_end = *mem;
    compile_expr(mem, self, params, car(cdr(cdr(args))));
    uint8_t* end = *mem;
    PATCH_JMP8(jump_to_false - 1, jump_to_end - jump_to_false);
    PATCH_JMP8(jump_to_end - 1, end - jump_to_end);

    return true;
}

bool compile_car(uint8_t** mem, Object* self, Object* params, Object* args)
{
    compile_expr(mem, self, params, car(args));
    EMIT_MOV64_REG_OFF8(REG_RET, REG_RET, -TYPE_CELL + offsetof(Object, car));
    return true;
}

bool compile_cdr(uint8_t** mem, Object* self, Object* params, Object* args)
{
    compile_expr(mem, self, params, car(args));
    EMIT_MOV64_REG_OFF8(REG_RET, REG_RET, -TYPE_CELL + offsetof(Object, cdr));
    return true;
}

bool compile_recursion(uint8_t** mem, Object* self, Object* params, Object* args)
{
    int len = length(args);
    int pos = 0;
    EMIT_ADD64_IMM8(REG_STACK, OBJ_SIZE * len);

    for (; args != Nil; args = cdr(args))
    {
        compile_expr(mem, self, params, car(args));
        EMIT_MOV64_OFF8_REG(REG_STACK, REG_RET, -8 * (len - pos));
        pos++;
    }

    for (int i = 0; i < len; i++)
    {
        EMIT_MOV64_REG_OFF8(REG_RET, REG_STACK, -8 * (len - i));
        EMIT_MOV64_OFF8_REG(REG_ARGS, REG_RET, i * 8);
    }

    EMIT_ADD64_IMM8(REG_STACK, -OBJ_SIZE * len);

    // Patch the offset right away
    EMIT_JMP32();
    uint8_t* start = (uint8_t*)func_body(self);
    ptrdiff_t backwards = start - *mem - 4; // The extra 4 is for the imm32 that we emit right now
    EMIT_IMM32(backwards);

    return true;
}

bool compile_expr(uint8_t** mem, Object* self, Object* params, Object* obj)
{
    switch (get_type(obj))
    {
    case TYPE_CELL:
        {
            Object* fn = car(obj);

            if (fn == self)
            {
                return compile_recursion(mem, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_add)
            {
                return compile_add(mem, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_sub)
            {
                return compile_sub(mem, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_less)
            {
                return compile_less(mem, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_eq)
            {
                return compile_eq(mem, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_car)
            {
                return compile_car(mem, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_cdr)
            {
                return compile_cdr(mem, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_if)
            {
                return compile_if(mem, self, params, cdr(obj));
            }
            else
            {
                error("Unknown builtin function");
            }
            break;
        }
    case TYPE_SYMBOL:
        return compile_argument(mem, params, obj);

    case TYPE_CONST:
    case TYPE_NUMBER:
        return compile_immediate(mem, obj);

    default:
        return compile_immediate(mem, Nil);
    }

    return false;
}

bool generate_bytecode(uint8_t** mem, Object* /*scope*/, Object* /*name*/, Object* self, Object* params, Object* body)
{
    bool ok = compile_expr(mem, self, params, body);
    EMIT_RET();
    return ok;
}

bool compile_to_bytecode(Object* scope, Object* name, Object* self, Object* params, Object* body)
{
    if (!valid_for_compile(self, params, body))
    {
        return false;
    }

    PUSH5(scope, name, self, params, body);

    void* memory = mmap(NULL, COMPILE_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t* ptr = (uint8_t*)memory;
    Object* old_body = func_body(self);
    // The body is used to store the pointer that self-recursive functions need
    get_obj(self)->func_body = (Object*) memory;
    bool ok = generate_bytecode(&ptr, scope, name, self, params, body);

    if (ok)
    {
        mprotect(memory, COMPILE_MEM_SIZE, PROT_READ | PROT_EXEC);

        debug("Compiled into %lu bytes.", ptr - (uint8_t*)memory);

        if (debug_on())
        {
            int pid = getpid();
            char buffer[1024];
            snprintf(buffer, sizeof(buffer), "gdb --pid=%d --batch --silent -ex 'disassemble /r %p,%p'", pid, memory, ptr);
            system(buffer);
        }

        get_obj(self)->compiled = COMPILE_CODE;

        CompiledFunction* comp = malloc(sizeof(CompiledFunction));
        comp->memory = memory;
        comp->name = name;
        comp->next = compiled_functions;
        compiled_functions = comp;
    }
    else
    {
        get_obj(self)->func_body = old_body;
        munmap(memory, COMPILE_MEM_SIZE);
    }

    POP();
    return ok;
}

bool compile_function(Object* scope, Object* args, CompileFunc compile_func, uint8_t compile_level)
{
    bool ok = true;
    Object* name = Nil;
    Object* func = Nil;
    PUSH4(scope, args, name, func);

    for (; get_type(args) == TYPE_CELL; args = cdr(args))
    {
        if (get_type(car(args)) != TYPE_SYMBOL)
        {
            error("Argument is not a symbol");
            ok = false;
        }
        else
        {
            name = car(args);

            if (get_type(name) == TYPE_CELL)
            {
                name = eval(scope, name);
            }

            debug("<<< Compiling '%s' >>>", get_symbol(name));

            func = symbol_lookup(scope, name);

            if (func == Undefined)
            {
                error("Undefined symbol: %s", get_symbol(name));
                ok = false;
            }
            else if (get_type(func) != TYPE_FUNCTION)
            {
                error("Symbol '%s' does not point to a function", get_symbol(name));
                ok = false;
            }
            else if (!compile_func(scope, name, func, func_params(func), func_body(func)))
            {
                error("Compilation of '%s' failed", get_symbol(name));
                ok = false;
            }
            else
            {
                get_obj(func)->compiled = compile_level;
            }
        }
    }

    POP();
    return ok;
}

void jit_resolve_symbols(Object* scope, Object* args)
{
    compile_function(scope, args, resolve_symbols, COMPILE_SYMBOLS);
}

void jit_compile(Object* scope, Object* args)
{
    if (compile_function(scope, args, resolve_symbols, COMPILE_SYMBOLS))
    {
        compile_function(scope, args, compile_to_bytecode, COMPILE_CODE);
    }
}

// The JIT functions receive their arguments in RDI and a "stack pointer" in RSI
typedef Object* (*JitFunc)(Object**, Object**);

Object* jit_eval(Object* fn, Object* args)
{
    assert(get_type(fn) == TYPE_FUNCTION);
    assert(get_obj(fn)->compiled == COMPILE_CODE);
    int len = length(args);
    int required = length(func_params(fn));

    if (len != required)
    {
        error("Argument length mismatch: expected %d, have %d.", required, len);
        return Nil;
    }

    Object* arg_stack[len + 1];
    Object* stack[JIT_STACK_SIZE];

    // The function arguments are bound in the reverse order they are declared to the
    // scope. Each value is copied into the stack buffer that is then passed
    // into the compiled function. The compiled functions expect the arguments
    // to be stored in RDI and a temporary stack pointer to be in RSI.
    for (Object* o = args; o != Nil; o = cdr(o))
    {
        arg_stack[--len] = cdr(car(o));
    }

    JitFunc func = (JitFunc)func_body(fn);
    return func(arg_stack, stack);
}
