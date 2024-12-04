#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <ctype.h>
#include <limits.h>

#define MAX_SYMBOL_LEN 64

//#define DEBUGGING 1

#ifdef DEBUGGING
#define DPRINT(msg) printf("DEBUG: %s\n", msg)
#else
#define DPRINT(msg)
#endif

enum Types {TYPE_NIL, TYPE_TRUE, TYPE_NUMBER, TYPE_CELL, TYPE_SYMBOL, TYPE_BUILTIN, TYPE_FUNCTION};

struct Object;
struct Scope;
typedef struct Object Object;
typedef struct Scope Scope;
typedef struct Object* (*Function) (struct Scope*, struct Object*);

struct Object
{
    int type;

    union
    {
        // Number
        int number;

        // Cons cell
        struct {
            Object* car;
            Object* cdr;
        };

        // Symbol
        char name[32];

        // Builtin function
        Function fn;

        // Custom functions
        struct {
            Object* func_name;
            Object* func_params;
            Object* func_body;
        };
    };
};

struct Scope
{
    Object* symbols;
    struct Scope* next;
};

Object Nil_obj = {.type = TYPE_NIL, .number = 0};
#define Nil &Nil_obj

Object True_obj = {.type = TYPE_TRUE, .number = 0};
#define True &True_obj

Object* AllSymbols = Nil;
Scope Env = {.symbols = Nil, .next = NULL};

void* mem_root;
void* mem_end;
void* mem_ptr;
bool running = true;
bool echo = false;

void error(const char* err)
{
    printf("Error: %s\n", err);
}

// Object creation

Object* allocate()
{
    Object* rv = mem_ptr;

    if (mem_ptr + sizeof(Object)  > mem_end)
    {
        // TODO: Garbage collect
        error("Not enough memory");
        abort();
    }

    mem_ptr += sizeof(Object);
    return rv;
}

Object* cons(Object* car, Object* cdr)
{
    Object* rv = allocate();
    rv->type = TYPE_CELL;
    rv->car = car;
    rv->cdr = cdr;
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

Object* make_number(int val)
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

Object* make_function(Object* name, Object* params, Object* body)
{
    Object* rv = allocate();
    rv->type = TYPE_FUNCTION;
    rv->func_name = name;
    rv->func_params = params;
    rv->func_body = body;
    return rv;
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

    AllSymbols = cons(make_symbol(name), AllSymbols);
    return AllSymbols->car;
}

void bind_value(Scope* scope, Object* symbol, Object* value)
{
    scope->symbols = cons(cons(symbol, value), scope->symbols);
}

void define_builtin_function(const char* name, Function fn)
{
    Object* sym = symbol(name);
    Object* fun = make_builtin(fn);
    bind_value(&Env, sym, fun);
}

Object* symbol_lookup(Scope* scope, Object* sym)
{
    for (Scope* s = scope; s; s = s->next)
    {
        for (Object* o = s->symbols; o != Nil; o = o->cdr)
        {
            assert(o->car->type = TYPE_CELL);

            if (o->car->car == sym)
            {
                return o->car->cdr;
            }
        }
    }

    return Nil;
}

void print_one(Object* obj)
{
    assert(obj);

    switch (obj->type)
    {
    case TYPE_NUMBER:
        printf("%d ", obj->number);
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
        printf("( ");
        print_one(obj->car);
        printf(" . ");
        print_one(obj->cdr);
        printf(" ) ");
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
    int ch = getc(stdin);
    ungetc(ch, stdin);
    return ch;
}

int get()
{
    int rc = getc(stdin);

    if (rc == EOF)
    {
        exit(0);
    }
    else if (echo && rc != '\n' && rc != '\r')
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
    DPRINT("list");
    Object* value = Nil;
    assert(peek() == '(');
    get();

    while (true)
    {
        Object* obj = parse_expr();

        if (!obj)
        {
            return reverse(value);
        }

        value = cons(obj, value);
    }

    return NULL;
}

Object* parse_number()
{
    DPRINT("number");
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
    DPRINT("quote");
    Object* fn = symbol("quote");
    assert(peek() == '\'');
    get();
    return cons(fn, parse_expr());
}

Object* parse_symbol()
{
    DPRINT("symbol");
    char name[MAX_SYMBOL_LEN];
    char* ptr = name;
    char ch = peek();

    while (ch != ')' && !isspace(ch))
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
    DPRINT("expr");

    while (true)
    {
        switch (peek())
        {
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

        default:
            return parse_symbol();
        };
    }

    return NULL;
}

// Evaluation
Object* eval(Scope* scope, Object* obj)
{
    switch (obj->type)
    {
    case TYPE_NUMBER:
    case TYPE_NIL:
    case TYPE_BUILTIN:
    case TYPE_FUNCTION:
        return obj;

    case TYPE_SYMBOL:
        return eval(scope, symbol_lookup(scope, obj));

    case TYPE_CELL:
        {
            Object* fn = eval(scope, obj->car);

            if (fn->type == TYPE_BUILTIN)
            {
                return fn->fn(scope, obj->cdr);
            }
            else if (fn->type == TYPE_FUNCTION)
            {
                Scope new_scope;
                new_scope.next = scope;
                new_scope.symbols = Nil;
                Object* param = fn->func_params;
                Object* arg = obj->cdr;

                while (param != Nil && arg != Nil)
                {
                    bind_value(&new_scope, param->car, eval(scope, arg->car));
                    param = param->cdr;
                    arg = arg->cdr;
                }

                if (param != Nil)
                {
                    error("Not enough arguments to function");
                    return Nil;
                }
                else if (arg != Nil)
                {
                    error("Too many arguments to function");
                    return Nil;
                }
                else
                {
                    return eval(&new_scope, fn->func_body);
                }
            }
            else
            {
                error("Not a function");
                return Nil;
            }
        }
    }

    assert(!true);
    return Nil;
}

// Builtin operators

Object* builtin_add(Scope* scope, Object* args)
{
    int sum = 0;

    for (; args != Nil; args = args->cdr)
    {
        Object* o = eval(scope, args->car);

        if (o->type != TYPE_NUMBER)
        {
            error("Not a number");
            return Nil;
        }

        sum += o->number;
    }

    return make_number(sum);
}

Object* builtin_quote(Scope*, Object* args)
{
    return args;
}

Object* builtin_eval(Scope* scope, Object* args)
{
    if (length(args) != 1)
    {
        error("eval takes exactly one argument");
        return Nil;
    }

    return eval(scope, eval(scope, args->car));
}

Object* builtin_print(Scope* scope, Object* args)
{
    print(eval(scope, args->car));
    return Nil;
}

Object* builtin_cons(Scope* scope, Object* args)
{
    if (length(args) != 2)
    {
        error("cons takes exactly two arguments");
        return Nil;
    }

    return cons(eval(scope, args->car), eval(scope, args->cdr->car));
}

Object* builtin_car(Scope* scope, Object* args)
{
    args = eval(scope, args->car);

    if (args->type != TYPE_CELL)
    {
        error("car takes exactly one argument");
        return Nil;
    }

    return args->car;
}

Object* builtin_cdr(Scope* scope, Object* args)
{
    args = eval(scope, args->car);

    if (args->type != TYPE_CELL)
    {
        error("cdr takes a list as its argument");
        return Nil;
    }

    return args->cdr;
}

Object* builtin_eq(Scope* scope, Object* args)
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
        return True;

    case TYPE_CELL:
    case TYPE_BUILTIN:
    case TYPE_FUNCTION:
        return Nil;

    case TYPE_NUMBER:
        return lhs->number == rhs->number ? True : Nil;

    case TYPE_SYMBOL:
        return strcmp(lhs->name, rhs->name) == 0 ? True : Nil;
    }

    assert(!true);
    return Nil;
}

