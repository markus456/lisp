(define foobar (lambda (x) (lambda (y) (x y))))
((foobar (lambda (z) (+ z 1))) 5)
