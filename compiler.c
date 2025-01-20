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
    char  id;
    int   ver;
    int   op;
    bool  printed;
    int   reg;

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

char bite_ids = 'a';

Bite* make_bite(Bite** bites, char id, int ver)
{
    Bite* rv = *bites;
    *bites = rv + 1;

    if (id == 0)
    {
        rv->id = bite_ids++;
        rv->ver = 0;
    }
    else if (ver != -1)
    {
        rv->id = id;
        rv->ver = ver + 1;
    }

    rv->reg = -1;
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

    Bite* b = make_bite(bites, 0, 0);
    b->op = OP_PARAMETER;
    b->arg1 = (Bite*)(intptr_t)(i * OBJ_SIZE);
    return b;
}

Bite* bite_immediate(Bite** bites, Object* arg)
{
    Bite* b = make_bite(bites, 0, 0);
    b->op = OP_CONSTANT;
    b->arg1 = (Bite*)arg;
    return b;
}

Bite* bite_recursion(Bite** bites, Object* self, Object* params, Object* args)
{
    Bite* arglist = NULL;

    for (; args != Nil; args = cdr(args))
    {
        Bite* list = make_bite(bites, -1, -1);
        list->op = OP_LIST;
        list->arg1 = bite_expr(bites, self, params, car(args));
        list->arg2 = arglist;
        arglist = list;
    }

    Bite* rec = make_bite(bites, 0, 0);
    rec->op = OP_RECURSE;
    rec->arg1 = arglist;
    return rec;
}

Bite* bite_call(Bite** bites, Object* self, Object* params, Object* func, Object* args)
{
    Bite* arglist = NULL;

    for (; args != Nil; args = cdr(args))
    {
        Bite* list = make_bite(bites, -1, -1);
        list->op = OP_LIST;
        list->arg1 = bite_expr(bites, self, params, car(args));
        list->arg2 = arglist;
        arglist = list;
    }

    Bite* call = make_bite(bites, 0, 0);
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
    char id = 0;
    int ver = 0;

    for (args = cdr(args); args != Nil; args = cdr(args))
    {
        Bite* rhs = bite_expr(bites, self, params, car(args));
        Bite* add = make_bite(bites, id, ver);
        add->op = OP_ADD;
        add->arg1 = lhs;
        add->arg2 = rhs;
        id = add->id;
        ver = add->ver;
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
        Bite* neg = make_bite(bites, b->id, b->ver);
        neg->op = OP_NEG;
        neg->arg1 = b;
        return neg;
    }

    Bite* lhs = bite_expr(bites, self, params, car(args));
    char id = 0;
    int ver = 0;

    for (args = cdr(args); args != Nil; args = cdr(args))
    {
        Bite* rhs = bite_expr(bites, self, params, car(args));
        Bite* sub = make_bite(bites, id, ver);
        sub->op = OP_SUB;
        sub->arg1 = lhs;
        sub->arg2 = rhs;
        id = sub->id;
        ver = sub->ver;
        lhs = sub;
    }

    assert(lhs);
    return lhs;
}

Bite* bite_less(Bite** bites, Object* self, Object* params, Object* args)
{
    Bite* lhs = bite_expr(bites, self, params, car(args));
    Bite* rhs = bite_expr(bites, self, params, car(cdr(args)));
    Bite* less = make_bite(bites, 0, 0);
    less->op = OP_LESS;
    less->arg1 = lhs;
    less->arg2 = rhs;
    return less;

}

Bite* bite_eq(Bite** bites, Object* self, Object* params, Object* args)
{
    Bite* lhs = bite_expr(bites, self, params, car(args));
    Bite* rhs = bite_expr(bites, self, params, car(cdr(args)));
    Bite* less = make_bite(bites, 0, 0);
    less->op = OP_EQ;
    less->arg1 = lhs;
    less->arg2 = rhs;
    return less;
}

Bite* bite_car(Bite** bites, Object* self, Object* params, Object* args)
{
    Bite* val = bite_expr(bites, self, params, car(args));
    Bite* b = make_bite(bites, val->id, val->ver);
    b->op = OP_PTR;
    b->arg1 = val;
    b->arg2 = (Bite*)(-TYPE_CELL + offsetof(Object, car));
    return b;
}

