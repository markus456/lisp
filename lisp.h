#pragma once

#define _GNU_SOURCE

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>

// This uses a similar type encoding scheme that the Ghoulom paper and Max
// Bernstein's Compiling a Lisp does:
//
//   https://bernsteinbear.com/blog/compiling-a-lisp-2/
//
// Because the garbage collection is implemented by storing a pointer at the
// beginning of all heap allocated objects and all heap allocated objects are at
// least one byte in size, the objects thus take 9 or more bytes. Aligning these
// to a 8 byte boundary gives 3 bits to store the type information and it makes
// the minimum allocations size 16 bytes.
//
// If lowest two bits are zero, the value is an integer and the numeric value of
// it can be extracted by shifting it two bits to the right. If the lowest three
// bits are all set, the value is a constant. Otherwise, it's a heap allocated
// object and the pointer to the real object can be extracted by masking off the
// lowest three bits.
//
// 0bX00 - Integer
// 0b001 - Symbol
// 0b010 - Builtin function
// 0b011 - Cons cell
// 0b101 - Lisp function
// 0b110 - Lisp macro
// 0b111 - Constant (nil or true)
//
// Nil is encoded as 0b001111 (0xf) and True as 0b011111 (0x1f).
enum Type {
    TYPE_NUMBER   = 0,
    TYPE_SYMBOL   = 1,
    TYPE_BUILTIN  = 2,
    TYPE_CELL     = 3,
    TYPE_FUNCTION = 5,
    TYPE_MACRO    = 6,
    TYPE_CONST    = 7
};

#define TYPE_MASK 0x7

struct Object;
typedef struct Object Object;
typedef struct Object* (*Function) (struct Object*, struct Object*);
struct UserFunction;
typedef struct UserFunction UserFunction;

struct UserFunction
{
    Object* func_params;
    Object* func_body;
    Object* func_env;
    void*   jit_mem; // Stores the JIT compiled code
    uint8_t compiled;
};

struct Object
{
    // The "301 Moved Permanently" pointer that's set during GC. This needs to
    // be included in the actual type as the GC needs to mark a pointer moved
    // and later accesses to it from other pointers must detect that it has been
    // moved. Initially the pointer contains only the type of the object in the
    // lowest three bits but once GC has moved, it it will contain the actual
    // address where the object was moved. Whether an object was moved can be
    // detected with: (moved & TYPE_MASK) == 0
    Object* moved;

    union
    {
        // Cons cell (TYPE_CELL)
        struct {
            Object* car;
            Object* cdr;
        };

        // Symbol (TYPE_SYMBOL)
        char name[1];

        // Builtin function (TYPE_BUILTIN)
        Function fn;

        // Custom functions (TYPE_FUNCTION)
        UserFunction ufn;

        // The special value where the return value is stashed for tail calls
        struct {
            Object* tail_expr;
            Object* tail_scope;
        };
    };
};

//
// The constant objects
//
#define Nil       ((Object*)0x0f)
#define True      ((Object*)0x1f)
#define Undefined ((Object*)0x2f) // Used as the error object in some functions
#define JitEnd    ((Object*)0x3f) // Marks the end of the JIT stack
#define JitPoison ((Object*)0x4f) // Marks unused JIT stack, debugging only

// Allocation sizes and such
#define ALLOC_ALIGN _Alignof(Object)
#define BASE_SIZE offsetof(Object, name)

// Stack variable tracking for GC
#define MAX_VARS 7

struct Frame
{
    struct Frame* next;
    int size;
    Object** vars[MAX_VARS];
};

typedef struct Frame Frame;

extern Frame* stack_top;
#define ENTER() Frame frame; frame.next = stack_top; frame.size = 0; stack_top = &frame
#define SETEND(n) frame.size = n
#define PUSH1(a) ENTER(); frame.vars[0] = &a; SETEND(1)
#define PUSH2(a, b) PUSH1(a); frame.vars[1] = &b; SETEND(2)
#define PUSH3(a, b, c) PUSH2(a, b); frame.vars[2] = &c; SETEND(3)
#define PUSH4(a, b, c, d) PUSH3(a, b, c); frame.vars[3] = &d; SETEND(4)
#define PUSH5(a, b, c, d, e) PUSH4(a, b, c, d); frame.vars[4] = &e; SETEND(5)
#define PUSH6(a, b, c, d, e, f) PUSH5(a, b, c, d, e); frame.vars[5] = &f; SETEND(6)
#define PUSH7(a, b, c, d, e, f, g) PUSH6(a, b, c, d, e, f); frame.vars[6] = &g; SETEND(7)
#define POP() stack_top = frame.next;

// Argument check macros
#define CHECK0ARGS(args) args != Nil
#define CHECK1ARGS(args) get_type(args) != TYPE_CELL || CHECK0ARGS(cdr(args))
#define CHECK2ARGS(args) get_type(args) != TYPE_CELL || CHECK1ARGS(cdr(args))
#define CHECK3ARGS(args) get_type(args) != TYPE_CELL || CHECK2ARGS(cdr(args))

int get_type(Object* obj);

// These "unmask" the pointer and return the real object. The specialized cell,
// func and builtin ones assume that the input is of the given type which is
// slightly more efficient as the members are then accessible at a constant
// offset.
Object* get_obj(Object* obj);
Object* get_cell(Object* obj);
Object* get_func(Object* obj);
Object* get_builtin(Object* obj);

const char* get_symbol(Object* obj);
const char* get_symbol_by_pointed_value(Object* val);
const char* get_type_name(enum Type type);

int64_t get_number(Object* obj);
Object* func_params(Object* obj);
Object* func_body(Object* obj);
uint8_t* func_jit_mem(Object* obj);
Object* symbol_lookup(Object* scope, Object* sym);
Object* make_ptr(Object* obj, enum Type type);
Object* make_number(int64_t val);
int64_t get_number(Object* obj);
void bind_value(Object* scope, Object* symbol, Object* value);
Object* symbol(const char* name);
Object* car(Object* obj);
Object* cdr(Object* obj);
Object* cons(Object* car, Object* cdr);
Object* eval(Object* scope, Object* obj);
int length(Object* list);

void do_writechar(Object* obj);

bool debug_on();

void print(Object* obj);
void debug_print(Object* obj);

void error(const char* fmt, ...);

void debugf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
