#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <time.h>

#define MAX_SYMBOL_LEN 1024

#define ALWAYS_GC 0

enum ObjectType {TYPE_CONST    = 0x01,
                 TYPE_NUMBER   = 0x02,
                 TYPE_SYMBOL   = 0x04,
                 TYPE_BUILTIN  = 0x08,
                 TYPE_CELL     = 0x10,
                 TYPE_FUNCTION = 0x20,
                 TYPE_MACRO    = 0x40,
                 TYPE_MOVED    = 0x80};

struct Object;
typedef struct Object Object;
typedef struct Object* (*Function) (struct Object*, struct Object*);

struct Object
{
    uint8_t type;

    union
    {
        // Number
        int64_t number;

        // Cons cell
        struct {
            Object* car;
            Object* cdr;
        };

        // Symbol
        char name[1];

        // Builtin function
        Function fn;

        // Custom functions
        struct {
            Object* func_params;
            Object* func_body;
            Object* func_env;
            bool    compiled;
        };

        // The "301 Moved Permanently" pointer that's set during GC
        Object* moved;

        // The special value where the return value is stashed for tail calls
        struct {
            Object* tail_expr;
            Object* tail_scope;
        };
    };
};

#define MAX_VARS 7
#define ALLOC_ALIGN _Alignof(Object)

#define BASE_SIZE offsetof(Object, number)

struct Frame
{
    struct Frame* next;
    Object** vars[MAX_VARS];
};

typedef struct Frame Frame;
Frame* stack_top = NULL;

#define ENTER() Frame frame; frame.next = stack_top; stack_top = &frame
#define SETEND(n) frame.vars[n] = NULL
#define PUSH1(a) ENTER(); frame.vars[0] = &a; SETEND(1)
#define PUSH2(a, b) PUSH1(a); frame.vars[1] = &b; SETEND(2)
#define PUSH3(a, b, c) PUSH2(a, b); frame.vars[2] = &c; SETEND(3)
#define PUSH4(a, b, c, d) PUSH3(a, b, c); frame.vars[3] = &d; SETEND(4)
#define PUSH5(a, b, c, d, e) PUSH4(a, b, c, d); frame.vars[4] = &e; SETEND(5)
#define PUSH6(a, b, c, d, e, f) PUSH5(a, b, c, d, e); frame.vars[5] = &f; SETEND(6)
#define PUSH7(a, b, c, d, e, f, g) PUSH6(a, b, c, d, e, f); frame.vars[6] = &g;
#define POP() stack_top = frame.next;

Object Nil_obj = {.type = TYPE_CONST, .number = 0};
#define Nil (&Nil_obj)

Object True_obj = {.type = TYPE_CONST, .number = 0};
#define True (&True_obj)

Object TailCall_obj = {.type = TYPE_CONST, .number = 0};
#define TailCall (&TailCall_obj)

Object* AllSymbols = Nil;
Object* Env = Nil;

#define CHECK0ARGS(args) args != Nil
#define CHECK1ARGS(args) args->type != TYPE_CELL || CHECK0ARGS(args->cdr)
#define CHECK2ARGS(args) args->type != TYPE_CELL || CHECK1ARGS(args->cdr)
#define CHECK3ARGS(args) args->type != TYPE_CELL || CHECK2ARGS(args->cdr)

FILE* input;
uint8_t* mem_root;
uint8_t* mem_end;
uint8_t* mem_ptr;
bool grow_memory = false;
bool is_running = true;
bool echo = false;
bool verbose_gc = false;
bool quiet = false;
int debug_step = 0;
int debug_depth = 0;
size_t memory_size = 1024 * 1024;
double memory_pct = 75.0;

void debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void print(Object* obj);
void print_one(Object* obj);

char error_stack[16][128];
uint8_t error_ptr = 0;

void error(const char* fmt, ...)
{
    int slot = error_ptr % 16;
    va_list args;
    va_start(args, fmt);
    vsnprintf(error_stack[slot], sizeof(error_stack[0]), fmt, args);
    va_end(args);
    error_ptr++;
}

#ifdef NDEBUG
#define is_debug false
#define is_stack_trace false
void debug(const char*, ...)
{
}
#else
bool is_debug = false;
bool is_stack_trace = false;
void debug(const char* fmt, ...)
{
    if (is_debug)
    {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        printf("\n");
    }
}
#endif

// Memory allocation and object sizes

size_t allocation_size(size_t size)
{
    size = ((size + ALLOC_ALIGN - 1) / ALLOC_ALIGN) * ALLOC_ALIGN;
    // All objects must be able to hold the "moved" pointer value and thus the
    // minimum allocation size is 16 bytes, assuming 8 bytes for the pointer and
    // the 8 bytes that the alignment of the union requires.
    const size_t min_size = BASE_SIZE + sizeof(Object*);
    return size > min_size ? size : min_size;
}

