;;
;; "Standard" library: functions from various lisp implementations as well as
;; other bits and pieces.
;;

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
;; Note: Most of these are inefficient naive implementations that use the two
;; available native operations: addition and subtraction.
;;

;; Helper for the mul function, uses an accumulator argument
(defun mul_impl (a b acc)
  (if (eq b 0)
      acc
      (mul_impl a (+ b -1) (+ acc a))))

;; Multiplication of two numbers
(defun mul (a b)
  (if (< a b)
      (mul_impl b a 0)
      (mul_impl a b 0)))

;; Helper for the largest_doubling function
(defun largest-doubling_impl (a b acc)
   (if (< a (+ b b))
      (cons b acc)
      (largest-doubling_impl a (+ b b) (+ acc acc))))

;; Returns a cons cell with the largest multiple of b that is less than or equal
;; to a. The value itself is in car and the multiplier is in cdr.
(defun largest-doubling (a b)
  (if (< a b)
      (cons b 0)
      (largest-doubling_impl a b 1)))

;; Helper for the div function, uses an accumulator argument
(defun div_impl (a b acc ld)
  (if (< a b)
      acc
      (div_impl (- a (car ld)) b (+ acc (cdr ld)) (largest-doubling (- a (car ld)) b))))

;; Division of two numbers, very naive implementation
(defun div (a b)
  (div_impl a b 0 (largest-doubling a b)))

;; Helper for the pow function
(defun pow_impl (a b acc)
  (if (eq b 0)
      acc
      (pow_impl a (- b 1) (mul acc a))))

;; Raises the first number to the power of the second
(defun pow (a b)
  (if (eq b 0)
      1
      (if (eq b 1)
          (a)
          (pow_impl a (- b 1) a))))

;; Helper for the mod function, finds the largest doubling of b that's less than a
(defun mod_impl (a b)
  (if (< a (+ b b))
      b
      (mod_impl a (+ b b))))

;; Modulo, remainder of a divided by b
(defun mod (a b)
  (if (< a b)
      a
      (mod (- a (mod_impl a b)) b)))

;;
;; List functions
;;

;; List length
(defun length(lst) (if (eq lst nil) 0 (+ 1 (length (cdr lst)))))

;; List append
(defun append(lst a) (if (eq lst nil) (cons a nil) (cons (car lst) (append (cdr lst) a))))

;; Map funtion to a list
(defun mapcar (f lst) (if (eq lst nil) nil (cons (apply f (list (car lst))) (mapcar f (cdr lst)))))

;; Get the Nth value in a list
(defun nth (a n) (if (< n 1) (car a) (nth (cdr a) (- n 1))))

;; Fill a list with values
(defun fill (n v) (fill_impl n v nil))
(defun fill_impl (n v acc)
  (if (< n 1) acc (fill_impl (- n 1) v (cons v acc))))

;; Generate a list of values by repeatedly calling a function
(defun generate (n v) (generate_impl n v nil))
(defun generate_impl (n v acc)
  (if (< n 1) acc (generate_impl (- n 1) v (cons (v) acc))))

;;
;; Utility functions
;;

;; Repeatedly evaluates a function
(defun loop (x) (if (x) (loop x) (loop x)))

;;
;; IO helpers
;;

;; Writes a list of numbers as unsigned bytes to stdout
(defun write-list (lst) (progn (mapcar write-char lst) nil))

;; Writes a list of numbers in base ten (i.e. pretty-printed) to stdout
(defun write-number (n)
  (if (< n 10) (write-char (+ n 48)) (progn (write-number (div n 10)) (write-char (+ (mod n 10) 48)))))

;; Compile all of the declared functions. This prevents the implementations and
;; builtins used by them from being overridden by the calling code.
(compile not and or xor mul_impl mul largest-doubling_impl largest-doubling
         div_impl div pow_impl pow mod_impl mod length append mapcar nth
         fill fill_impl generate generate_impl loop write-list write-number)
