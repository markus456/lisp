FLAGS:=-g -fno-omit-frame-pointer -Wall  -Wextra -Werror -Wno-unused-parameter -std=c11
DEBUG_FLAGS=-fsanitize=address -fsanitize=undefined
RELEASE_FLAGS=-flto -O3 -DNDEBUG

all: lisp

lisp: lisp.c lisp.c compiler.c compiler.h impl/x86_64.c impl/x86_64.h
	$(CC) $(FLAGS) $(DEBUG_FLAGS) lisp.c compiler.c impl/x86_64.c -o lisp

.PHONY: release
release: lisp.c lisp.c compiler.c compiler.h impl/x86_64.c impl/x86_64.h
	$(CC) $(FLAGS) $(RELEASE_FLAGS) lisp.c compiler.c impl/x86_64.c -o lisp

.PHONY: gc_debug
gc_debug: lisp.c lisp.c compiler.c compiler.h impl/x86_64.c impl/x86_64.h
	$(CC) $(FLAGS) -fsanitize=address -fsanitize=undefined lisp.c compiler.c impl/x86_64.c -o lisp

.PHONY: test
test: lisp
	./run_tests.sh

.PHONY: clean
clean:
	rm lisp

.PHONY: demo
demo: release
	time -f 'time: %e' ./lisp -q < demos/game-of-life.lisp

game_of_life: demo

rule110: release
	./lisp -q < demos/rule110.lisp