size_t type_size(enum ObjectType type)
{
    switch (type)
    {
    case TYPE_NUMBER:
        return allocation_size(BASE_SIZE + sizeof(int64_t));
    case TYPE_SYMBOL:
        // The actual size of the symbol is determined by the length of the name
        return allocation_size(BASE_SIZE);
    case TYPE_CELL:
        return allocation_size(BASE_SIZE + sizeof(Object*) * 2);
    case TYPE_FUNCTION:
    case TYPE_MACRO:
        return allocation_size(BASE_SIZE + sizeof(Object*) * 3 + sizeof(bool));
    case TYPE_BUILTIN:
        return allocation_size(BASE_SIZE + sizeof(Function));
    case TYPE_CONST:
    default:
        assert(false);
        return 0;
    }
}

size_t object_size(Object* obj)
{
    if (obj->type == TYPE_SYMBOL)
    {
        return allocation_size(BASE_SIZE + strlen(obj->name) + 1);
    }

    return type_size(obj->type);
}

// Garbage collection

Object* make_living(Object* obj)
{
    if (obj == Nil || obj == True)
    {
        // Constant type
        return obj;
    }
    else if (obj->type != TYPE_MOVED)
    {
        size_t size = object_size(obj);
        assert(mem_ptr + size <= mem_end);
        memcpy(mem_ptr, obj, size);
        obj->type = TYPE_MOVED;
        obj->moved = (Object*)mem_ptr;
        mem_ptr += size;
    }

    assert(obj->type == TYPE_MOVED);
    return obj->moved;
}

void fix_references(Object* obj)
{
    switch (obj->type)
    {
    case TYPE_SYMBOL:
    case TYPE_NUMBER:
    case TYPE_BUILTIN:
        break;

    case TYPE_CELL:
        obj->car = make_living(obj->car);
        obj->cdr = make_living(obj->cdr);
        break;

    case TYPE_FUNCTION:
    case TYPE_MACRO:
        obj->func_params = make_living(obj->func_params);
        obj->func_body = make_living(obj->func_body);
        obj->func_env = make_living(obj->func_env);
        break;

    case TYPE_CONST:
    case TYPE_MOVED:
    default:
        assert(!true);
        break;
    }
}

void collect_garbage()
{
    size_t space_size = memory_size / 2;
    size_t memory_used = mem_ptr - mem_root;
    uint8_t* old_root = NULL;

    if (grow_memory)
    {
        old_root = mem_root;
        memory_size *= 2;
        space_size = memory_size / 2;
        mem_root = aligned_alloc(ALLOC_ALIGN, memory_size);
        mem_ptr = mem_root;
        mem_end = mem_root + memory_size / 2;
    }
    else if (mem_end == mem_root + space_size)
    {
        mem_ptr = mem_root + space_size;
        mem_end = mem_root + memory_size;
    }
    else
    {
        memory_used -= space_size;
        mem_ptr = mem_root;
        mem_end = mem_root + space_size;
    }

    uint8_t* scan_start = mem_ptr;
    uint8_t* scan_ptr = scan_start;

    Env = make_living(Env);
    AllSymbols = make_living(AllSymbols);

    for (Frame* f = stack_top; f; f = f->next)
    {
        for (int i = 0; i < MAX_VARS && f->vars[i]; i++)
        {
            *f->vars[i] = make_living(*f->vars[i]);
        }
    }

    while (scan_ptr < mem_ptr)
    {
        Object* o = (Object*)scan_ptr;
        fix_references(o);
        scan_ptr += object_size(o);
    }

    assert(scan_ptr == mem_ptr);

    size_t still_in_use = scan_ptr - scan_start;
    double pct_in_use = ((double)still_in_use / (double)space_size) * 100.0;

    if (verbose_gc)
    {
        size_t memory_freed = memory_used - still_in_use;

        if (grow_memory)
        {
            printf("\nMemory resized: %lu -> %lu\n", memory_size / 2, memory_size);
        }

        if (memory_freed)
        {
            double pct_freed = ((double)memory_freed / (double)space_size) * 100.0;
            printf("\nMemory freed: %lu (%.1lf%%) Memory used: %lu (%.1lf%%)\n", memory_freed, pct_freed, still_in_use, pct_in_use);
        }
    }

    if (grow_memory)
    {
        grow_memory = false;
        free(old_root);
    }
    else if (pct_in_use > memory_pct)
    {
        grow_memory = true;
    }
}

// Object creation
Object* symbol_lookup(Object* scope, Object* sym);

Object* allocate(size_t size)
{
#if ALWAYS_GC
    collect_garbage();
#endif
    assert(allocation_size(size) == size);

    if (mem_ptr + size > mem_end)
    {
        collect_garbage();
    }

    Object* rv = (Object*)mem_ptr;
    mem_ptr += size;
    return rv;
}

Object* cons(Object* car, Object* cdr)
{
    PUSH2(car, cdr);
    Object* rv = allocate(type_size(TYPE_CELL));
    rv->type = TYPE_CELL;
    rv->car = car;
    rv->cdr = cdr;
    POP();
    return rv;
}

Object* car(Object* obj)
{
    return obj->type == TYPE_CELL ? obj->car : Nil;
}

Object* cdr(Object* obj)
{
    return obj->type == TYPE_CELL ? obj->cdr : Nil;
}

