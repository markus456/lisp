;; Testing Church encodings of natural numbers.
;; The functions are based on the ones found here:
;;
;;   https://www.sfu.ca/~tjd/383summer2019/scheme-ChurchEncodings.html
;;
(defun zero (f) (lambda (x) x))
(defun one (f) (lambda (x) (f x)))

(defun successor (n)
  (lambda (f)
    (lambda (x)
      (f ((n f) x)))))

(defun sum (n)
  (lambda (m)
    (lambda (f)
      (lambda (x)
        ((m f) ((n f) x))))))

(defun mul (n)
  (lambda (m)
    ((m (sum n)) zero)))

(defun pow (a)
  (lambda (b)
    (b a)))

(defun printc (c) ((c (lambda (x) (+ x 1))) 0))

(define two (successor one))
(define three ((sum two) one))
(define four ((mul two) two))
(define five ((sum four) one))
(define six ((mul three) two))
(define seven (successor six))
(define eight ((pow two) three))
(define nine ((sum ((pow two) three)) one))
(define ten ((sum five) five))

(printc zero)
(printc one)
(printc two)
(printc three)
(printc four)
(printc five)
(printc six)
(printc seven)
(printc eight)
(printc nine)
(printc ten)

(printc ((mul ten) ten))
(printc ((pow ten) five))
(printc ((pow two) ((mul ten) two)))
