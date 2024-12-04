all: lisp

release:
	cc -g -O3 -Wall -DNDEBUG -Wextra -Werror -std=c11 lisp.c -o lisp

lisp:
	cc -g -fsanitize=address -fsanitize=undefined -Wall -Wextra -Werror -std=c11 lisp.c -o lisp

test: lisp
	./lisp -e < test.txt

clean:
	rm lisp