Object* make_number(int64_t val)
{
    Object* rv = allocate(type_size(TYPE_NUMBER));
    rv->type = TYPE_NUMBER;
    rv->number = val;
    return rv;
}

Object* make_symbol(const char* name)
{
    size_t sz = allocation_size(BASE_SIZE + strlen(name) + 1);
    Object* rv = allocate(sz);
    rv->type = TYPE_SYMBOL;
    strcpy(rv->name, name);
    return rv;
}

Object* make_builtin(Function func)
{
    Object* rv = allocate(type_size(TYPE_BUILTIN));
    rv->type = TYPE_BUILTIN;
    rv->fn = func;
    return rv;
}

Object* make_function(Object* params, Object* body, Object* env)
{
    PUSH3(params, body, env);
    Object* rv = allocate(type_size(TYPE_FUNCTION));
    rv->type = TYPE_FUNCTION;
    rv->func_params = params;
    rv->func_body = body;
    rv->func_env = env;
    rv->compiled = false;
    POP();
    return rv;
}

Object* new_scope(Object* prev_scope)
{
    return cons(Nil, prev_scope);
}

Object* symbol(const char* name)
{
    for (Object* o = AllSymbols; o != Nil; o = o->cdr)
    {
        if (strcmp(o->car->name, name) == 0)
        {
            return o->car;
        }
    }

    Object* sym = make_symbol(name);
    AllSymbols = cons(sym, AllSymbols);
    return AllSymbols->car;
}

void bind_value(Object* scope, Object* symbol, Object* value)
{
    if (is_debug)
    {
        printf("Binding '%s' to ", symbol->name);
        print(value);
    }

    Object* bound = Nil;
    PUSH4(scope, symbol, value, bound);
    bound = cons(symbol, value);
    Object* res = cons(bound, scope->car);
    scope->car = res;
    POP();
}

void define_builtin_function(const char* name, Function fn)
{
    Object* func = Nil;
    Object* sym = Nil;
    PUSH2(func, sym);

    func = make_builtin(fn);
    sym = symbol(name);

    bind_value(Env, sym, func);
    POP();
}

void define_alias(const char* name, const char* alias)
{
    Object* sym = Nil;
    Object* sym_alias = Nil;
    Object* val = Nil;
    PUSH3(sym, sym_alias, val);

    sym = symbol(name);
    sym_alias = symbol(alias);
    val = symbol_lookup(Env, sym);

    if (val)
    {
        bind_value(Env, sym_alias, val);
    }
    else
    {
        error("Undefined symbol: %s", name);
    }

    POP();
}

void define_constant(const char* name, Object* value)
{
    Object* sym = Nil;
    PUSH2(value, sym);
    sym = symbol(name);
    bind_value(Env, sym, value);
    POP();
}

void print_scope(Object* scope)
{
    int num_scopes = 0;

    for (Object* s = scope; s != Nil; s = s->cdr)
    {
        ++num_scopes;
    }

    for (Object* s = scope; s != Nil; s = s->cdr)
    {
        printf("===== Scope %d =====\n", num_scopes--);
        print(s->car);
    }
}

Object* symbol_lookup(Object* scope, Object* sym)
{
    for (Object* s = scope; s != Nil; s = s->cdr)
    {
        for (Object* o = s->car; o != Nil; o = o->cdr)
        {
            assert(o->car->type = TYPE_CELL);

            if (o->car->car == sym)
            {
                if (is_debug)
                {
                    printf("Symbol '%s' points to ", o->car->car->name);
                    print(o->car->cdr);
                }

                return o->car->cdr;
            }
        }
    }

    return NULL;
}

void print_one(Object* obj)
{
    assert(obj);
    assert(obj != TailCall);

    switch (obj->type)
    {
    case TYPE_NUMBER:
        printf("%ld ", obj->number);
        break;
    case TYPE_SYMBOL:
        printf("%s ", obj->name);
        break;
    case TYPE_CONST:
        if (obj == True)
        {
            printf("t ");
        }
        else
        {
            assert(obj == Nil);
            printf("nil ");
        }
        break;
    case TYPE_CELL:
        {
            printf("( ");
            Object* o = obj;

            for (; o->type == TYPE_CELL; o = o->cdr)
            {
                print_one(o->car);
            }

            if (o != Nil)
            {
                printf(". ");
                print_one(o);
            }
            printf(") ");
        }
        break;
    case TYPE_FUNCTION:
        if (obj->compiled)
        {
            printf("<compiled func> ");
        }
        else
        {
            printf("<func> ");
            if (is_debug)
            {
                print_one(obj->func_params);
                print_one(obj->func_body);
            }
        }
        break;
    case TYPE_MACRO:
        printf("<macro> ");
        break;
    case TYPE_BUILTIN:
        printf("<builtin> ");
        break;
    default:
        assert(false);
        break;
    }
}

void print(Object* obj)
{
    print_one(obj);
    printf("\n");
}

// Parsing and tokenization

int peek()
{
    int ch = getc(input);
    ungetc(ch, input);
    return ch;
}

int get()
{
    int rc = getc(input);

    if (echo && rc != EOF && rc != '\n' && rc != '\r')
    {
        printf("%c", rc);
    }

    return rc;
}

