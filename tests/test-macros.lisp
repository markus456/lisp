(defmacro perhaps (condition body) (list 'if condition body '()))
(define i 0)
(perhaps (eq i 0) 5)
(macroexpand perhaps ((eq i 0) 5))
