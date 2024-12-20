#!/bin/bash

for testcase in tests/*.lisp
do
    echo "Test: $testcase"
	./lisp -ge < "$testcase" || break
done