Object* reverse(Object* list)
{
    assert(list);
    Object* newlist = Nil;

    while (list != Nil)
    {
        Object* next = list->cdr;
        list->cdr = newlist;
        newlist = list;
        list = next;
    }

    return newlist;
}

int length(Object* list)
{
    int i = 0;

    for (; list != Nil; list = list->cdr)
    {
        i++;
    }

    return i;
}

Object* parse_expr();

Object* parse_list()
{
    Object* value = Nil;
    Object* obj = Nil;
    PUSH2(value, obj);
    assert(peek() == '(');
    get();
    obj = parse_expr();

    while (obj)
    {
        value = cons(obj, value);
        obj = parse_expr();
    }

    POP();
    return reverse(value);
}

Object* parse_number()
{
    char ch = get();
    uint64_t val = ch - '0';

    while (isdigit(peek()))
    {
        ch = get();
        val = ch - '0' + val * 10;

        if (val >= LONG_MAX)
        {
            error("Integer overflow");
            return Nil;
        }
    }

    int64_t ival = val;
    return make_number(ival);
}

Object* parse_quote()
{
    Object* fn = symbol("quote");
    Object* arg = Nil;
    Object* arg_list = Nil;
    PUSH3(fn, arg, arg_list);
    assert(peek() == '\'');
    get();
    arg = parse_expr();
    arg_list = cons(arg, Nil);
    POP();
    return cons(fn, arg_list);
}

Object* parse_symbol()
{
    char name[MAX_SYMBOL_LEN];
    char* ptr = name;
    char ch = peek();

    while (ch != ')' && ch != '(' && !isspace(ch))
    {
        *ptr++ = get();
        ch = peek();

        if (ptr == name + MAX_SYMBOL_LEN)
        {
            error("Symbol name too long");
            return NULL;
        }
    }

    *ptr = '\0';
    return symbol(name);
}

Object* parse_expr()
{
    while (true)
    {
        switch (peek())
        {
        case ';':
            {
                char c = get();

                while (c != '\n' && c != EOF)
                {
                    c = get();
                }

                if (echo)
                {
                    printf("\n");
                }
                assert(c == '\n' || c == EOF);
            }
            break;

        case ' ':
        case '\t':
        case '\r':
        case '\n':
            get();
            break;

        case '(':
            return parse_list();

        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            return parse_number();

        case '-':
            {
                get();
                Object* o;

                if (isdigit(peek()))
                {
                    o = parse_number();
                    o->number = -o->number;
                }
                else if (isspace(peek()))
                {
                    o = symbol("-");
                }
                else
                {
                    o = parse_symbol();
                    memmove(o->name + 1, o->name, strlen(o->name));
                    o->name[0] = '-';
                }

                return o;
            }

        case '\'':
            return parse_quote();

        case ')':
            get();
            return NULL;

        case EOF:
            return NULL;

        default:
            return parse_symbol();
        };
    }

    return NULL;
}
// Evaluation

Object* eval(Object* scope, Object* obj);

Object* expand_macro(Object* scope, Object* macro, Object* args)
{
    Object* param = macro->func_params;
    PUSH4(macro, param, args, scope);
    scope = new_scope(scope);

    while (param != Nil && args != Nil)
    {
        assert(param->type == TYPE_CELL);

        if (args->type != TYPE_CELL)
        {
            break;
        }

        bind_value(scope, param->car, args->car);
        param = param->cdr;
        args = args->cdr;
    }

    Object* ret = Nil;

    if (args != Nil)
    {
        if (args->type == TYPE_CELL)
        {
            error("Too many arguments to macro");
        }
        else
        {
            error("Invalid argument type:");
            print(args);
        }
    }
    else if (param != Nil)
    {
        error("Not enough arguments to macro");
    }
    else
    {
        ret = eval(scope, macro->func_body);
    }

    POP();
    return ret;
}

