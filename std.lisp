;; "Standard" library functions from various lisp implementations


;;
;; Logical functions
;;

;; Logical NOT
(defun not (a) (if a nil t))

;; Logical AND
(defun and (a b) (if a (if b t nil) nil))

;; Logical OR
(defun or (a b) (if a t (if b t nil)))

;; Logical XOR
(defun xor (a b) (if a (if b nil t) (if b t nil)))

;;
;; Mathematical functions
;;

;; Multiplication of two numbers
(defun mul (a b)
  (if (or (eq a 0) (eq b 0))
      0
      (mul (+ a a) (- b 1))))

;;
;; List functions
;;

;; List length
(defun length(lst) (if (eq lst nil) 0 (+ 1 (length (cdr lst)))))

;; List append
(defun append(lst a) (if (eq lst nil) (cons a nil) (cons (car lst) (append (cdr lst) a))))

;; Map funtion to a list
(defun mapcar (f lst) (if (eq lst nil) nil (cons (apply f (list (car lst))) (mapcar f (cdr lst)))))