Bite* bite_cdr(Bite** bites, Object* self, Object* params, Object* args)
{
    Bite* val = bite_expr(bites, self, params, car(args));
    Bite* b = make_bite(bites, val->id, val->ver);
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
    Bite* branch = make_bite(bites, -1, -1);
    branch->op = OP_BRANCH;
    branch->arg1 = if_true;
    branch->arg2 = if_false;
    Bite* if_bite = make_bite(bites, branch->id, branch->ver);
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
    char buf[20];
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
    print_fixed("%c%d = 0x%lx", bite->id, bite->ver, (intptr_t)bite->arg1);
}

void print_bite_parameter(Bite* bite)
{
    print_fixed("%c%d = args[%ld]", bite->id, bite->ver, (intptr_t)bite->arg1);
}

void print_bite_add(Bite* bite)
{
    print_fixed("%c%d = %c%d + %c%d", bite->id, bite->ver, bite->arg1->id, bite->arg1->ver, bite->arg2->id, bite->arg2->ver);
}

void print_bite_sub(Bite* bite)
{
    print_fixed("%c%d = %c%d - %c%d", bite->id, bite->ver, bite->arg1->id, bite->arg1->ver, bite->arg2->id, bite->arg2->ver);
}

void print_bite_neg(Bite* bite)
{
    print_fixed("%c%d = -%c%d", bite->id, bite->ver, bite->arg1->id, bite->arg1->ver);
}

void print_bite_less(Bite* bite)
{
    print_fixed("%c%d = %c%d < %c%d", bite->id, bite->ver, bite->arg1->id, bite->arg1->ver, bite->arg2->id, bite->arg2->ver);
}

void print_bite_eq(Bite* bite)
{
    print_fixed("%c%d = %c%d == %c%d", bite->id, bite->ver, bite->arg1->id, bite->arg1->ver, bite->arg2->id, bite->arg2->ver);
}

void print_bite_ptr(Bite* bite)
{
    print_fixed("%c%d = %c%d[%ld]", bite->id, bite->ver, bite->arg1->id, bite->arg1->ver, (intptr_t)bite->arg2);
}

void print_bite_if(Bite* bite)
{
    print_fixed("%c%d = %c%d ? %c%d : %c%d", bite->id, bite->ver, bite->arg1->id, bite->arg1->ver,
           bite->arg2->arg1->id, bite->arg2->arg1->ver,bite->arg2->arg2->id, bite->arg2->arg2->ver);
}