Object* eval_cell(Object* scope, Object* obj)
{
    Object* ret = Nil;
    Object* fn = Nil;
    Object* param = Nil;
    Object* arg = Nil;
    Object* next_scope = Nil;
    PUSH7(scope, obj, ret, fn, param, arg, next_scope);

 start:

    fn = eval(scope, obj->car);

    if (fn->type == TYPE_MACRO)
    {
        ret = expand_macro(scope, fn, obj->cdr);
        ret = eval(scope, ret);
    }
    else if (fn->type == TYPE_BUILTIN)
    {
        ret = fn->fn(scope, obj->cdr);
    }
    else if (fn->type == TYPE_FUNCTION)
    {
        next_scope = new_scope(fn->func_env != Nil ? fn->func_env : scope);
        param = fn->func_params;
        arg = obj->cdr;
        assert(param == Nil || param->type == TYPE_CELL);
        assert(arg == Nil || arg->type == TYPE_CELL);

        while (param != Nil && arg != Nil)
        {
            ret = eval(scope, arg->car);
            bind_value(next_scope, param->car, ret);
            param = param->cdr;
            arg = arg->cdr;
        }

        if (param != Nil)
        {
            error("Not enough arguments to function '%s'. Expected %d, have %d.",
                  obj->car->type == TYPE_SYMBOL ? obj->car->name : "<func>",
                  length(fn->func_params), length(obj->cdr));
        }
        else if (arg != Nil)
        {
            error("Too many arguments to function '%s'. Expected %d, have %d.",
                  obj->car->type == TYPE_SYMBOL ? obj->car->name : "<func>",
                  length(fn->func_params), length(obj->cdr));
        }
        else
        {
            if (fn->func_body->type == TYPE_CELL)
            {
                debug("Function body is a list, evaluating in the same frame:");
                obj = fn->func_body;
                scope = next_scope;
                goto start;
            }

            ret = eval(next_scope, fn->func_body);
        }

        if (is_debug)
        {
            printf("Return from: ");
            print(fn->func_body);
        }
    }
    else
    {
        error("Not a function:");
        print(fn);
    }

    if (ret == TailCall)
    {
        obj = ret->tail_expr;
        scope = ret->tail_scope;
        ret = Nil;

        if (obj->type == TYPE_CELL)
        {
            if (is_stack_trace)
            {
                printf("Doing tail call: ");
                print(obj);
                printf(":::::::::::: DO TAIL :::::::::::::::::\n");
                print(scope->car);
            }

            goto start;
        }

        if (is_stack_trace)
        {
            printf("NOT doing tail call: ");
            print(obj);
            printf(":::::::::::: DO NOT TAIL :::::::::::::::::\n");
            print(scope->car);
        }

        // Not a list, evalue it here
        ret = eval(scope, obj);
    }

    POP();
    return ret;
}

Object* eval(Object* scope, Object* obj)
{
    assert(obj);
    Object* ret;

#ifndef NDEBUG
    PUSH2(scope, obj);
#endif

    if (is_stack_trace)
    {
        printf("EVAL %d (%d) ", debug_step++, debug_depth);
        for (int i = 0; i < debug_depth; i++)
        {
            printf(". ");
        }

        printf(": ");
        print(obj);
        ++debug_depth;
    }

    switch (obj->type)
    {
    case TYPE_CONST:
    case TYPE_NUMBER:
    case TYPE_BUILTIN:
    case TYPE_FUNCTION:
    case TYPE_MACRO:
        ret = obj;
        break;

    case TYPE_SYMBOL:
        ret = symbol_lookup(scope, obj);

        if (!ret)
        {
            ret = Nil;
            error("Undefined symbol: %s", obj->name);

            if (is_debug)
            {
                print_scope(scope);
            }

        }
        break;

    default:
        assert(obj->type == TYPE_CELL);
        ret = eval_cell(scope, obj);
        break;
    }

    if (is_stack_trace)
    {
        --debug_depth;
        printf("RET: ");
        print(obj);
        printf(" -> ");
        print(ret);
    }

#ifndef NDEBUG
    POP();
#endif
    return ret;
}

bool compile_function(Object* scope, Object* name, Object* self, Object* params, Object* body)
{
    bool ok = true;

    if (body->type == TYPE_CELL && body->car->type == TYPE_SYMBOL)
    {
        bool is_param = false;

        for (Object* p = params; p->type == TYPE_CELL; p = p->cdr)
        {
            if (p->car == body->car)
            {
                is_param = true;
                break;
            }
        }

        if (is_param)
        {
            debug("Symbol '%s' is a parameter of the function, not a builtin function", body->car->name);
        }
        else if (body->car == name)
        {
            debug("Symbol '%s' points to the function itself, resolving immediately", body->car->name);
            body->car = self;
        }
        else
        {
            Object* val = symbol_lookup(scope, body->car);

            if (!val)
            {
                error("Undefined symbol: %s", body->car->name);
                ok = false;
            }
            else if (val->type == TYPE_BUILTIN || val->type == TYPE_FUNCTION || val->type == TYPE_MACRO)
            {
                debug("Symbol '%s' is a special form, function or macro.", body->car->name);
                Object* global_val = symbol_lookup(Env, body->car);

                if (val != global_val)
                {
                    debug("Symbol '%s' does not come from the global scope.", body->car->name);
                }
                else
                {
                    debug("Symbol '%s' is from the global scope, resolving immediately.", body->car->name);
                    body->car = val;
                }
            }
        }

        for (body = body->cdr; body->type == TYPE_CELL; body = body->cdr)
        {
            if (body->car->type == TYPE_CELL)
            {
                if (!compile_function(scope, name, self, params, body->car))
                {
                    ok = false;
                }
            }
        }
    }

    return ok;
}

// Builtin operators

Object* builtin_add(Object* scope, Object* args)
{
    if (args == Nil)
    {
        error("Not enough arguments to '+'.");
        return Nil;
    }

    PUSH2(scope, args);
    int64_t sum = 0;

    for (; args != Nil; args = args->cdr)
    {
        Object* o = eval(scope, args->car);

        if (o->type != TYPE_NUMBER)
        {
            error("Not a number");
            POP();
            return Nil;
        }

        sum += o->number;
    }

    POP();
    return make_number(sum);
}

