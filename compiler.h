#pragma once

#include "lisp.h"

//
// Compilation related defines
//

#define COMPILE_SYMBOLS 1
#define COMPILE_CODE    2

#define JIT_STACK_SIZE (4096 / sizeof(Object*))

Object* jit_eval(Object* fn, Object* args);

void jit_resolve_symbols(Object* scope, Object* args);
void jit_compile(Object* scope, Object* args);

// Returns a NULL pointer if there's no JIT call in progress
Object** jit_stack();
void jit_stack_set_size(size_t size);

void jit_free();
