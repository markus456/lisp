#!/bin/bash

for testcase in tests/*.lisp
do
    echo "Test: $testcase"
	./lisp -e < "$testcase" || break
done
