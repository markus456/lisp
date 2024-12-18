
;; Logical OR
(defun or (a b) (if a t (if b t nil)))

(defun mul (a b acc)
  (if (or (eq b 0) (eq a 0))
      acc
      (mul a (+ b -1) (+ acc a))))

;; These will fail due to lack of the tail call optimization
(mul 1 1024 0)
(mul 1 (mul 1024 2 0) 0)
(mul 1 (mul 1024 4 0) 0)
(mul 1 (mul 1024 8 0) 0)
(mul 1 (mul 1024 16 0) 0)
(mul 1 (mul 1024 32 0) 0)
(mul 1 (mul 1024 64 0) 0)