Object* builtin_sub(Object* scope, Object* args)
{
    if (args == Nil)
    {
        error("Not enough arguments to '-'.");
        return Nil;
    }

    PUSH2(scope, args);
    int64_t sum = 0;

    Object* o = eval(scope, args->car);

    if (o->type != TYPE_NUMBER)
    {
        error("Not a number");
        return Nil;
    }

    sum = o->number;
    args = args->cdr;

    if (args == Nil)
    {
        sum = -sum;
    }
    else
    {
        for (; args != Nil; args = args->cdr)
        {
            o = eval(scope, args->car);

            if (o->type != TYPE_NUMBER)
            {
                error("Not a number");
                return Nil;
            }

            sum -= o->number;
        }
    }

    POP();
    return make_number(sum);
}

Object* builtin_less(Object* scope, Object* args)
{
    if (CHECK2ARGS(args))
    {
        error("< expects exactly two arguments");
        return Nil;
    }

    Object* lhs = Nil;
    Object* rhs = Nil;
    PUSH4(scope, args, lhs, rhs);

    lhs = eval(scope, args->car);
    rhs = eval(scope, args->cdr->car);
    Object* ret = Nil;

    if (lhs->type == TYPE_NUMBER && rhs->type == TYPE_NUMBER && lhs->number < rhs->number)
    {
        ret = True;
    }

    POP();
    return ret;
}

Object* builtin_quote(Object*, Object* args)
{
    if (CHECK1ARGS(args))
    {
        error("Quote takes exactly one argument");
        return Nil;
    }

    return args->car;
}

Object* builtin_list(Object* scope, Object* args)
{
    Object* ret = Nil;
    Object* argret = Nil;
    PUSH4(scope, args, ret, argret);

    while (args != Nil)
    {
        argret = eval(scope, args->car);
        ret = cons(argret, ret);
        args = args->cdr;
    }

    POP();
    return reverse(ret);
}

Object* builtin_eval(Object* scope, Object* args)
{
    if (CHECK1ARGS(args))
    {
        error("eval takes exactly one argument");
        return Nil;
    }

    PUSH1(scope);

    Object* ret = eval(scope, eval(scope, args->car));

    POP();
    return ret;
}

Object* builtin_apply(Object* scope, Object* args)
{
    if (CHECK2ARGS(args))
    {
        error("apply takes exactly two arguments");
        return Nil;
    }

    Object* func = Nil;
    Object* func_args = Nil;
    PUSH4(scope, args, func, func_args);

    func = eval(scope, args->car);
    func_args = eval(scope, args->cdr->car);

    if (func_args != Nil && func_args->type != TYPE_CELL)
    {
        error("Arguments for apply are not a list");
        POP();
        return Nil;
    }

    Object* ret = cons(func, func_args);
    ret = eval(scope, ret);

    POP();
    return ret;
}

Object* builtin_print(Object* scope, Object* args)
{
    PUSH2(scope, args);

    while (args != Nil)
    {
        print(eval(scope, args->car));
        args = args->cdr;
    }

    POP();
    return Nil;
}

Object* builtin_writechar(Object* scope, Object* args)
{
    if (CHECK1ARGS(args))
    {
        error("'write-char' takes exactly one argument.");
    }
    else
    {
        Object* obj = eval(scope, args->car);

        if (obj->type == TYPE_NUMBER)
        {
            unsigned char ch = obj->number;
            fwrite(&ch, 1, 1, stdout);
        }
        else if (obj->type == TYPE_SYMBOL)
        {
            fwrite(obj->name, strlen(obj->name), 1, stdout);
        }
        else
        {
            error("'write-char' takes a symbol or a number as its argument.");
            print(obj);
        }
    }

    return Nil;
}

Object* builtin_rand(Object*, Object*)
{
    return make_number(rand());
}

Object* builtin_cons(Object* scope, Object* args)
{
    if (CHECK2ARGS(args))
    {
        error("cons takes exactly two arguments");
        return Nil;
    }

    Object* car = Nil;
    Object* cdr = Nil;
    PUSH4(scope, args, car, cdr);

    car = eval(scope, args->car);
    cdr = eval(scope, args->cdr->car);
    Object* ret = cons(car, cdr);

    POP();
    return ret;
}

Object* builtin_car(Object* scope, Object* args)
{
    if (CHECK1ARGS(args))
    {
        error("car takes a list as its argument");
        return Nil;
    }

    args = eval(scope, args->car);

    if (args->type != TYPE_CELL)
    {
        error("Evaluation did not produce a list");
        return Nil;
    }

    return args->car;
}

Object* builtin_cdr(Object* scope, Object* args)
{
    if (CHECK1ARGS(args))
    {
        error("cdr takes a list as its argument");
        return Nil;
    }

    args = eval(scope, args->car);

    if (args->type != TYPE_CELL)
    {
        error("Evaluation did not produce a list");
        return Nil;
    }

    return args->cdr;
}

