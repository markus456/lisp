#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>

#define MAX_SYMBOL_LEN 55

#define ALWAYS_GC 0

enum ObjectType {TYPE_NIL = 0, TYPE_TRUE = 1, TYPE_NUMBER = 2, TYPE_SYMBOL = 3, TYPE_BUILTIN = 4,
                 TYPE_CELL = 5, TYPE_FUNCTION = 6, TYPE_MACRO = 7, TYPE_MOVED = 8};

struct Object;
typedef struct Object Object;
typedef struct Object* (*Function) (struct Object*, struct Object*);

struct Object
{
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
        char name[MAX_SYMBOL_LEN + 1];

        // Builtin function
        Function fn;

        // Custom functions
        struct {
            Object* func_params;
            Object* func_body;
            Object* func_env;
        };

        Object* moved;
    };

    uint8_t type;
};

#define MAX_VARS 7

struct Frame
{
    struct Frame* next;
    Object** vars[MAX_VARS];
};

typedef struct Frame Frame;
Frame* stack_top = NULL;

#define ENTER() Frame frame = {0}; frame.next = stack_top; stack_top = &frame
#define PUSH1(a) ENTER(); frame.vars[0] = &a;
#define PUSH2(a, b) PUSH1(a);  frame.vars[1] = &b;
#define PUSH3(a, b, c) PUSH2(a, b); frame.vars[2] = &c;
#define PUSH4(a, b, c, d) PUSH3(a, b, c); frame.vars[3] = &d;
#define PUSH5(a, b, c, d, e) PUSH4(a, b, c, d); frame.vars[4] = &e;
#define PUSH6(a, b, c, d, e, f) PUSH5(a, b, c, d, e); frame.vars[5] = &f;
#define PUSH7(a, b, c, d, e, f, g) PUSH6(a, b, c, d, e, f); frame.vars[6] = &g;
#define POP() stack_top = frame.next;

Object Nil_obj = {.type = TYPE_NIL, .number = 0};
#define Nil &Nil_obj

Object True_obj = {.type = TYPE_TRUE, .number = 0};
#define True &True_obj

Object* AllSymbols = Nil;
Object* Env = Nil;

FILE* input;
uint8_t* mem_root;
uint8_t* mem_end;
uint8_t* mem_ptr;
bool is_running = true;
bool is_error = false;
bool echo = false;
bool is_debug = false;
bool verbose_gc = false;
int debug_step = 0;
int debug_depth = 0;
size_t memory_size = 1024 * 1024;

void debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void print(Object* obj);
void print_one(Object* obj);

void error(const char* fmt, ...)
{
    is_error = true;

    va_list args;
    va_start(args, fmt);
    printf("Error: ");
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

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
        assert(mem_ptr + sizeof(Object) <= mem_end);
        memcpy(mem_ptr, obj, sizeof(Object));
        obj->type = TYPE_MOVED;
        obj->moved = (Object*)mem_ptr;
        mem_ptr += sizeof(Object);
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

    case TYPE_NIL:
    case TYPE_TRUE:
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

    if (mem_end == mem_root + space_size)
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
        fix_references((Object*)scan_ptr);
        scan_ptr += sizeof(Object);
    }

    assert(scan_ptr == mem_ptr);

    if (verbose_gc)
    {
        size_t still_in_use = scan_ptr - scan_start;
        size_t memory_freed = memory_used - still_in_use;

        if (memory_freed)
        {
            printf("\nMemory freed: %lu Memory used: %lu\n", memory_freed, still_in_use);
        }
    }
}

// Object creation

Object* allocate()
{
#if ALWAYS_GC
    collect_garbage();
#endif

    if (mem_ptr + sizeof(Object) > mem_end)
    {
        collect_garbage();

        if (mem_ptr + sizeof(Object) > mem_end)
        {
            error("Not enough memory");
            abort();
        }
    }

    Object* rv = (Object*)mem_ptr;
    mem_ptr += sizeof(Object);
    return rv;
}

Object* cons(Object* car, Object* cdr)
{
    PUSH2(car, cdr);
    Object* rv = allocate();
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
    Object* rv = allocate();
    rv->type = TYPE_NUMBER;
    rv->number = val;
    return rv;
}

Object* make_symbol(const char* name)
{
    Object* rv = allocate();
    rv->type = TYPE_SYMBOL;
    strcpy(rv->name, name);
    return rv;
}

Object* make_builtin(Function func)
{
    Object* rv = allocate();
    rv->type = TYPE_BUILTIN;
    rv->fn = func;
    return rv;
}

