# lisp

A simple lisp interpreter.

# Building the Example

An example Game of Life program written in this Lisp language is included. To
build and run it:

```
make demo
```

# Language Features

- There are only two constants, `nil` for the empty list and `t` for the true
  value.

- Names are case-sensitive: `T` and `t` are not the same symbol and thus
  `(eq T t)` will raise an undefined symbol error.

- Tail recursion is supported for both branches of the `if` function and
  the last argument of the `progn` function.

- Symbols must be less than 1024 characters long.

- Rudimentary compilation into x86-64 bytecode is supported by the `compile`
  function. See the `Lisp Compilation` section for more information.

### Special Forms (builtin functions)

- `+`: Adds all the arguments together.

- `-`: Subtracts all the values from the first one. If given only one argument,
  negates it.

- `<`: Compares two numbers and returns `t` if the first one is less than the
  second one.

- `quote`: Quotes the arguments and returns them without evaluating.

- `cons`: Creates a cons cell (i.e. a pair).

- `car`: Gets the `car` of the cons cell (i.e. the first value of the pair).

- `cdr`: Gets the `cdr` of the cons cell  (i.e. the second value of the pair).

- `eq`: Compares two objects for equality and returns `t` if they are the same
  type and compare equal or `nil` if they don't. Only numbers and symbols can be
  compared.

- `if`: If the first argument evaluates to a non-`nil` value, the second
  argument is evaluated. Otherwise, the third argument is evaluated.

- `list`: Creates a list of all the arguments after evaluating them.

- `eval`: Evaluates list given as the first argument.

- `progn`: Evaluates all of the lists given as arguments and returns the value of the last one.

- `lambda`: Creates a lambda closure. The first argument is the formal
  parameters and the second one is the function body. Closures can refer to
  variables in the scope that they can see and their values will be captured
  when the lambda function is created.

- `define` and `defvar`: Defines the symbol given as the first argument to point
  to the second argument.

- `defun`: Defines a function. The first argument is the function name, the
  second is the argument list and the third argument is the function body.

- `freeze`: Resolves all symbols in the functions given as the arguments. This
  effectively makes them pure functions as global variables are resolved by
  this.

- `compile`: Compile all of the functions given as the arguments. The supported
  builtins that can be compiled are `+`, `-`, `<`, `eq`, `car`, `cdr` and
  `if`. Self-recursion is also supported. If the `-d` flag is used, the compiled
  code is disassembled by GDB whenever `compile` is called, make sure GDB is
  installed on your system.

- `defmacro`: Defines a macro. Macro expansion behaves similarly to function
  execution except that the arguments to the macro are not evaluated and the
  macro expansion always happens in the global scope.

- `macroexpand`: Expands the given macro without evaluating it. Useful for
  debugging macros.

- `apply`: Applies the function given as the first argument to the list given as
  the second argument.

- `print`: Prints the arguments to stdout after evaluating them.

- `write-char`: Writes the input to stdout as unsigned bytes.

- `sleep`: Sleep for the given amount of milliseconds.

- `rand`: Return a "random" number. This uses the C `rand()` function seeded to
  the current time so it's not a very reliable source of randomness.

- `load`: Loads a lisp program from a file. Useful for loading libraries. File
  names that have spaces in them cannot be loaded.

- `exit`: Exits the program immediately.

- `debug`: If the first argument is non-nil, debug mode is turned on. Only in
  debug builds.

## Lisp Compilation

Lisp functions can be compiled into x86_64 machine code with the `compile`
builtin. The compilation only works for functions that do not allocate memory
and that only call other compiled functions or are self-recursive. Both
tail-position recursion and non-tail-position recursion works but the latter
will be translated into a function call and thus it'll use up the stack space.

The order of compilation matters. The compilation of the function only succeeds
if all of the functions that it calls have already been compiled. The exception
to this is of course self-recursion which is handled separately.

The following is an example of a function that will compile:

```
(foo (a) (+ a a))
(bar (a b) (- (foo a) b))

;; foo must be compiled before bar
(compile foo)
(compile bar)
```

And here's an example that won't as the leaf function uses `cons`.

```
(push-one (a) (cons 1 a))
(dumb-add (a b) (+ (car (foo a) 2)))

;; Will fail as it uses 'cons'
(compile push-one)
;; Will fail as it uses 'push-one'
(compile dumb-add)
```

# Building

Run `make` to build a debug version and `make release` for an optimized
one.

`make test` can be used to run the test suite.