Object* builtin_eq(Object* scope, Object* args)
{
    if (CHECK2ARGS(args))
    {
        error("= takes exactly two arguments");
        return Nil;
    }

    PUSH2(scope, args);
    Object* lhs = eval(scope, args->car);
    Object* rhs = eval(scope, args->cdr->car);

    if (lhs->type != rhs->type)
    {
        POP();
        return Nil;
    }

    Object* ret = Nil;

    switch (lhs->type)
    {
    case TYPE_CONST:
        ret = True;
        break;

    case TYPE_CELL:
    case TYPE_BUILTIN:
    case TYPE_FUNCTION:
    case TYPE_MACRO:
        ret = Nil;
        break;

    case TYPE_NUMBER:
        ret = lhs->number == rhs->number ? True : Nil;
        break;

    case TYPE_SYMBOL:
        ret = strcmp(lhs->name, rhs->name) == 0 ? True : Nil;
        break;

    default:
        assert(!true);
        break;
    }

    if (is_debug)
    {
        printf("Equals: ");
        print(ret);
    }

    POP();
    return ret;
}

Object* builtin_if(Object* scope, Object* args)
{
    if (CHECK3ARGS(args))
    {
        error("if takes exactly three arguments");
        return Nil;
    }

    PUSH2(scope, args);
    Object* cond = eval(scope, args->car);
    Object* res = cond != Nil ? args->cdr->car : args->cdr->cdr->car;

    if (is_debug)
    {
        printf("Condition ");
        print_one(args->car);
        printf(" evaluates to ");
        print(cond);

        printf("Evaluating ");
        print(res);
    }

    TailCall->tail_expr = res;
    TailCall->tail_scope = scope;
    res = TailCall;

    POP();
    return res;
}

Object* builtin_progn(Object* scope, Object* args)
{
    Object* ret = Nil;
    PUSH2(scope, args);

    for (; args != Nil && args->cdr != Nil; args = args->cdr)
    {
         ret = eval(scope, args->car);
    }

    POP();

    if (args != Nil)
    {
        assert(args->cdr == Nil);
        TailCall->tail_expr = args->car;
        TailCall->tail_scope = scope;
        ret = TailCall;
    }

    return ret;
}

Object* builtin_exit(Object*, Object*)
{
    is_running = false;
    return Nil;
}

Object* builtin_debug(Object* scope, Object* args)
{
    if (CHECK1ARGS(args))
    {
        error("debug takes exactly one argument");
        return Nil;
    }

    Object* ret = eval(scope, args->car);
#ifndef NDEBUG
    is_debug = ret != Nil;
#else
    (void)ret;
    error("debug is not usable in release mode");
#endif
    return Nil;
}

Object* builtin_lambda(Object* scope, Object* args)
{
    if (CHECK2ARGS(args))
    {
        error("lambda takes exactly two arguments");
        print(args);
        return Nil;
    }

    Object* params = args->car;
    Object* body = args->cdr->car;

    return make_function(params, body, scope);
}

Object* builtin_define(Object* scope, Object* args)
{
    if (CHECK2ARGS(args))
    {
        error("define takes exactly two arguments");
        return Nil;
    }

    Object* name = args->car;

    if (name->type != TYPE_SYMBOL)
    {
        error("First argument is not a symbol");
        return Nil;
    }

    Object* value = Nil;
    PUSH4(scope, args, name, value);

    value = eval(scope, args->cdr->car);
    bind_value(scope, name, value);
    return name;
}

Object* builtin_defun(Object* scope, Object* args)
{
    if (CHECK3ARGS(args))
    {
        error("defun takes exactly three arguments");
        return Nil;
    }

    Object* name = args->car;
    Object* params = args->cdr->car;
    Object* body = args->cdr->cdr->car;
    Object* func = Nil;
    PUSH5(scope, name, params, body, func);

    func = make_function(params, body, scope);
    bind_value(scope, name, func);
    POP();
    return func;
}

Object* builtin_compile(Object* scope, Object* args)
{
    for (; args->type == TYPE_CELL; args = args->cdr)
    {
        if (args->car->type != TYPE_SYMBOL)
        {
            error("Argument is not a symbol");
        }
        else
        {
            Object* name = args->car;
            Object* func = symbol_lookup(scope, name);

            if (!func)
            {
                error("Undefined symbol: %s", name->name);
            }
            else if (func->type != TYPE_FUNCTION)
            {
                error("Symbol '%s' does not point to a function", name->name);
            }
            else
            {
                // Resolve all of the symbols in the function body that point to known
                // functions or macros with the final value. This removes the need for a
                // symbol lookup during the execution of the function.
                if (!compile_function(scope, name, func, func->func_params, func->func_body))
                {
                    error("Compilation failed");
                }
                else
                {
                    func->compiled = true;
                }
            }
        }
    }

    return Nil;
}

Object* builtin_defmacro(Object* scope, Object* args)
{
    if (CHECK3ARGS(args))
    {
        error("defmacro takes exactly three arguments");
        return Nil;
    }

    Object* func = builtin_defun(scope, args);
    func->type = TYPE_MACRO;
    return func;
}