Object* make_function(Object* params, Object* body, Object* env)
{
    PUSH3(params, body, env);
    Object* rv = allocate();
    rv->type = TYPE_FUNCTION;
    rv->func_params = params;
    rv->func_body = body;
    rv->func_env = env;
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

    for (Object* o = scope->car; o != Nil; o = o->cdr)
    {
        assert(o->type == TYPE_CELL);

        if (o->car->car == symbol)
        {
            o->car->cdr->car = value;
            return;
        }
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

    for (Object* s = scope; s; s = s->cdr)
    {
        ++num_scopes;
    }

    for (Object* s = scope; s; s = s->cdr)
    {
        printf("===== Scope %d =====\n", num_scopes--);
        print(s);
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

    error("Undefined symbol: %s", sym->name);

    if (is_debug)
    {
        print_scope(scope);
    }

    return Nil;
}

void print_one(Object* obj)
{
    assert(obj);

    switch (obj->type)
    {
    case TYPE_NUMBER:
        printf("%ld ", obj->number);
        break;
    case TYPE_SYMBOL:
        printf("%s ", obj->name);
        break;
    case TYPE_NIL:
        printf("nil ");
        break;
    case TYPE_TRUE:
        printf("t ");
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
        printf("<func> ");
        if (is_debug)
        {
            print_one(obj->func_params);
            print_one(obj->func_body);
        }
        break;
    case TYPE_MACRO:
        printf("<macro> ");
        break;
    case TYPE_BUILTIN:
        printf("<builtin> ");
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
    bool negative = ch == '-';

    if (negative)
    {
        ch = get();
    }

    unsigned int val = ch - '0';

    while (isdigit(peek()))
    {
        ch = get();
        val = ch - '0' + val * 10;

        if (val >= INT_MAX)
        {
            error("Integer overflow");
            return Nil;
        }
    }

    int ival = val;
    return make_number(negative ? -ival : ival);
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
        case '-':
            return parse_number();

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
        bind_value(scope, param->car, args->car);
        param = param->cdr;
        args = args->cdr;
    }

    Object* ret = Nil;

    if (param != Nil)
    {
        error("Not enough arguments to macro");
    }
    else if (args != Nil)
    {
        error("Too many arguments to macro");
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
        assert(arg->type == TYPE_CELL);

        while (param != Nil && arg != Nil)
        {
            ret = eval(scope, arg->car);
            bind_value(next_scope, param->car, ret);
            param = param->cdr;
            arg = arg->cdr;
        }

        if (param != Nil)
        {
            error("Not enough arguments to function. Expected %d, have %d.",
                  length(fn->func_params), length(obj->cdr));
        }
        else if (arg != Nil)
        {
            error("Too many arguments to function. Expected %d, have %d.",
                  length(fn->func_params), length(obj->cdr));
        }
        else
        {
            ret = eval(next_scope, fn->func_body);
        }
    }
    else
    {
        error("Not a function");
        print(fn);
    }

    POP();
    return ret;
}

Object* eval(Object* scope, Object* obj)
{
    assert(obj);
    Object* ret = Nil;
    PUSH1(scope);

    if (is_error)
    {
        POP();
        return ret;
    }

    if (is_debug)
    {
        for (int i = 0; i < debug_depth; i++)
        {
            printf(". ");
        }

        printf("EVAL %d: ", debug_step++);
        print(obj);
        ++debug_depth;
    }

    switch (obj->type)
    {
    case TYPE_NUMBER:
    case TYPE_NIL:
    case TYPE_TRUE:
    case TYPE_BUILTIN:
    case TYPE_FUNCTION:
    case TYPE_MACRO:
        ret = obj;
        break;

    case TYPE_SYMBOL:
        ret = symbol_lookup(scope, obj);
        break;

    case TYPE_CELL:
        ret = eval_cell(scope, obj);
        break;

    default:
        assert(!true);
        break;
    }

    if (is_debug)
    {
        --debug_depth;
        printf("Returning: ");
        print(ret);
    }

    POP();
    return ret;
}

// Builtin operators

Object* builtin_add(Object* scope, Object* args)
{
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
    PUSH2(scope, args);
    int64_t sum = 0;

    for (; args != Nil; args = args->cdr)
    {
        Object* o = eval(scope, args->car);

        if (o->type != TYPE_NUMBER)
        {
            error("Not a number");
            return Nil;
        }

        sum -= o->number;
    }

    POP();
    return make_number(sum);
}

Object* builtin_less(Object* scope, Object* args)
{
    if (length(args) != 2)
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
    if (length(args) != 1)
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
    if (length(args) != 2)
    {
        error("apply takes exactly two arguments");
        return Nil;
    }

    Object* func = Nil;
    Object* func_args = Nil;
    PUSH4(scope, args, func, func_args);

    func = eval(scope, args->car);
    func_args = eval(scope, args->cdr->car);

    if (func_args->type != TYPE_CELL)
    {
        error("Argument for apply are not a list");
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

Object* builtin_cons(Object* scope, Object* args)
{
    if (length(args) != 2)
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
    if (length(args) != 1)
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
    if (length(args) != 1)
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
    if (length(args) != 2)
    {
        error("= takes exactly two arguments");
        return Nil;
    }

    Object* lhs = eval(scope, args->car);
    Object* rhs = eval(scope, args->cdr->car);

    if (lhs->type != rhs->type)
    {
        return Nil;
    }

    switch (lhs->type)
    {
    case TYPE_NIL:
    case TYPE_TRUE:
        return True;

    case TYPE_CELL:
    case TYPE_BUILTIN:
    case TYPE_FUNCTION:
    case TYPE_MACRO:
        return Nil;

    case TYPE_NUMBER:
        return lhs->number == rhs->number ? True : Nil;

    case TYPE_SYMBOL:
        return strcmp(lhs->name, rhs->name) == 0 ? True : Nil;
    }

    assert(!true);
    return Nil;
}

Object* builtin_if(Object* scope, Object* args)
{
    if (length(args) != 3)
    {
        error("if takes exactly three arguments");
        return Nil;
    }

    PUSH2(scope, args);

    Object* cond = eval(scope, args->car);

    if (is_debug)
    {
        printf("Condition ");
        print_one(args->car);
        printf(" evaluates to ");
        print(cond);

        printf("Evaluating ");
        print(cond != Nil ? args->cdr->car : args->cdr->cdr->car);
    }

    Object* res = eval(scope, cond != Nil ? args->cdr->car : args->cdr->cdr->car);

    if (is_debug)
    {
        printf("Evaluated to ");
        print(res);
    }

    return res;
}

Object* builtin_progn(Object* scope, Object* args)
{
    Object* ret = Nil;

    for (; args != Nil; args = args->cdr)
    {
         ret = eval(scope, args->car);
    }

    return ret;
}

Object* builtin_exit(Object*, Object*)
{
    is_running = false;
    return Nil;
}

Object* builtin_lambda(Object* scope, Object* args)
{
    if (length(args) != 2)
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
    int n = length(args);

    if (n != 2)
    {
        error("define takes exactly two arguments, have %d", n);
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
    if (length(args) != 3)
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

Object* builtin_defmacro(Object* scope, Object* args)
{
    if (length(args) != 3)
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
    if (length(args) != 2)
    {
        error("macroexpand takes exactly two arguments");
        return Nil;
    }

    if (args->car->type != TYPE_SYMBOL)
    {
        error("First argument is not a symbol");
        return Nil;
    }

    Object* macro = eval(scope, args->car);

    if (macro->type != TYPE_MACRO)
    {
        error("%s is not a macro", args->name);
        return Nil;
    }

    return expand_macro(scope, macro, args->cdr->car);
}

Object* builtin_load(Object* scope, Object* args)
{
    if (length(args) != 1)
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

    while (peek() != EOF)
    {
        Object* expr = parse_expr(scope);

        if (expr)
        {
            print(eval(scope, expr));
        }
        else
        {
            break;
        }
    }

    fclose(f);
    input = stdin;

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
    define_builtin_function("defmacro", builtin_defmacro);
    define_builtin_function("macroexpand", builtin_macroexpand);

    // Exta builtins
    define_builtin_function("apply", builtin_apply);

    // Debug, OS, etc.
    define_builtin_function("print", builtin_print);
    define_builtin_function("load", builtin_load);
    define_builtin_function("exit", builtin_exit);
}

void parse()
{
    if (is_debug)
    {
        debug_step = 0;
    }

    printf("> ");
    fflush(stdout);

    is_error = false;
    Object* obj = parse_expr(Env);

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

        print(eval(Env, obj));
        collect_garbage();
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

    while ((ch  = getopt(argc, argv, "dgexm:")) != -1)
    {
        switch (ch)
        {
        case 'm':
            memory_size = atoi(optarg);
            break;

        case 'e':
            echo = true;
            break;

        case 'g':
            verbose_gc = true;
            break;

        case 'd':
            is_debug = true;
            break;

        default:
            printf("Unknown option: %c\n", ch);
            return 1;
        }
    }

    memory_size = ((memory_size / sizeof(Object)) + 1) * sizeof(Object);
    mem_root = malloc(memory_size);
    mem_ptr = mem_root;
    mem_end = mem_root + memory_size / 2;

    define_builtins();

    while (is_running)
    {
        parse();
    }

    free(mem_root);
}
