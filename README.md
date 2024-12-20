# lisp

A simple lisp interpreter

# Language Features

- There are only two constants, `nil` for the empty list and `t` for the true
  value.

- Names are case-sensitive: `T` and `t` are not the same symbol and thus
  `(eq T t)` will raise an undefined symbol error.

- Tail recursion is supported for the `if` function.

### Special Forms (builtin functions)

- `+`: Adds all the arguments together.

- `-`: Subtracts all the arguments, can be used to negate numbers.

- `<`: Compares two numbers and returns `t` if the first one is less than the
  second one.

- `quote`: Quotes the arguments and returns them without evaluating.

- `cons`: Creates a cons cell (i.e. a pair).

- `car`: Gets the `car` of the cons cell (i.e. the first value of the pair).

- `cdr`: Gets the `cdr` of the cons cell  (i.e. the second value of the pair).

- `eq`: Compares two objects for equality and returns `t` if they are the same
  type and compare equal or `nil` if they don't. Only numbers and symbols can be
  compared.

- `if`: If the first argument evalues to a non-`nil` value, the second argument
  is evaluated. Otherwise, the third argument is evaluated.

- `list`: Creates a list of all the arguments after evaluating them.

- `eval`: Evaluates list given as the first argument.

- `progn`: Evaluates all of the lists given as arguments and returns the value of the last one.

- `lambda`: Creates a lambda closure. The first argument is the formal
  parameters and the second one is the function body. Closures can refer to
  variables in the scope that they can see and their values will be captured
  when the lambda function is created.

- `define`: Defines the symbol given as the first argument to point to the second argument.

- `defun`: Defines a function. The first argument is the function name, the
  second is the argument list and the third argument is the function body.

- `defmacro`: Defines a macro. Macro expansion behaves similarly to function
  execution except that the arguments to the macro are not evaluated and the
  macro expansion always happens in the global scope.

- `macroexpand`: Expands the given macro without evaluating it. Useful for
  debugging macros.

- `apply`: Applies the function given as the first argument to the list given as
  the second argument.

- `print`: Prints the arguments to stdout after evaluating them.

- `load`: Loads a lisp program from a file. Useful for loading libraries. File

- names that have spaces in them cannot be loaded.

- `exit`: Exits the program immediately.

# Building

Run `make` to build a debug version and `make release` for an optimized
one.

`make test` can be used to run the test suite.
