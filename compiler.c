#include "compiler.h"
#include "lisp.h"

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
Object* builtin_less(Object* scope, Object* args);
Object* builtin_add(Object* scope, Object* args);
Object* builtin_eq(Object* scope, Object* args);

bool compile_expr(uint8_t** ptr, Object* params, Object* body);

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

bool resolve_symbols(Object* scope, Object* name, Object* self, Object* params, Object* body)
{
    bool ok = true;

    if (get_type(body) == TYPE_CELL && get_type(car(body)) == TYPE_SYMBOL)
    {
        Object* sym = car(body);

        if (is_parameter(params, sym))
        {
            debug("Symbol '%s' is a parameter of the function, not a builtin function", get_symbol(sym));
        }
        else if (sym == name)
        {
            debug("Symbol '%s' points to the function itself, resolving immediately", get_symbol(sym));
            get_obj(body)->car = self;
        }
        else
        {
            Object* val = symbol_lookup(scope, sym);

            if (val == Undefined)
            {
                error("Undefined symbol: %s", get_symbol(sym));
                ok = false;
            }
            else
            {
                debug("Symbol '%s' found, resolving immediately.", get_symbol(sym));
                get_obj(body)->car = val;
            }
        }

        for (body = cdr(body); get_type(body) == TYPE_CELL; body = cdr(body))
        {
            if (get_type(car(body)) == TYPE_CELL)
            {
                if (!resolve_symbols(scope, name, self, params, car(body)))
                {
                    ok = false;
                }
            }
        }
    }

    return ok;
}

void jit_free()
{
}

bool compile_expr(uint8_t**, Object*, Object*)
{
    return false;
}

void compile_function(Object* scope, Object* args, CompileFunc compile_func)
{
    Object* name = Nil;
    Object* func = Nil;
    PUSH4(scope, args, name, func);

    for (; get_type(args) == TYPE_CELL; args = cdr(args))
    {
        if (get_type(car(args)) != TYPE_SYMBOL)
        {
            error("Argument is not a symbol");
        }
        else
        {
            name = car(args);

            if (get_type(name) == TYPE_CELL)
            {
                name = eval(scope, name);
            }

            func = symbol_lookup(scope, name);

            if (func == Undefined)
            {
                error("Undefined symbol: %s", get_symbol(name));
            }
            else if (get_type(func) != TYPE_FUNCTION)
            {
                error("Symbol '%s' does not point to a function", get_symbol(name));
            }
            else if (!compile_func(scope, name, func, func_params(func), func_body(func)))
            {
                error("Compilation failed");
            }
        }
    }

    POP();
}

void jit_resolve_symbols(Object* scope, Object* args)
{
    compile_function(scope, args, resolve_symbols);
}

void jit_compile(Object* scope, Object* args)
{
    compile_function(scope, args, resolve_symbols);
}

typedef Object* (*JitFunc)(Object**);

Object* jit_eval(Object*, Object*)
{
    return Nil;
}
