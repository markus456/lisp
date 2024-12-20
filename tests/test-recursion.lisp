
;; Logical OR
(defun or (a b) (if a t (if b t nil)))

(defun mul_impl (a b acc)
  (if (eq b 0)
      acc
      (mul_impl a (+ b -1) (+ acc a))))

(defun mul (a b)
  (if (< a b)
      (mul_impl b a 0)
      (mul_impl a b 0)))

;; These will fail due to lack of the tail call optimization
(mul 1 1024)
(mul 1 (mul 1024 2))
(mul 1 (mul 1024 4))
(mul 1 (mul 1024 8))
(mul 1 (mul 1024 16))
(mul 1 (mul 1024 32))
(mul 1 (mul 1024 64))
(mul 1 (mul 1024 128))
(mul 1 (mul 1024 256))
(mul 1 (mul 1024 512))
(mul 1024 (mul 1024 1024))
