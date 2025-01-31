#include "compiler.h"
#include "lisp.h"

// Only x86-64 is supported currently
#include "impl/x86_64.h"

#define COMPILE_MEM_SIZE 4096

#define MAX(a, b) (a > b ? a : b)

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
bool compile_expr_recurse(uint8_t** mem, Object* self, Object* params, Object* obj, bool can_recurse);

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
    if (get_type(body) != TYPE_CELL)
    {
        return true;
    }

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
    if (type == TYPE_NUMBER || type == TYPE_CONST
        || (type == TYPE_SYMBOL && (body == symbol("nil") || body == symbol("t"))))
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
    else if (get_type(func) == TYPE_FUNCTION && get_obj(func)->compiled == COMPILE_CODE)
    {
        debug("Other compiled function");
    }
    else if (get_type(func) != TYPE_BUILTIN)
    {
        error("Not a builtin or a compiled function, too complex");
        print(body);
        return false;
    }
    else if (!is_supported_builtin(get_obj(func)->fn))
    {
        error("Builtin not supported, too complex");
        print(body);
        return false;
    }

    assert(get_type(car(body)) == TYPE_BUILTIN || func == self
           || (get_type(car(body)) == TYPE_FUNCTION && get_obj(car(body))->compiled == COMPILE_CODE));
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

// A single statement (if, eq, +) turns into bytecode that has multiple
// parts. Thus, one function is transformed one bite (pun intended) at a time
// into bytecode that then goes through (eventually) optimization and finally
// gets transformed into machine code.
struct Bite
{
    char  id[10];
    int   op;
    bool  printed;
    int   reg;
    int   reg_count;

    struct Bite* arg1;
    struct Bite* arg2;
};

typedef struct Bite Bite;

enum BiteType {
    OP_CONSTANT,
    OP_PARAMETER,
    OP_ADD,
    OP_SUB,
    OP_NEG,
    OP_LESS,
    OP_EQ,
    OP_PTR,
    OP_IF,
    OP_BRANCH,
    OP_LIST,
    OP_RECURSE,
    OP_CALL,
};

int bite_ids;

Bite* make_bite(Bite** bites)
{
    Bite* rv = *bites;
    *bites = rv + 1;

    const int radix = ('z' + 1) - 'a';
    int id = bite_ids++;
    bool big = false;
    char buffer[sizeof(rv->id)];
    char* ptr = buffer;

    while (id >= radix)
    {
        *ptr++ = 'a' + (id % radix);
        id /= radix;
        big = true;
    }

    *ptr++ = 'a' + (id % radix) - (big ? 1 : 0);
    char* output = rv->id;

    while (ptr > buffer)
    {
        *output++ = *--ptr;
    }

    *output = 0;

    rv->reg = -1;
    rv->reg_count = 0;
    rv->printed = false;
    return rv;
}

Bite* bite_expr(Bite** bites, Object* self, Object* params, Object* obj);

Bite* bite_argument(Bite** bites, Object* params, Object* arg)
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

    Bite* b = make_bite(bites);
    b->op = OP_PARAMETER;
    b->arg1 = (Bite*)(intptr_t)(i * OBJ_SIZE);
    return b;
}

Bite* bite_immediate(Bite** bites, Object* arg)
{
    Bite* b = make_bite(bites);
    b->op = OP_CONSTANT;
    b->arg1 = (Bite*)arg;
    return b;
}

Bite* bite_recursion(Bite** bites, Object* self, Object* params, Object* args)
{
    Bite* arglist = NULL;

    for (; args != Nil; args = cdr(args))
    {
        Bite* list = make_bite(bites);
        list->op = OP_LIST;
        list->arg1 = bite_expr(bites, self, params, car(args));
        list->arg2 = arglist;
        arglist = list;
    }

    Bite* rec = make_bite(bites);
    rec->op = OP_RECURSE;
    rec->arg1 = arglist;
    return rec;
}

Bite* bite_call(Bite** bites, Object* self, Object* params, Object* func, Object* args)
{
    Bite* arglist = NULL;

    for (; args != Nil; args = cdr(args))
    {
        Bite* list = make_bite(bites);
        list->op = OP_LIST;
        list->arg1 = bite_expr(bites, self, params, car(args));
        list->arg2 = arglist;
        arglist = list;
    }

    Bite* call = make_bite(bites);
    call->op = OP_CALL;
    call->arg1 = arglist;
    call->arg2 = (Bite*)func_body(func);
    return call;
}

Bite* bite_add(Bite** bites, Object* self, Object* params, Object* args)
{
    int num_args = length(args);

    if (num_args == 0)
    {
        return bite_immediate(bites, 0);
    }
    else if (num_args == 1)
    {
        return bite_expr(bites, self, params, car(args));
    }

    Bite* lhs = bite_expr(bites, self, params, car(args));

    for (args = cdr(args); args != Nil; args = cdr(args))
    {
        Bite* rhs = bite_expr(bites, self, params, car(args));
        Bite* add = make_bite(bites);
        add->op = OP_ADD;
        add->arg1 = lhs;
        add->arg2 = rhs;
        lhs = add;
    }

    assert(lhs);
    return lhs;
}

