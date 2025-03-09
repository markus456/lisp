(load std.lisp)
(defvar width 256)
(defvar max_iter 4096)

(defun print_state (state)
  (if state
      (progn
      (if (eq (car state) 0)
          (write-char 32) ;; Dead cell, write a space
          (write-char 226 150 136)) ;; Live cell, write a Unicode full block
      (print_state (cdr state))) ;; Recurse
      (write-char 10))) ;; End of line, write a newline

;; Current pattern 111 110 101 100 011 010 001 000
;; New state        0   1   1   0   1   1   1   0
(defun next_cell_state (left mid rest)
  (if (eq left 1)
      (if (eq mid 1)
          (if (eq (car rest) 1)
              0
              1)
          (car rest))
      (if (eq mid 1)
          1
          (car rest)
      )))


(defun next_state (state left mid rest)
  (if rest
      (cons (next_cell_state left mid rest) (next_state state mid (car rest) (cdr rest)))
      (cons mid nil)))

(defun main_loop (state iter)
  (progn (print_state state)
         (sleep 25)
         (if (< iter max_iter)
             (main_loop (cons (car state) (next_state state (car state) (car (cdr state)) (cdr (cdr state)))) (+ iter 1))
             nil)))

(compile main_loop next_state next_cell_state print_state)

(main_loop (append (fill width 0) 1) 0)
