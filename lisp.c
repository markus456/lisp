#include "lisp.h"
#include "compiler.h"


#define MAX_SYMBOL_LEN 1024

#define ALWAYS_GC 0

Frame* stack_top = NULL;

int get_type(Object* obj)
{
    uint8_t type = (intptr_t)obj & TYPE_MASK;
    return (type & 0x3) == 0 ? TYPE_NUMBER : type;
}

Object* get_obj(Object* obj)
{
    intptr_t p = (intptr_t)obj;
    return (Object*)(p & ~TYPE_MASK);
}

Object* get_cell(Object* obj)
{
    assert(get_type(obj) == TYPE_CELL);
    intptr_t p = (intptr_t)obj;
    return (Object*)(p - TYPE_CELL);
}

Object* get_func(Object* obj)
{
    assert(get_type(obj) == TYPE_FUNCTION);
    intptr_t p = (intptr_t)obj;
    return (Object*)(p - TYPE_FUNCTION);
}

Object* get_builtin(Object* obj)
{
    assert(get_type(obj) == TYPE_BUILTIN);
    intptr_t p = (intptr_t)obj;
    return (Object*)(p - TYPE_BUILTIN);
}

int get_stored_type(Object* obj)
{
    return (intptr_t)get_obj(obj)->moved & TYPE_MASK;
}

const char* get_symbol(Object* obj)
{
    return get_obj(obj)->name;
}

Object* make_ptr(Object* obj, enum Type type)
{
    intptr_t p = (intptr_t)obj;
    return (Object*)(p | type);
}

// The special return value from some of the functions that indicates that the
// return value should be evaluated in the same stack frame.
Object TailCall_obj = {0};
#define TailCall (&TailCall_obj)

Object* AllSymbols = Nil;
Object* Env = Nil;

//
// Globals
//

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

void print(Object* obj);
void print_one(Object* obj);

char error_stack[16][128];
uint8_t error_ptr = 0;

bool no_error()
{
    return error_ptr == 0;
}

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

bool debug_on()
{
    return is_debug;
}

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

size_t type_size(enum Type type)
{
    switch (type)
    {
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
    case TYPE_NUMBER:
    case TYPE_CONST:
    default:
        assert(false);
        return 0;
    }
}

size_t object_size(Object* obj)
{
    int type = get_stored_type(obj);

    if (type == TYPE_SYMBOL)
    {
        return allocation_size(BASE_SIZE + strlen(get_symbol(obj)) + 1);
    }

    return type_size(type);
}

// Garbage collection

Object* make_living(Object* obj)
{
    int type = get_type(obj);

    if (type == TYPE_CONST || type == TYPE_NUMBER)
    {
        // Constant type
        return obj;
    }

    Object* ptr = get_obj(obj);

    // The moved pointer is set to the "moved to" address which has 8 byte
    // alignment. If any of the lowest bits are set, the object has not yet been
    // moved. In this case get_stored_type will return a non-zero value.
    if (get_stored_type(ptr))
    {
        size_t size = object_size(ptr);
        assert(mem_ptr + size <= mem_end);
        memcpy(mem_ptr, ptr, size);
        assert(((intptr_t)mem_ptr & TYPE_MASK) == 0);
        assert(((intptr_t)((Object*)mem_ptr)->moved & TYPE_MASK) == type);
        ptr->moved = (Object*)mem_ptr;
        mem_ptr += size;
    }

    assert(ptr->moved);
    return make_ptr(ptr->moved, type);
}