Bite* bite_sub(Bite** bites, Object* self, Object* params, Object* args)
{
    int num_args = length(args);

    if (num_args == 1)
    {
        Bite* b = bite_expr(bites, self, params, car(args));
        Bite* neg = make_bite(bites);
        neg->op = OP_NEG;
        neg->arg1 = b;
        return neg;
    }

    Bite* lhs = bite_expr(bites, self, params, car(args));

    for (args = cdr(args); args != Nil; args = cdr(args))
    {
        Bite* rhs = bite_expr(bites, self, params, car(args));
        Bite* sub = make_bite(bites);
        sub->op = OP_SUB;
        sub->arg1 = lhs;
        sub->arg2 = rhs;
        lhs = sub;
    }

    assert(lhs);
    return lhs;
}

Bite* bite_less(Bite** bites, Object* self, Object* params, Object* args)
{
    Bite* lhs = bite_expr(bites, self, params, car(args));
    Bite* rhs = bite_expr(bites, self, params, car(cdr(args)));
    Bite* less = make_bite(bites);
    less->op = OP_LESS;
    less->arg1 = lhs;
    less->arg2 = rhs;
    return less;

}

Bite* bite_eq(Bite** bites, Object* self, Object* params, Object* args)
{
    Bite* lhs = bite_expr(bites, self, params, car(args));
    Bite* rhs = bite_expr(bites, self, params, car(cdr(args)));
    Bite* less = make_bite(bites);
    less->op = OP_EQ;
    less->arg1 = lhs;
    less->arg2 = rhs;
    return less;
}

Bite* bite_car(Bite** bites, Object* self, Object* params, Object* args)
{
    Bite* val = bite_expr(bites, self, params, car(args));
    Bite* b = make_bite(bites);
    b->op = OP_PTR;
    b->arg1 = val;
    b->arg2 = (Bite*)(-TYPE_CELL + offsetof(Object, car));
    return b;
}

Bite* bite_cdr(Bite** bites, Object* self, Object* params, Object* args)
{
    Bite* val = bite_expr(bites, self, params, car(args));
    Bite* b = make_bite(bites);
    b->op = OP_PTR;
    b->arg1 = val;
    b->arg2 = (Bite*)(-TYPE_CELL + offsetof(Object, cdr));
    return b;
}

Bite* bite_if(Bite** bites, Object* self, Object* params, Object* args)
{
    Bite* cond = bite_expr(bites, self, params, car(args));
    Bite* if_true = bite_expr(bites, self, params, car(cdr(args)));
    Bite* if_false = bite_expr(bites, self, params, car(cdr(cdr(args))));
    Bite* branch = make_bite(bites);
    branch->op = OP_BRANCH;
    branch->arg1 = if_true;
    branch->arg2 = if_false;
    Bite* if_bite = make_bite(bites);
    if_bite->op = OP_IF;
    if_bite->arg1 = cond;
    if_bite->arg2 = branch;
    return if_bite;
}