Object* builtin_if(Scope* scope, Object* args)
{
    if (length(args) != 3)
    {
        error("if takes exactly three arguments");
        return Nil;
    }

    return eval(scope, eval(scope, args->car) != Nil ? args->cdr->car : args->cdr->cdr->car);
}

Object* builtin_exit(Scope*, Object*)
{
    running = false;
    return Nil;
}

Object* builtin_defun(Scope* scope, Object* args)
{
    if (length(args) != 3)
    {
        error("defun takes exactly three arguments");
        return Nil;
    }

    Object* name = args->car;
    Object* params = args->cdr->car;
    Object* body = args->cdr->cdr->car;

    Object* func = make_function(name, params, body);
    bind_value(scope, name, func);
    return func;
}

// Garbage collection

void collect_garbage()
{
}

// The program itself

void define_builtins()
{
    bind_value(&Env, symbol("nil"), Nil);
    bind_value(&Env, symbol("t"), True);

    define_builtin_function("+", builtin_add);
    define_builtin_function("quote", builtin_quote);
    define_builtin_function("eval", builtin_eval);
    define_builtin_function("print", builtin_print);
    define_builtin_function("cons", builtin_cons);
    define_builtin_function("car", builtin_car);
    define_builtin_function("cdr", builtin_cdr);
    define_builtin_function("=", builtin_eq);
    define_builtin_function("if", builtin_if);
    define_builtin_function("exit", builtin_exit);
    define_builtin_function("defun", builtin_defun);
}

void parse()
{
    printf("> ");
    fflush(stdout);

    Object* obj = parse_expr();

    if (echo)
    {
        printf("\n");
    }

    print(eval(&Env, obj));
    collect_garbage();
}

int main(int argc, char** argv)
{
    size_t memory_size = 1024 * 1024;

    int ch;

    while ((ch  = getopt(argc, argv, "em:")) != -1)
    {
        switch (ch)
        {
        case 'm':
            memory_size = atoi(optarg);
            break;

        case 'e':
            echo = true;
            break;

        default:
            printf("Unknown option: %c\n", ch);
            return 1;
        }
    }

    mem_root = malloc(memory_size);
    mem_ptr = mem_root;
    mem_end = mem_root + memory_size;

    define_builtins();

    while (running)
    {
        parse();
    }

    free(mem_root);
}
