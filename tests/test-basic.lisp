(defun mul_impl (x n s) (if (eq n 1) (+ s) (mul_impl x (+ n -1) (+ x s))))
(defun mul (x n) (mul_impl x n x))
(defun pow (x n) (if (eq n 1) (+ x) (pow (+ x x) (+ n -1))))
(pow 2 32)
(mul 5 10)
(exit)