Bite* bite_expr(Bite** bites, Object* self, Object* params, Object* obj)
{
    switch (get_type(obj))
    {
    case TYPE_CELL:
        {
            Object* fn = car(obj);

            if (fn == self)
            {
                return bite_recursion(bites, self, params, cdr(obj));
            }
            else if (get_type(fn) == TYPE_FUNCTION)
            {
                return bite_call(bites, self, params, fn, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_add)
            {
                return bite_add(bites, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_sub)
            {
                return bite_sub(bites, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_less)
            {
                return bite_less(bites, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_eq)
            {
                return bite_eq(bites, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_car)
            {
                return bite_car(bites, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_cdr)
            {
                return bite_cdr(bites, self, params, cdr(obj));
            }
            else if (get_obj(fn)->fn == builtin_if)
            {
                return bite_if(bites, self, params, cdr(obj));
            }
            else
            {
                error("Unknown builtin function");
            }
            break;
        }
    case TYPE_SYMBOL:
        if (obj == symbol("nil"))
        {
            return bite_immediate(bites, Nil);
        }
        else if (obj == symbol("t"))
        {
            return bite_immediate(bites, True);
        }

        return bite_argument(bites, params, obj);

    case TYPE_CONST:
    case TYPE_NUMBER:
        return bite_immediate(bites, obj);

    default:
        return bite_immediate(bites, Nil);
    }

    assert(!true);
    return bite_immediate(bites, Undefined);
}

void print_fixed(const char* fmt, ...)
{
    char buf[25];
    memset(buf, ' ', sizeof(buf));
    buf[sizeof(buf) - 1] = 0;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    assert(n < (int)sizeof(buf));
    buf[n] = ' ';
    va_end(args);
    printf("%s", buf);
}

void print_bite_constant(Bite* bite)
{
    print_fixed("%s = 0x%lx", bite->id, (intptr_t)bite->arg1);
}

void print_bite_parameter(Bite* bite)
{
    print_fixed("%s = args[%ld]", bite->id, (intptr_t)bite->arg1);
}

void print_bite_add(Bite* bite)
{
    print_fixed("%s = %s + %s", bite->id, bite->arg1->id, bite->arg2->id);
}

void print_bite_sub(Bite* bite)
{
    print_fixed("%s = %s - %s", bite->id, bite->arg1->id, bite->arg2->id);
}

void print_bite_neg(Bite* bite)
{
    print_fixed("%s = -%s", bite->id, bite->arg1->id);
}

void print_bite_less(Bite* bite)
{
    print_fixed("%s = %s < %s", bite->id, bite->arg1->id, bite->arg2->id);
}

void print_bite_eq(Bite* bite)
{
    print_fixed("%s = %s == %s", bite->id, bite->arg1->id, bite->arg2->id);
}

void print_bite_ptr(Bite* bite)
{
    print_fixed("%s = %s[%ld]", bite->id, bite->arg1->id, (intptr_t)bite->arg2);
}

void print_bite_if(Bite* bite)
{
    print_fixed("%s = %s ? %s : %s", bite->id, bite->arg1->id, bite->arg2->arg1->id, bite->arg2->arg2->id);
}

void print_bite_call_or_recurse(Bite* bite, const char* type)
{
    printf("%s = %s(", bite->id, type);

    for (Bite* b = bite->arg1; b; b = b->arg2)
    {
        printf("%s", b->arg1->id);

        if (b->arg2)
        {
            printf(", ");
        }
    }

    printf(")");
}


void print_bite_norecurse(Bite* bite)
{
    switch (bite->op)
    {
    case OP_CONSTANT:
        print_bite_constant(bite);
        break;
    case OP_PARAMETER:
        print_bite_parameter(bite);
        break;
    case OP_ADD:
        print_bite_add(bite);
        break;
    case OP_SUB:
        print_bite_sub(bite);
        break;
    case OP_NEG:
        print_bite_neg(bite);
        break;
    case OP_LESS:
        print_bite_less(bite);
        break;
    case OP_EQ:
        print_bite_eq(bite);
        break;
    case OP_PTR:
        print_bite_ptr(bite);
        break;
    case OP_IF:
        print_bite_if(bite);
        break;
    case OP_RECURSE:
        print_bite_call_or_recurse(bite, "recurse");
        break;
    case OP_CALL:
        print_bite_call_or_recurse(bite, "call");
        break;

    case OP_BRANCH:
    case OP_LIST:
    default:
        assert(false);
    }
}

void print_one_bitecode(Bite* bite)
{
    if (bite->printed)
    {
        return;
    }

    switch (bite->op)
    {
    case OP_CONSTANT:
        print_bite_constant(bite);
        break;
    case OP_PARAMETER:
        print_bite_parameter(bite);
        break;
    case OP_ADD:
        print_one_bitecode(bite->arg1);
        print_one_bitecode(bite->arg2);
        print_bite_add(bite);
        break;
    case OP_SUB:
        print_one_bitecode(bite->arg1);
        print_one_bitecode(bite->arg2);
        print_bite_sub(bite);
        break;
    case OP_NEG:
        print_one_bitecode(bite->arg1);
        print_bite_neg(bite);
        break;
    case OP_LESS:
        print_one_bitecode(bite->arg1);
        print_one_bitecode(bite->arg2);
        print_bite_less(bite);
        break;
    case OP_EQ:
        print_one_bitecode(bite->arg1);
        print_one_bitecode(bite->arg2);
        print_bite_eq(bite);
        break;
    case OP_PTR:
        print_one_bitecode(bite->arg1);
        print_bite_ptr(bite);
        break;
    case OP_IF:
        print_one_bitecode(bite->arg1);
        print_one_bitecode(bite->arg2->arg1);
        print_one_bitecode(bite->arg2->arg2);
        assert(bite->arg2->op == OP_BRANCH);
        print_bite_if(bite);
        break;

    case OP_RECURSE:
        for (Bite* b = bite->arg1; b; b = b->arg2)
        {
            print_one_bitecode(b->arg1);
        }

        print_bite_call_or_recurse(bite, "recurse");
        break;
    case OP_CALL:
        for (Bite* b = bite->arg1; b; b = b->arg2)
        {
            print_one_bitecode(b->arg1);
        }

        print_bite_call_or_recurse(bite, "call");
        break;

    case OP_BRANCH:
    case OP_LIST:
    default:
        assert(false);
    }

    printf("\n");
    bite->printed = true;
}

void mark_unprinted(Bite* bite)
{
    bite->printed = false;

    switch (bite->op)
    {
    case OP_CONSTANT:
    case OP_PARAMETER:
        break;

    case OP_ADD:
    case OP_SUB:
    case OP_LESS:
    case OP_EQ:
        mark_unprinted(bite->arg1);
        mark_unprinted(bite->arg2);
        break;

    case OP_NEG:
    case OP_PTR:
        mark_unprinted(bite->arg1);
        break;

    case OP_IF:
        mark_unprinted(bite->arg1);
        mark_unprinted(bite->arg2->arg1);
        mark_unprinted(bite->arg2->arg2);
        break;

    case OP_RECURSE:
    case OP_CALL:
        for (Bite* b = bite->arg1; b; b = b->arg2)
        {
            mark_unprinted(b->arg1);
        }
        break;

    case OP_BRANCH:
    case OP_LIST:
    default:
        assert(false);
    }
}


void print_bitecode(Bite* bite)
{
    mark_unprinted(bite);
    print_one_bitecode(bite);
}

//
// Direct compilation from lisp to machine code
//

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
    if (args == Nil)
    {
        return compile_immediate(mem, 0);
    }
    else if (cdr(args) == Nil)
    {
        return compile_expr(mem, self, params, car(args));
    }

    // At least two arguments
    assert(args != Nil && cdr(args) != Nil);
    RESERVE_STACK(OBJ_SIZE);
    compile_expr(mem, self, params, car(args));
    EMIT_MOV64_OFF8_REG(REG_FRAME, REG_RET, -OBJ_SIZE);

    for (args = cdr(args); args != Nil; args = cdr(args))
    {
        compile_expr(mem, self, params, car(args));
        EMIT_ADD64_OFF8_REG(REG_FRAME, REG_RET, -OBJ_SIZE);
    }

    EMIT_MOV64_REG_OFF8(REG_RET, REG_FRAME, -OBJ_SIZE);
    FREE_STACK(OBJ_SIZE);
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
        RESERVE_STACK(OBJ_SIZE);
        compile_expr(mem, self, params, car(args));
        EMIT_MOV64_OFF8_REG(REG_FRAME, REG_RET, -OBJ_SIZE);

        for (args = cdr(args); args != Nil; args = cdr(args))
        {
            compile_expr(mem, self, params, car(args));
            EMIT_SUB64_OFF8_REG(REG_FRAME, REG_RET, -OBJ_SIZE);
        }

        EMIT_MOV64_REG_OFF8(REG_RET, REG_FRAME, -OBJ_SIZE);
        FREE_STACK(OBJ_SIZE);
    }
    return true;
}

bool compile_less(uint8_t** mem, Object* self, Object* params, Object* args)
{
    RESERVE_STACK(OBJ_SIZE);

    compile_expr(mem, self, params, car(args));
    EMIT_SAR64_IMM8(REG_RET, 2);
    EMIT_MOV64_OFF8_REG(REG_FRAME, REG_RET, -OBJ_SIZE);
    compile_expr(mem, self, params, car(cdr(args)));
    EMIT_SAR64_IMM8(REG_RET, 2);

    EMIT_CMP64_REG_OFF8(REG_RET, REG_FRAME, -OBJ_SIZE);
    EMIT_MOV64_REG_IMM64(REG_RET, (intptr_t)True);
    EMIT_JL_OFF8();
    uint8_t* jump_start = *mem;

    EMIT_MOV64_REG_IMM64(REG_RET, (intptr_t)Nil);
    uint8_t* jump_end = *mem;
    PATCH_JMP8(jump_start, jump_end - jump_start);

    FREE_STACK(OBJ_SIZE);
    return true;
}

bool compile_eq(uint8_t** mem, Object* self, Object* params, Object* args)
{
    RESERVE_STACK(OBJ_SIZE);

    compile_expr(mem, self, params, car(args));
    EMIT_MOV64_OFF8_REG(REG_FRAME, REG_RET, -OBJ_SIZE);
    compile_expr(mem, self, params, car(cdr(args)));

    EMIT_CMP64_REG_OFF8(REG_RET, REG_FRAME, -OBJ_SIZE);
    EMIT_MOV64_REG_IMM64(REG_RET, (intptr_t)True);
    EMIT_JE_OFF8();
    uint8_t* jump_start = *mem;

    EMIT_MOV64_REG_IMM64(REG_RET, (intptr_t)Nil);
    uint8_t* jump_end = *mem;
    PATCH_JMP8(jump_start, jump_end - jump_start);

    FREE_STACK(OBJ_SIZE);
    return true;
}

bool compile_if(uint8_t** mem, Object* self, Object* params, Object* args)
{
    compile_expr(mem, self, params, car(args));
    EMIT_CMP64_REG_IMM8(REG_RET, (intptr_t)Nil);
    EMIT_JE_OFF32();
    uint8_t* jump_to_false = *mem;
    compile_expr_recurse(mem, self, params, car(cdr(args)), true);
    EMIT_JMP_OFF32();
    uint8_t* jump_to_end = *mem;
    compile_expr_recurse(mem, self, params, car(cdr(cdr(args))), true);
    uint8_t* end = *mem;
    PATCH_JMP32(jump_to_false, jump_to_end - jump_to_false);
    PATCH_JMP32(jump_to_end, end - jump_to_end);

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
    int pos = 1;
    RESERVE_STACK(OBJ_SIZE * len);

    for (; args != Nil; args = cdr(args))
    {
        compile_expr(mem, self, params, car(args));
        EMIT_MOV64_OFF8_REG(REG_FRAME, REG_RET, -OBJ_SIZE * pos++);
    }

    for (int i = 0; i < len; i++)
    {
        EMIT_MOV64_REG_OFF8(REG_RET, REG_FRAME, -OBJ_SIZE * (i + 1));
        EMIT_MOV64_OFF8_REG(REG_ARGS, REG_RET, OBJ_SIZE * i);
    }

    FREE_STACK(OBJ_SIZE * len);

    // Patch the offset right away
    EMIT_JMP_OFF32_NO_PLACEHOLDER();
    uint8_t* start = (uint8_t*)func_body(self);
    ptrdiff_t backwards = start - *mem - 4; // The extra 4 is for the imm32 that we emit right now
    EMIT_IMM32(backwards);

    return true;
}

bool compile_call(uint8_t** mem, Object* self, Object* params, Object* func, Object* args)
{
    int len = length(args);
    int pos = 0;
    RESERVE_STACK(OBJ_SIZE * len);

    for (; args != Nil; args = cdr(args))
    {
        compile_expr(mem, self, params, car(args));
        EMIT_MOV64_OFF8_REG(REG_FRAME, REG_RET, -OBJ_SIZE * (len - pos));
        pos++;
    }

    EMIT_PUSH(REG_ARGS);
    EMIT_MOV64_REG_REG(REG_ARGS, REG_FRAME);
    EMIT_SUB64_IMM8(REG_ARGS, OBJ_SIZE * len);

    intptr_t fn = (intptr_t)func_body(func);
    EMIT_MOV64_REG_IMM64(REG_RET, fn);
    EMIT_CALL_REG(REG_RET);

    EMIT_POP(REG_ARGS);
    FREE_STACK(OBJ_SIZE * len);

    return true;
}

bool compile_expr_recurse(uint8_t** mem, Object* self, Object* params, Object* obj, bool can_recurse)
{
    switch (get_type(obj))
    {
    case TYPE_CELL:
        {
            Object* fn = car(obj);

            if (fn == self)
            {
                if (can_recurse)
                {
                    return compile_recursion(mem, self, params, cdr(obj));
                }
                else
                {
                    error("Cannot compile self-recursion in a non-tail recursive context.");
                }
            }
            else if (get_type(fn) == TYPE_FUNCTION)
            {
                return compile_call(mem, self, params, fn, cdr(obj));
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

bool compile_expr(uint8_t** mem, Object* self, Object* params, Object* obj)
{
    return compile_expr_recurse(mem, self, params, obj, false);
}

//
// Bite compilation to machine code
//

#define TEMP_REGISTERS 4
#define LAST_TEMP_REGISTER TEMP_REGISTERS - 1

int get_register(Bite* bite)
{
    switch (bite->reg)
    {
    case 0:
        return REG_RET;
    case 1:
        return REG_TMP1;
    case 2:
        return REG_TMP2;
    case 3:
        return REG_TMP3;
    default:
        assert(false);
        break;
    }

    return -1;
}

bool is_argument(Bite* bite)
{
    return bite->op == OP_PARAMETER;
}

intptr_t get_constant(Bite* bite)
{
    return (intptr_t)bite->arg1;
}

int get_temp_offset(int tmp)
{
    return (tmp + 1) * -8;
}

struct RegList
{
    int reg[4];
    int size;
};

typedef struct RegList RegList;

RegList* reglist;
int temps = 0;

RegList* reglist_push(RegList* dest, int reg)
{
    assert(reg >= 0);
    assert(reglist->size > 0);
    memcpy(dest, reglist, sizeof(*reglist));

    for (int i = 0; i < dest->size; i++)
    {
        if (dest->reg[i] == reg)
        {
            for (int j = i + 1; j < dest->size; j++)
            {
                dest->reg[j - 1] = dest->reg[j];
            }

            break;
        }
    }

    dest->size--;
    RegList* prev = reglist;
    reglist = dest;

    if (debug_on())
    {
        printf("Removed register %d: [", reg);

        for (int i = 0; i < prev->size; i++)
        {
            printf(" %d", prev->reg[i]);
        }

        printf(" ] [");

        for (int i = 0; i < reglist->size; i++)
        {
            printf(" %d", reglist->reg[i]);
        }

        printf(" ]\n");
    }

    return prev;
}

void reglist_pop(RegList* list)
{
    reglist = list;
}

bool bite_compile(uint8_t** mem, Bite* bite);

bool bite_compile_constant(uint8_t** mem, Bite* bite)
{
    bite->reg = reglist->reg[0];
    debug("%s takes register %d", bite->id, bite->reg);
    EMIT_MOV64_REG_IMM64(get_register(bite), get_constant(bite));
    return true;
}

bool bite_compile_argument(uint8_t** mem, Bite* bite)
{
    bite->reg = reglist->reg[0];
    debug("%s takes register %d", bite->id, bite->reg);
    EMIT_MOV64_REG_OFF8(get_register(bite), REG_ARGS, get_constant(bite));
    return true;
}

bool bite_compile_binary_op(uint8_t** mem, Bite* bite, int op)
{
    Bite* lhs = bite->arg1;
    Bite* rhs = bite->arg2;

    // Constant should've been folded by now
    assert(lhs->op != OP_CONSTANT || rhs->op != OP_CONSTANT);

    if (rhs->reg_count == 0)
    {
        if (!bite_compile(mem, lhs))
        {
            return false;
        }

        if (is_argument(rhs))
        {
            switch (op)
            {
            case OP_ADD:
                EMIT_ADD64_REG_OFF8(get_register(lhs), REG_ARGS, get_constant(rhs));
                break;

            case OP_SUB:
                EMIT_SUB64_REG_OFF8(get_register(lhs), REG_ARGS, get_constant(rhs));
                break;

            case OP_LESS:
            case OP_EQ:
                EMIT_CMP64_REG_OFF8(get_register(lhs), REG_ARGS, get_constant(rhs));
                break;

            default:
                assert(false);
                break;
            }
        }
        else
        {
            // TODO: deal with larger than 32-bit constants
            assert(get_constant(rhs) < 0xffffffff);

            switch (op)
            {
            case OP_ADD:
                EMIT_ADD64_IMM32(get_register(lhs), get_constant(rhs));
                break;

            case OP_SUB:
                EMIT_SUB64_IMM32(get_register(lhs), get_constant(rhs));
                break;

            case OP_LESS:
            case OP_EQ:
                EMIT_CMP64_IMM32(get_register(lhs), get_constant(rhs));
                break;

            default:
                assert(false);
                break;
            }
        }

        bite->reg = lhs->reg;
        debug("%s uses register %d from %s", bite->id, bite->reg, lhs->id);
    }
    else if (rhs->reg_count <= lhs->reg_count && rhs->reg_count < reglist->size)
    {
        if (!bite_compile(mem, lhs))
        {
            return false;
        }


        RegList r;
        RegList* prev = reglist_push(&r, lhs->reg);

        if (!bite_compile(mem, rhs))
        {
            return false;
        }

        reglist_pop(prev);

        assert(rhs->reg != lhs->reg);

        switch (op)
        {
        case OP_ADD:
            EMIT_ADD64_REG_REG(get_register(lhs), get_register(rhs));
            break;

        case OP_SUB:
            EMIT_SUB64_REG_REG(get_register(lhs), get_register(rhs));
            break;

        case OP_LESS:
        case OP_EQ:
            EMIT_CMP64_REG_REG(get_register(lhs), get_register(rhs));
            break;

        default:
            assert(false);
            break;
        }

        bite->reg = lhs->reg;
        debug("%s uses register %d from %s", bite->id, bite->reg, lhs->id);
    }
    else if (rhs->reg_count > lhs->reg_count && lhs->reg_count < reglist->size)
    {
        if (!bite_compile(mem, rhs))
        {
            return false;
        }

        RegList r;
        RegList* prev = reglist_push(&r, rhs->reg);

        if (!bite_compile(mem, lhs))
        {
            return false;
        }

        reglist_pop(prev);

        assert(rhs->reg != lhs->reg);

        switch (op)
        {
        case OP_ADD:
            EMIT_ADD64_REG_REG(get_register(lhs), get_register(rhs));
            break;

        case OP_SUB:
            EMIT_SUB64_REG_REG(get_register(lhs), get_register(rhs));
            break;

        case OP_LESS:
        case OP_EQ:
            EMIT_CMP64_REG_REG(get_register(lhs), get_register(rhs));
            break;

        default:
            assert(false);
            break;
        }

        bite->reg = lhs->reg;
        debug("%s uses register %d from %s", bite->id, bite->reg, lhs->id);
    }
    else
    {
        // Spill to memory
        assert(rhs->reg_count >= reglist->size && lhs->reg_count >= reglist->size);

        if (!bite_compile(mem, rhs))
        {
            return false;
        }

        int temp = temps++;
        assert(get_temp_offset(temp) < 128);
        EMIT_MOV64_OFF8_REG(REG_FRAME, get_register(rhs), get_temp_offset(temp));
        debug("%s spilled to memory from register %d", rhs->id, rhs->reg);

        if (!bite_compile(mem, lhs))
        {
            return false;
        }

        switch (op)
        {
        case OP_ADD:
            EMIT_ADD64_REG_OFF8(get_register(lhs), REG_FRAME, get_temp_offset(temp));
            break;

        case OP_SUB:
            EMIT_SUB64_REG_OFF8(get_register(lhs), REG_FRAME, get_temp_offset(temp));
            break;

        case OP_LESS:
        case OP_EQ:
            EMIT_CMP64_REG_OFF8(get_register(lhs), REG_FRAME, get_temp_offset(temp));
            break;

        default:
            assert(false);
            break;
        }

        bite->reg = lhs->reg;
    }

    if (bite->op == OP_EQ || bite->op == OP_LESS)
    {
        EMIT_MOV64_REG_IMM64(get_register(bite), (intptr_t)True);

        if (bite->op == OP_EQ)
        {
            EMIT_JE_OFF8();
        }
        else
        {
            EMIT_JL_OFF8();
        }
        uint8_t* jump_start = *mem;

        EMIT_MOV64_REG_IMM64(get_register(bite), (intptr_t)Nil);
        uint8_t* jump_end = *mem;
        PATCH_JMP8(jump_start, jump_end - jump_start);
    }

    return true;
}

bool bite_compile(uint8_t** mem, Bite* bite)
{
    switch (bite->op)
    {
    case OP_CONSTANT:
        return bite_compile_constant(mem, bite);

    case OP_PARAMETER:
        return bite_compile_argument(mem, bite);

    case OP_ADD:
    case OP_SUB:
    case OP_EQ:
    case OP_LESS:
        return bite_compile_binary_op(mem, bite, bite->op);

    case OP_NEG:
    case OP_PTR:
    case OP_IF:
    case OP_RECURSE:
    case OP_CALL:
    case OP_BRANCH:
    case OP_LIST:
    default:
        break;
    }

    return false;
}

Bite* fold_constants(Bite* bite);

Bite* compile_time_add(Bite* arg1, Bite* arg2)
{
    Object* lhs = (Object*)arg1->arg1;
    Object* rhs = (Object*)arg2->arg1;
    Object* result = make_number(get_number(lhs) + get_number(rhs));

    debug("Compile time add: %s + %s => %ld + %ld = %ld => %p + %p = %p",
          arg1->id, arg2->id,
          get_number(lhs), get_number(rhs), get_number(lhs) + get_number(rhs),
          lhs, rhs, result);

    arg1->arg1 = (Bite*)result;
    return arg1;
}

Bite* compile_time_sub(Bite* arg1, Bite* arg2)
{
    Object* lhs = (Object*)arg1->arg1;
    Object* rhs = (Object*)arg2->arg1;
    Object* result = make_number(get_number(lhs) - get_number(rhs));

    debug("Compile time sub: %s - %s => %ld - %ld = %ld => %p - %p = %p",
          arg1->id, arg2->id,
          get_number(lhs), get_number(rhs), get_number(lhs) - get_number(rhs),
          lhs, rhs, result);

    arg1->arg1 = (Bite*)result;
    return arg1;
}

Bite* optimize_add_sub(Bite* arith, bool* optimized)
{
    debug("%s: %s", arith->op == OP_ADD ? "ADD" : "SUB", arith->id);

    if (arith->arg1->op == OP_CONSTANT && arith->arg2->op == OP_CONSTANT)
    {
        if (arith->op == OP_ADD)
        {
            arith = compile_time_add(arith->arg1, arith->arg2);
        }
        else
        {
            arith = compile_time_sub(arith->arg1, arith->arg2);
        }

        *optimized = true;
    }
    else if (arith->arg1->op == arith->op && arith->arg2->op == OP_CONSTANT)
    {
        // For both addition and subtraction, we can add up the constants together.
        bool found = false;

        for (Bite* b = arith->arg1; !found && b->op == arith->op; b = b->arg1)
        {
            if (b->arg2->op == OP_CONSTANT)
            {
                b->arg2 = compile_time_add(arith->arg2, b->arg2);
                arith = arith->arg1;
                *optimized = true;
                break;
            }
            else if (b->arg1->op == OP_CONSTANT)
            {
                b->arg1 = compile_time_add(arith->arg1, b->arg1);
                arith = arith->arg2;
                *optimized = true;
                break;
            }
        }
    }

    return arith;
}

Bite* fold_constants(Bite* bite)
{
    bool optimized = false;
    switch (bite->op)
    {
    case OP_CONSTANT:
    case OP_PARAMETER:
        break;

    case OP_SUB:
    case OP_ADD:
        do
        {
            optimized = false;
            bite->arg1 = fold_constants(bite->arg1);
            bite->arg2 = fold_constants(bite->arg2);
            bite = optimize_add_sub(bite, &optimized);
        }
        while (optimized && (bite->op == OP_ADD || bite->op == OP_SUB));
        break;

    case OP_LESS:
    case OP_EQ:
        bite->arg1 = fold_constants(bite->arg1);
        bite->arg2 = fold_constants(bite->arg2);
        break;

    case OP_NEG:
    case OP_PTR:
        bite->arg1 = fold_constants(bite->arg1);
        break;

    case OP_IF:
        bite->arg1 = fold_constants(bite->arg1);
        bite->arg2->arg1 = fold_constants(bite->arg2->arg1);
        bite->arg2->arg2 = fold_constants(bite->arg2->arg2);
        break;

    case OP_RECURSE:
    case OP_CALL:
        for (Bite* b = bite->arg1; b; b = b->arg2)
        {
            b->arg1 = fold_constants(b->arg1);
        }
        break;

    case OP_BRANCH:
    case OP_LIST:
    default:
        assert(false);
    }

    return bite;
}


Bite* temporaries[128];

void add_temp(Bite* bite)
{
    ptrdiff_t num = 0;
    Bite** b = temporaries;

    while (b[num])
    {
        if (b[num] == bite)
        {
            debug("Already in use: %s", bite->id);
            return;
        }

        num++;
    }

    bite->reg = num;

    debug("Allocated: %s to slot %d", bite->id, bite->reg);

    b[num] = bite;
}

void remove_temp(Bite* bite)
{
    Bite** b = temporaries;

    while (*b)
    {
        if (*b == bite)
        {
            debug("Free: %s from slot %d", bite->id, bite->reg);

            Bite** out = b++;

            while (*b)
            {
                *out++ = *b++;
            }

            *out = NULL;
            return;
        }

        b++;
    }
}

const char* reg_name(int reg)
{
    switch (reg)
    {
    case 0:
        return "rax";
    case 1:
        return "rsi";
    case 2:
        return "rdx";
    case 3:
        return "rcx";
    }

    static char buffer[64];
    sprintf(buffer, "temp@%d", reg);
    return buffer;
}

typedef void (*RecurseBiteFunc)(Bite*, int);

void recurse_bites(Bite* bite, RecurseBiteFunc func, int depth)
{
    switch (bite->op)
    {
    case OP_CONSTANT:
    case OP_PARAMETER:
        break;

    case OP_ADD:
    case OP_SUB:
    case OP_LESS:
    case OP_EQ:
        recurse_bites(bite->arg1, func, depth + 1);
        recurse_bites(bite->arg2, func, depth + 1);
        break;

    case OP_NEG:
    case OP_PTR:
        recurse_bites(bite->arg1, func, depth + 1);
        break;

    case OP_IF:
        recurse_bites(bite->arg1, func, depth + 1);
        recurse_bites(bite->arg2->arg1, func, depth + 1);
        recurse_bites(bite->arg2->arg2, func, depth + 1);
        break;

    case OP_RECURSE:
    case OP_CALL:
        for (Bite* b = bite->arg1; b; b = b->arg2)
        {
            recurse_bites(b->arg1, func, depth + 1);
        }
        break;

    case OP_BRANCH:
    case OP_LIST:
    default:
        assert(false);
    }

    func(bite, depth);
}

void print_registers(Bite* bite, int depth)
{
    printf("|");
    for (int i = 0; i < depth; i++)
    {
        printf("-");
    }
    printf("> ");

    print_bite_norecurse(bite);
    printf(" [%d]", bite->reg_count);

    if (bite->reg != -1)
    {
        printf(" %s", reg_name(bite->reg));
    }

    printf("\n");
}

void calculate_register_count(Bite* bite, bool left_leaf)
{
    switch (bite->op)
    {
    case OP_CONSTANT:
    case OP_PARAMETER:
        bite->reg_count = left_leaf ? 1 : 0;
        break;

    case OP_ADD:
    case OP_SUB:
    case OP_LESS:
    case OP_EQ:
        calculate_register_count(bite->arg1, true);
        calculate_register_count(bite->arg2, false);

        if (bite->arg1->reg_count == bite->arg2->reg_count)
        {
            bite->reg_count = bite->arg1->reg_count + 1;
        }
        else
        {
            bite->reg_count = MAX(bite->arg1->reg_count, bite->arg2->reg_count);
        }
        break;

    case OP_NEG:
    case OP_PTR:
        calculate_register_count(bite->arg1, true);
        bite->reg_count = bite->arg1->reg_count;
        break;

    case OP_IF:
        calculate_register_count(bite->arg1, false);
        calculate_register_count(bite->arg2->arg1, false);
        calculate_register_count(bite->arg2->arg2, false);
        break;

    case OP_RECURSE:
    case OP_CALL:
        {
            int reg_count = -1;

            for (Bite* b = bite->arg1; b; b = b->arg2)
            {
                calculate_register_count(b->arg1, false);

                if (reg_count == -1)
                {
                    reg_count = b->reg_count;
                }
                else if (b->reg_count > reg_count)
                {
                    reg_count = b->reg_count;
                }
            }

            bite->reg_count = MAX(0, reg_count);
        }
        break;

    case OP_BRANCH:
    case OP_LIST:
    default:
        assert(false);
    }
}

bool generate_bytecode(uint8_t** mem, Object* /*scope*/, Object* /*name*/, Object* self, Object* params, Object* body)
{
    Bite bitecode[1024];
    Bite* ptr = bitecode;
    bite_ids = 0;
    Bite* res = bite_expr(&ptr, self, params, body);

    if (debug_on())
    {
        debug("Generated %ld bites, resulting variable is: %s.\n", ptr - bitecode, res->id);
        print_bitecode(res);
    }

    res = fold_constants(res);

    if (debug_on())
    {
        debug("After constant folding");
        print_bitecode(res);
    }

    calculate_register_count(res, false);

    RegList regs;
    for (int i = 0; i < 4; i++)
    {
        regs.reg[i] = i;
    }

    regs.size = TEMP_REGISTERS;
    reglist = &regs;

    if (debug_on())
    {
        debug("After counting registers");
        recurse_bites(res, print_registers, 0);
    }

    bool ok;
    uint8_t* orig_mem = *mem;

    size_t stack_space = 0;

    if (res->reg_count > TEMP_REGISTERS)
    {
        stack_space = OBJ_SIZE * (res->reg_count - TEMP_REGISTERS);
    }

    if (stack_space > 0)
    {
        RESERVE_STACK(stack_space);
    }

    ok = bite_compile(mem, res);

    if (ok && res->reg > 0)
    {
        // If the return value didn't end up being in RAX, move it there.
        // This probably could be solved somehow but this'll do for now.
        EMIT_MOV64_REG_REG(REG_RET, get_register(res));
    }

    if (stack_space > 0)
    {
        FREE_STACK(stack_space);
    }

    if (!ok)
    {
        debug("Bite compilation FAILED!");
        *mem = orig_mem;
        ok = compile_expr(mem, self, params, body);
    }
    else if (debug_on())
    {
        debug("Bite compilation successful!");
        recurse_bites(res, print_registers, 0);
    }

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

            printf("BEGIN dump of '%s'\n", get_symbol(name));
            fflush(stdout);
            system(buffer);
            printf("END dump of '%s'\n", get_symbol(name));
            fflush(stdout);
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

            debug("<<< %s '%s' >>>", compile_level == COMPILE_CODE ? "Compiling": "Resolving symbols for",
                  get_symbol(name));

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

// The JIT functions receive their arguments in RDI
typedef Object* (*JitFunc)(Object**);

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

    // The function arguments are bound in the reverse order they are declared to the
    // scope. Each value is copied into the stack buffer that is then passed
    // into the compiled function. The compiled functions expect the arguments
    // to be stored in RDI and a temporary stack pointer to be in RSI.
    for (Object* o = args; o != Nil; o = cdr(o))
    {
        arg_stack[--len] = cdr(car(o));
    }

    JitFunc func = (JitFunc)func_body(fn);
    return func(arg_stack);
}