void print_bite_call_or_recurse(Bite* bite, const char* type)
{
    printf("%c%d = %s(", bite->id, bite->ver, type);

    for (Bite* b = bite->arg1; b; b = b->arg2)
    {
        printf("%c%d", b->arg1->id, b->arg1->ver);

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

Bite* fold_constants(Bite* bite);

Bite* compile_time_add(Bite* arg1, Bite* arg2)
{
    Object* lhs = (Object*)arg1->arg1;
    Object* rhs = (Object*)arg2->arg1;
    Object* result = make_number(get_number(lhs) + get_number(rhs));

    debug("Compile time add: %c%d + %c%d => %p + %p = %p",
          arg1->id, arg1->ver, arg2->id, arg2->ver,
          lhs, rhs, result);

    arg1->arg1 = (Bite*)result;
    return arg1;
}

Bite* optimize_add(Bite* add, bool* optimized)
{
    debug("ADD: %c%d", add->id, add->ver);

    if (add->arg1->op == OP_CONSTANT && add->arg2->op == OP_CONSTANT)
    {
        add = compile_time_add(add->arg1, add->arg2);
        *optimized = true;
    }
    else if (add->arg1->op == OP_ADD && add->arg2->op == OP_CONSTANT)
    {
        bool found = false;

        for (Bite* b = add->arg1; !found && b->op == OP_ADD; b = b->arg1)
        {
            if (b->arg2->op == OP_CONSTANT)
            {
                add = compile_time_add(add->arg2, b->arg2);
                *optimized = true;
                break;
            }
            else if (b->arg1->op == OP_CONSTANT)
            {
                add = compile_time_add(add->arg1, b->arg2);
                *optimized = true;
                break;
            }
        }
    }

    return add;
}

Bite* fold_constants(Bite* bite)
{
    bool optimized = false;
    switch (bite->op)
    {
    case OP_CONSTANT:
    case OP_PARAMETER:
        break;

    case OP_ADD:
        do
        {
            optimized = false;
            bite->arg1 = fold_constants(bite->arg1);
            bite->arg2 = fold_constants(bite->arg2);
            bite = optimize_add(bite, &optimized);
        }
        while (optimized && bite->op == OP_ADD);
        break;

    case OP_SUB:
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
            debug("Already in use: %c%d", bite->id, bite->ver);
            return;
        }

        num++;
    }

    bite->reg = num;

    debug("Allocated: %c%d to slot %d", bite->id, bite->ver, bite->reg);

    b[num] = bite;
}

void remove_temp(Bite* bite)
{
    Bite** b = temporaries;

    while (*b)
    {
        if (*b == bite)
        {
            debug("Free: %c%d from slot %d", bite->id, bite->ver, bite->reg);

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

void analyze_liveness(Bite* bite, Bite** live_ones)
{
    Bite* variables[1024];
    Bite** ptr = variables;
    memset(variables, 0, sizeof(variables));

    // Copy to outgoing
    for (Bite** b = live_ones; *b; b++)
    {
        if (*b != bite)
        {
            *ptr++ = *b;
        }
    }

    if (bite->op != OP_CONSTANT && bite->op != OP_PARAMETER)
    {
        add_temp(bite);
    }

    print_bite_norecurse(bite);

    printf(" | { ");
    for (Bite** b = live_ones; *b; b++)
    {
        printf("%c%d%s ", (*b)->id, (*b)->ver, (*b == bite ? "+" : ""));
    }
    printf(" -> ");

    for (Bite** b = variables; *b; b++)
    {
        printf("%c%d ", (*b)->id, (*b)->ver);
    }

    printf("} ");

    for (Bite** b = temporaries; *b; b++)
    {
        printf("%s = %c%d ", reg_name((*b)->reg), (*b)->id, (*b)->ver);
    }

    printf("\n");

    switch (bite->op)
    {
    case OP_CONSTANT:
    case OP_PARAMETER:
        break;

    case OP_ADD:
    case OP_SUB:
    case OP_LESS:
    case OP_EQ:
        *ptr++ = bite->arg1;
        analyze_liveness(bite->arg1, variables);
        remove_temp(bite->arg1);

        *ptr++ = bite->arg2;
        analyze_liveness(bite->arg2, variables);
        remove_temp(bite->arg2);
        break;

    case OP_NEG:
    case OP_PTR:
        *ptr++ = bite->arg1;
        analyze_liveness(bite->arg1, variables);
        remove_temp(bite->arg1);
        break;

    case OP_IF:
        *ptr = bite->arg1;
        analyze_liveness(bite->arg1, variables);
        remove_temp(bite->arg1);

        *ptr = bite->arg2->arg1;
        analyze_liveness(bite->arg2->arg1, variables);
        remove_temp(bite->arg2->arg1);

        *ptr = bite->arg2->arg2;
        analyze_liveness(bite->arg2->arg2, variables);
        remove_temp(bite->arg2->arg2);

        assert(bite->arg2->op == OP_BRANCH);
        break;

    case OP_RECURSE:
    case OP_CALL:
        for (Bite* b = bite->arg1; b; b = b->arg2)
        {
            *ptr++ = b->arg1;
            analyze_liveness(b->arg1, variables);
        }

        for (Bite* b = bite->arg1; b; b = b->arg2)
        {
            remove_temp(b);
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
    bite_ids = 'a';
    Bite* res = bite_expr(&ptr, self, params, body);

    if (debug_on())
    {
        debug("Generated %ld bites, resulting variable is: %c%d.\n", ptr - bitecode, res->id, res->ver);
        print_bitecode(res);
    }

    res = fold_constants(res);

    if (debug_on())
    {
        debug("After constant folding");
        print_bitecode(res);
    }

    debug("ANALYZE LIVENESS START");
    memset(temporaries, 0, sizeof(temporaries));
    add_temp(res);
    Bite* live_ones[2] = {res, NULL};
    analyze_liveness(res, live_ones);
    remove_temp(res);
    debug("ANALYZE LIVENESS END");

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
