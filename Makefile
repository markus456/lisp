FLAGS:=-g -fno-omit-frame-pointer -Wall  -Wextra -Werror -Wno-unused-parameter -std=c11

all: lisp

release:
	$(CC) $(FLAGS) -flto -O3 -DNDEBUG lisp.c compiler.c impl/x86_64.c -o lisp

lisp: lisp.c lisp.c compiler.c compiler.h impl/x86_64.c impl/x86_64.h
	$(CC) $(FLAGS) -fsanitize=address -fsanitize=undefined lisp.c compiler.c impl/x86_64.c -o lisp

test: lisp
	./run_tests.sh

clean:
	rm lisp

demo: release
	time -f 'time: %e' ./lisp -q < demos/game-of-life.lisp

game_of_life: demo

rule110: release
	./lisp -q < demos/rule110.lisp