Object* builtin_macroexpand(Object* scope, Object* args)
{
    if (CHECK2ARGS(args))
    {
        error("macroexpand takes exactly two arguments");
        return Nil;
    }

    if (args->car->type != TYPE_SYMBOL)
    {
        error("First argument is not a symbol");
        return Nil;
    }

    PUSH2(scope, args);
    Object* macro = eval(scope, args->car);
    Object* ret = Nil;

    if (macro->type != TYPE_MACRO)
    {
        error("%s is not a macro", args->name);
    }
    else
    {
        ret = expand_macro(scope, macro, args->cdr->car);
    }

    POP();
    return ret;
}

Object* builtin_load(Object* scope, Object* args)
{
    if (CHECK1ARGS(args))
    {
        error("load takes exactly one argument");
        return Nil;
    }

    if (args->car->type != TYPE_SYMBOL)
    {
        error("First argument is not a symbol");
        return Nil;
    }

    FILE* f = fopen(args->car->name, "r");

    if (!f)
    {
        error("Failed to open file: %d, %s", errno, strerror(errno));
        return Nil;
    }

    input = f;
    Object* expr = Nil;
    Object* ret = Nil;
    PUSH3(scope, expr, ret);

    while (peek() != EOF)
    {
        expr = parse_expr();

        if (expr)
        {
            ret = eval(scope, expr);

            if (!quiet)
            {
                print(ret);
            }
        }
        else
        {
            break;
        }
    }

    fclose(f);
    input = stdin;

    POP();
    return Nil;
}

// The program itself

void define_builtins()
{
    Env = new_scope(Nil);

    // Minimal builtins
    bind_value(Env, symbol("nil"), Nil);
    bind_value(Env, symbol("t"), True);
    define_builtin_function("+", builtin_add);
    define_builtin_function("-", builtin_sub);
    define_builtin_function("<", builtin_less);
    define_builtin_function("quote", builtin_quote);
    define_builtin_function("cons", builtin_cons);
    define_builtin_function("car", builtin_car);
    define_builtin_function("cdr", builtin_cdr);
    define_builtin_function("eq", builtin_eq);
    define_builtin_function("if", builtin_if);
    define_builtin_function("list", builtin_list);
    define_builtin_function("eval", builtin_eval);
    define_builtin_function("progn", builtin_progn);
    define_builtin_function("lambda", builtin_lambda);
    define_builtin_function("define", builtin_define);
    define_builtin_function("defun", builtin_defun);
    define_builtin_function("compile", builtin_compile);
    define_builtin_function("defmacro", builtin_defmacro);
    define_builtin_function("macroexpand", builtin_macroexpand);

    // Exta builtins
    define_builtin_function("apply", builtin_apply);

    // Debug, OS, etc.
    define_builtin_function("print", builtin_print);
    define_builtin_function("write-char", builtin_writechar);
    define_builtin_function("rand", builtin_rand);
    define_builtin_function("load", builtin_load);
    define_builtin_function("exit", builtin_exit);
    define_builtin_function("debug", builtin_debug);

    // Some common aliases
    define_alias("define", "defvar");
}

void parse()
{
    if (is_debug)
    {
        debug_step = 0;
    }

    if (!quiet)
    {
        printf("> ");
        fflush(stdout);
    }

    Object* obj = parse_expr();

    if (echo)
    {
        printf("\n");
    }

    if (obj)
    {
        if (is_debug)
        {
            printf("======================================================================\n");
        }

        obj = eval(Env, obj);

        if (!quiet)
        {
            print(obj);
        }
        else if (error_ptr)
        {
            --error_ptr;

            for (int i = 16; i >= 0; i--)
            {
                int slot = error_ptr - i;

                if (slot >= 0)
                {
                    printf("Error: %s\n", error_stack[slot % 16]);
                }
            }

            error_ptr = 0;
        }
    }
    else if (peek() == EOF)
    {
        printf("\n");
        is_running = false;
    }
    else
    {
        printf("Malformed input\n");
    }
}

int main(int argc, char** argv)
{
    int ch;
    input = stdin;
    srand(time(NULL));

    while ((ch = getopt(argc, argv, "dgesxm:q")) != -1)
    {
        switch (ch)
        {
        case 'm':
            memory_pct = atof(optarg);
            break;

        case 'e':
            echo = true;
            break;

        case 'g':
            verbose_gc = true;
            break;

#ifndef NDEBUG
        case 's':
            is_stack_trace = true;
            break;

        case 'd':
            is_stack_trace = true;
            is_debug = true;
            break;
#endif
        case 'q':
            quiet = true;
            break;

        default:
            printf("Unknown option: %c\n", ch);
            return 1;
        }
    }

    if (memory_pct > 99.0)
    {
        memory_pct = 99.0;
    }
    else if (memory_pct < 1.0)
    {
        memory_pct = 1.0;
    }

    memory_size = ((memory_size + ALLOC_ALIGN - 1) / ALLOC_ALIGN) * ALLOC_ALIGN;
    mem_root = aligned_alloc(ALLOC_ALIGN, memory_size);
    mem_ptr = mem_root;
    mem_end = mem_root + memory_size / 2;

    define_builtins();

    while (is_running)
    {
        parse();
    }

    free(mem_root);
}
