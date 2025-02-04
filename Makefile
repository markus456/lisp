all: lisp

release:
	cc -flto -fno-omit-frame-pointer -g -O3 -Wall -DNDEBUG -Wextra -Werror -std=c11 lisp.c compiler.c impl/x86_64.c -o lisp

lisp: lisp.c lisp.c compiler.c compiler.h impl/x86_64.c impl/x86_64.h
	cc -fno-omit-frame-pointer -g -fsanitize=address -fsanitize=undefined -Wall -Wextra -Werror -std=c11 -Wl,-E lisp.c compiler.c impl/x86_64.c -o lisp

test: lisp
	./run_tests.sh

clean:
	rm lisp

demo: release
	./lisp -q < game-of-life.lisp
