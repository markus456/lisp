#!/bin/bash

for testcase in tests/*.lisp
do
    echo "Test: $testcase"
	./lisp -ge -m $((1024 * 1024 * 64)) < "$testcase" || break
done
