all: lisp

release:
	cc -fno-omit-frame-pointer -g -O3 -Wall -DNDEBUG -Wextra -Werror -std=c11 lisp.c -o lisp

lisp: lisp.c
	cc -fno-omit-frame-pointer -g -fsanitize=address -fsanitize=undefined -Wall -Wextra -Werror -std=c11 lisp.c -o lisp

test: lisp
	./run_tests.sh

clean:
	rm lisp

demo: release
	./lisp -q < game-of-life.lisp