void fix_references(Object* obj)
{
    // The type information is also included in the object header. Otherwise it
    // would not be possible to know the type of the object when the heap memory is
    // scanned during garbage collection.
    assert(get_obj(obj) == obj);
    int type = get_stored_type(obj);

    switch (type)
    {
    case TYPE_SYMBOL:
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

    case TYPE_NUMBER:
    case TYPE_CONST:
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
        for (int i = 0; i < f->size; i++)
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

    assert(get_obj(rv) == rv);
    return rv;
}

Object* cons(Object* car, Object* cdr)
{
    PUSH2(car, cdr);
    Object* rv = allocate(type_size(TYPE_CELL));
    rv->moved = (Object*)TYPE_CELL;
    rv->car = car;
    rv->cdr = cdr;
    POP();
    return make_ptr(rv, TYPE_CELL);
}

Object* car(Object* obj)
{
    assert(get_type(obj) == TYPE_CELL);
    return get_cell(obj)->car;
}

Object* cdr(Object* obj)
{
    assert(get_type(obj) == TYPE_CELL);
    return get_cell(obj)->cdr;
}

Object* make_number(int64_t val)
{
    return (Object*)((uint64_t)val << 2);
}

int64_t get_number(Object* obj)
{
    assert(get_type(obj) == TYPE_NUMBER);
    int64_t val = (int64_t)obj;
    return val >> 2;
}

Object* make_symbol(const char* name)
{
    size_t sz = allocation_size(BASE_SIZE + strlen(name) + 1);
    Object* rv = allocate(sz);
    rv->moved = (Object*)TYPE_SYMBOL;
    strcpy(rv->name, name);
    return make_ptr(rv, TYPE_SYMBOL);
}

Object* make_builtin(Function func)
{
    Object* rv = allocate(type_size(TYPE_BUILTIN));
    rv->moved = (Object*)TYPE_BUILTIN;
    rv->fn = func;
    return make_ptr(rv, TYPE_BUILTIN);
}

Object* make_function(Object* params, Object* body, Object* env)
{
    PUSH3(params, body, env);
    Object* rv = allocate(type_size(TYPE_FUNCTION));
    rv->moved = (Object*)TYPE_FUNCTION;
    rv->func_params = params;
    rv->func_body = body;
    rv->func_env = env;
    rv->compiled = 0;
    POP();
    return make_ptr(rv, TYPE_FUNCTION);
}

Object* func_body(Object* obj)
{
    assert(get_type(obj) == TYPE_FUNCTION || get_type(obj) == TYPE_MACRO);
    return get_func(obj)->func_body;
}

Object* func_params(Object* obj)
{
    assert(get_type(obj) == TYPE_FUNCTION || get_type(obj) == TYPE_MACRO);
    return get_func(obj)->func_params;
}

Object* func_env(Object* obj)
{
    assert(get_type(obj) == TYPE_FUNCTION || get_type(obj) == TYPE_MACRO);
    return get_func(obj)->func_env;
}

Object* new_scope(Object* prev_scope)
{
    return cons(Nil, prev_scope);
}

Object* symbol(const char* name)
{
    for (Object* o = AllSymbols; o != Nil; o = cdr(o))
    {
        Object* val = car(o);

        if (strcmp(get_symbol(val), name) == 0)
        {
            return val;
        }
    }

    Object* sym = make_symbol(name);
    AllSymbols = cons(sym, AllSymbols);
    return car(AllSymbols);
}

void bind_value(Object* scope, Object* symbol, Object* value)
{
    if (is_debug)
    {
        printf("Binding '%s' to ", get_symbol(symbol));
        print(value);
    }

    Object* bound = Nil;
    PUSH4(scope, symbol, value, bound);
    bound = cons(symbol, value);
    Object* res = cons(bound, car(scope));
    get_cell(scope)->car = res;
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

    for (Object* s = scope; s != Nil; s = cdr(s))
    {
        ++num_scopes;
    }

    for (Object* s = scope; s != Nil; s = cdr(s))
    {
        printf("===== Scope %d =====\n", num_scopes--);
        print(car(s));
    }
}

Object* symbol_lookup(Object* scope, Object* sym)
{
    for (Object* s = scope; s != Nil; s = cdr(s))
    {
        for (Object* o = car(s); o != Nil; o = cdr(o))
        {
            Object* kv = car(o);
            assert(get_type(kv) == TYPE_CELL);
            Object* key = car(kv);

            if (key == sym)
            {
                if (is_debug)
                {
                    printf("Symbol '%s' points to ", get_symbol(key));
                    print(cdr(kv));
                }

                return cdr(kv);
            }
        }
    }

    return Undefined;
}

void print_one(Object* obj)
{
    assert(obj != TailCall);

    switch (get_type(obj))
    {
    case TYPE_NUMBER:
        printf("%ld ", get_number(obj));
        break;
    case TYPE_SYMBOL:
        printf("%s ", get_symbol(obj));
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

            for (; get_type(o) == TYPE_CELL; o = cdr(o))
            {
                print_one(car(o));
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
        if (get_func(obj)->compiled)
        {
            printf("<compiled func> ");
        }
        else
        {
            printf("<func> ");
            if (is_debug)
            {
                print_one(func_params(obj));
                print_one(func_body(obj));
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

void debug_print(Object* obj)
{
    if (is_debug)
    {
        print_one(obj);
        printf("\n");
    }
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
        Object* next = cdr(list);
        get_cell(list)->cdr = newlist;
        newlist = list;
        list = next;
    }

    return newlist;
}

int length(Object* list)
{
    int i = 0;

    for (; list != Nil; list = cdr(list))
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

    while (obj != Undefined)
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

        if (val >= LONG_MAX >> 2)
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
            return Undefined;
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

                    if (o != Nil)
                    {
                        int64_t val = get_number(o);
                        o = make_number(-val);
                    }
                }
                else if (isspace(peek()))
                {
                    o = symbol("-");
                }
                else
                {
                    o = parse_symbol();
                    char* name = (char*)get_symbol(o);
                    memmove(name + 1, name, strlen(name));
                    name[0] = '-';
                }

                return o;
            }

        case '\'':
            return parse_quote();

        case ')':
            get();
            return Undefined;

        case EOF:
            return Undefined;

        default:
            return parse_symbol();
        };
    }

    return Undefined;
}
// Evaluation

Object* expand_macro(Object* scope, Object* macro, Object* args)
{
    Object* param = func_params(macro);
    PUSH4(macro, param, args, scope);
    scope = new_scope(scope);

    while (param != Nil && args != Nil)
    {
        assert(get_type(param) == TYPE_CELL);

        if (get_type(args) != TYPE_CELL)
        {
            break;
        }

        bind_value(scope, car(param), car(args));
        param = cdr(param);
        args = cdr(args);
    }

    Object* ret = Nil;

    if (args != Nil)
    {
        if (get_type(args) == TYPE_CELL)
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
        ret = eval(scope, func_body(macro));
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

    fn = eval(scope, car(obj));
    int type = get_type(fn);

    if (type == TYPE_MACRO)
    {
        ret = expand_macro(scope, fn, cdr(obj));
        ret = eval(scope, ret);
    }
    else if (type == TYPE_BUILTIN)
    {
        ret = get_builtin(fn)->fn(scope, cdr(obj));
    }
    else if (type == TYPE_FUNCTION)
    {
        next_scope = new_scope(func_env(fn));
        param = func_params(fn);
        arg = cdr(obj);
        assert(param == Nil || get_type(param) == TYPE_CELL);
        assert(arg == Nil || get_type(arg) == TYPE_CELL);

        while (param != Nil && arg != Nil)
        {
            ret = eval(scope, car(arg));
            bind_value(next_scope, car(param), ret);
            param = cdr(param);
            arg = cdr(arg);
        }

        if (param != Nil)
        {
            Object* sym = car(obj);
            error("Not enough arguments to function '%s'. Expected %d, have %d.",
                  get_type(sym) == TYPE_SYMBOL ? get_symbol(sym) : "<func>",
                  length(func_params(fn)), length(cdr(obj)));
        }
        else if (arg != Nil)
        {
            Object* sym = car(obj);
            error("Too many arguments to function '%s'. Expected %d, have %d.",
                  get_type(sym) == TYPE_SYMBOL ? get_symbol(sym) : "<func>",
                  length(func_params(fn)), length(cdr(obj)));
        }
        else if (get_func(fn)->compiled == COMPILE_CODE)
        {
            // The arguments to the function are bound to the newest scope.
            ret = jit_eval(fn, car(next_scope));
        }
        else
        {
            Object* body = func_body(fn);

            if (get_type(body) == TYPE_CELL)
            {
                debug("Function body is a list, evaluating in the same frame:");
                obj = body;
                scope = next_scope;
                goto start;
            }

            ret = eval(next_scope, body);
        }

        if (is_debug)
        {
            printf("Return from: ");
            print(func_body(fn));
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

        if (get_type(obj) == TYPE_CELL)
        {
            if (is_stack_trace)
            {
                printf("Doing tail call: ");
                print(obj);
                printf(":::::::::::: DO TAIL :::::::::::::::::\n");
                print(car(scope));
            }

            goto start;
        }

        if (is_stack_trace)
        {
            printf("NOT doing tail call: ");
            print(obj);
            printf(":::::::::::: DO NOT TAIL :::::::::::::::::\n");
            print(car(scope));
        }

        // Not a list, evalue it here
        ret = eval(scope, obj);
    }

    POP();
    return ret;
}

Object* eval(Object* scope, Object* obj)
{
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

    switch (get_type(obj))
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

        if (ret == Undefined)
        {
            ret = Nil;
            error("Undefined symbol: %s", get_symbol(obj));

            if (is_debug)
            {
                print_scope(scope);
            }

        }
        break;

    default:
        assert(get_type(obj) == TYPE_CELL);
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

    for (; args != Nil; args = cdr(args))
    {
        Object* o = eval(scope, car(args));

        if (get_type(o) != TYPE_NUMBER)
        {
            error("Not a number");
            POP();
            return Nil;
        }

        sum += get_number(o);
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

    Object* o = eval(scope, car(args));

    if (get_type(o) != TYPE_NUMBER)
    {
        error("Not a number");
        return Nil;
    }

    sum = get_number(o);
    args = cdr(args);

    if (args == Nil)
    {
        sum = -sum;
    }
    else
    {
        for (; args != Nil; args = cdr(args))
        {
            o = eval(scope, car(args));

            if (get_type(o) != TYPE_NUMBER)
            {
                error("Not a number");
                return Nil;
            }

            sum -= get_number(o);
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

    lhs = eval(scope, car(args));
    rhs = eval(scope, car(cdr(args)));

    POP();
    return get_number(lhs) < get_number(rhs) ? True : Nil;

}

Object* builtin_quote(Object*, Object* args)
{
    if (CHECK1ARGS(args))
    {
        error("Quote takes exactly one argument");
        return Nil;
    }

    return car(args);
}

Object* builtin_list(Object* scope, Object* args)
{
    Object* ret = Nil;
    Object* argret = Nil;
    PUSH4(scope, args, ret, argret);

    while (args != Nil)
    {
        argret = eval(scope, car(args));
        ret = cons(argret, ret);
        args = cdr(args);
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

    Object* ret = eval(scope, eval(scope, car(args)));

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

    func = eval(scope, car(args));
    func_args = eval(scope, car(cdr(args)));

    if (func_args != Nil && get_type(func_args) != TYPE_CELL)
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
        print(eval(scope, car(args)));
        args = cdr(args);
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
        Object* obj = eval(scope, car(args));

        if (get_type(obj) == TYPE_NUMBER)
        {
            unsigned char ch = get_number(obj);
            fwrite(&ch, 1, 1, stdout);
        }
        else if (get_type(obj) == TYPE_SYMBOL)
        {
            const char* sym = get_symbol(obj);
            fwrite(sym, strlen(sym), 1, stdout);
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

    Object* car_obj = Nil;
    Object* cdr_obj = Nil;
    PUSH4(scope, args, car_obj, cdr_obj);

    car_obj = eval(scope, car(args));
    cdr_obj = eval(scope, car(cdr(args)));
    Object* ret = cons(car_obj, cdr_obj);

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

    args = eval(scope, car(args));

    if (get_type(args) != TYPE_CELL)
    {
        error("Evaluation did not produce a list");
        return Nil;
    }

    return car(args);
}

Object* builtin_cdr(Object* scope, Object* args)
{
    if (CHECK1ARGS(args))
    {
        error("cdr takes a list as its argument");
        return Nil;
    }

    args = eval(scope, car(args));

    if (get_type(args) != TYPE_CELL)
    {
        error("Evaluation did not produce a list");
        return Nil;
    }

    return cdr(args);
}

Object* builtin_eq(Object* scope, Object* args)
{
    if (CHECK2ARGS(args))
    {
        error("= takes exactly two arguments");
        return Nil;
    }

    PUSH2(scope, args);
    Object* lhs = eval(scope, car(args));
    Object* rhs = eval(scope, car(cdr(args)));
    POP();

    // All types except cons cells can be compared for equality by comparing the
    // pointers. For cons cells, this function returns true if the two values
    // point to the same cons cell which is not what equality means for e.g. two
    // lists. Comparing lists must be done using other means.
    debug("Equals: %s", lhs == rhs ? "t" : "nil");
    return lhs == rhs ? True : Nil;
}

Object* builtin_if(Object* scope, Object* args)
{
    if (CHECK3ARGS(args))
    {
        error("if takes exactly three arguments");
        return Nil;
    }

    PUSH2(scope, args);
    Object* cond = eval(scope, car(args));
    Object* res = cond != Nil ? car(cdr(args)) : car(cdr(cdr(args)));

    if (is_debug)
    {
        printf("Condition ");
        print_one(car(args));
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

    for (; args != Nil && cdr(args) != Nil; args = cdr(args))
    {
         ret = eval(scope, car(args));
    }

    POP();

    if (args != Nil)
    {
        assert(cdr(args) == Nil);
        TailCall->tail_expr = car(args);
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

    Object* ret = eval(scope, car(args));
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

    Object* params = car(args);
    Object* body = car(cdr(args));

    return make_function(params, body, scope);
}

Object* builtin_define(Object* scope, Object* args)
{
    if (CHECK2ARGS(args))
    {
        error("define takes exactly two arguments");
        return Nil;
    }

    Object* name = car(args);

    if (get_type(name) != TYPE_SYMBOL)
    {
        error("First argument is not a symbol");
        return Nil;
    }

    Object* value = Nil;
    PUSH4(scope, args, name, value);

    value = eval(scope, car(cdr(args)));
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

    Object* name = car(args);
    Object* params = car(cdr(args));
    Object* body = car(cdr(cdr(args)));
    Object* func = Nil;
    PUSH5(scope, name, params, body, func);

    func = make_function(params, body, scope);
    bind_value(scope, name, func);
    POP();
    return func;
}

Object* builtin_freeze(Object* scope, Object* args)
{
    jit_resolve_symbols(scope, args);
    return Nil;
}

Object* builtin_compile(Object* scope, Object* args)
{
    jit_compile(scope, args);
    return Nil;
}

Object* builtin_defmacro(Object* scope, Object* args)
{
    if (CHECK3ARGS(args))
    {
        error("defmacro takes exactly three arguments");
        return Nil;
    }

    Object* name = car(args);
    Object* params = car(cdr(args));
    Object* body = car(cdr(cdr(args)));
    Object* func = Nil;
    PUSH5(scope, name, params, body, func);

    func = make_function(params, body, scope);
    func = get_func(func);
    func->moved = (Object*)TYPE_MACRO;
    func = make_ptr(func, TYPE_MACRO);
    bind_value(scope, name, func);
    POP();
    return func;
}

Object* builtin_macroexpand(Object* scope, Object* args)
{
    if (CHECK2ARGS(args))
    {
        error("macroexpand takes exactly two arguments");
        return Nil;
    }

    if (get_type(car(args)) != TYPE_SYMBOL)
    {
        error("First argument is not a symbol");
        return Nil;
    }

    PUSH2(scope, args);
    Object* macro = eval(scope, car(args));
    Object* ret = Nil;

    if (get_type(macro) != TYPE_MACRO)
    {
        error("%s is not a macro", get_symbol(args));
    }
    else
    {
        ret = expand_macro(scope, macro, car(cdr(args)));
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

    if (get_type(car(args)) != TYPE_SYMBOL)
    {
        error("First argument is not a symbol");
        return Nil;
    }

    FILE* f = fopen(get_symbol(car(args)), "r");

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

        if (expr != Undefined)
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
    define_builtin_function("freeze", builtin_freeze);
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

    if (obj != Undefined)
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

        if (error_ptr)
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

    jit_free();
    free(mem_root);
}
