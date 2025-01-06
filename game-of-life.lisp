(load std.lisp)
;; Configuration
(defvar iterations 1000)
(defvar screen_width 100)
(defvar screen_height 50)

;; Constants
(defvar newline 10)
(defvar alive 48)
(defvar dead 32)
(defvar escape 27)

;;
;; Printing of game state
;;

;; Prints one row
(defun print_row (lst)
  (if (eq lst nil)
      (write-char newline)
      (progn (write-char (if (eq (car lst) 0) dead alive))
             (print_row (cdr lst)))))

;; Prints the game state
(defun print_state (lst)
  (if (eq lst nil)
      nil
      (progn (print_row (car lst))
             (print_state (cdr lst)))))

;; Moves the cursor to the top left of the screen
(defun reset_cursor ()
  (write-list '(escape '[ 'H)))

;; Clears the screen and moves the cursor to the top left
(defun clear_screen ()
  (progn (write-list '(escape '[ 50 'J))
         (reset_cursor)))

;; (defun reset_cursor ()
;;   (progn (write-char escape)
;;          (write-char '[)
;;          (write-number (+ screen_height 1))
;;          (write-char 'F)
;;          (write-char escape)
;;          (write-char '[)
;;          (write-char 'J)))

;;
;; Game state computation
;;

;; Wraps x around limit
(defun wrap (x limit)
  (if (< x limit)
      (if (< -1 x)
          x
          (+ x limit))
      (- x limit)))

;; Get value at coordinates
(defun get_xy(grid x y)
  (nth (nth grid (wrap y screen_height)) (wrap x screen_width)))

;; Get the next state of this cell based on the sum of neighbors and 
(defun compute_cell_result (sum self)
  (if (eq self 1)
      (if (or (eq sum 2) (eq sum 3)) 1 0)
      (if (eq sum 3) 1 0)))

;; The values of all neighbor cells
(defun get_neighbours (st x y)
  (+ (get_xy st (- x 1) (- y 1))
     (get_xy st x       (- y 1))
     (get_xy st (+ x 1) (- y 1))
     (get_xy st (- x 1) y)
     (get_xy st (+ x 1) y)
     (get_xy st (- x 1) (+ y 1))
     (get_xy st x       (+ y 1))
     (get_xy st (+ x 1) (+ y 1))))

;; Main cell state computation
(defun compute_cell (st x y)
  (compute_cell_result (get_neighbours st x y)
                       (get_xy st x y)))

(defun compute_cell_state (st next_st row x y)
  (if (eq y -1)
      next_st
      (if (eq x -1)
          (compute_cell_state st (cons row next_st) nil (- screen_width 1) (- y 1))
          (compute_cell_state st next_st (cons (compute_cell st x y) row) (- x 1) y))))

;; Creates the initial random state
(defun create_start_state ()
  (generate screen_height
            (lambda () (generate screen_width
                                 (lambda () (if (eq (mod (rand) 2) 0) 0 1))))))

;; The main loop, evaluates the next state and prints the current one
(defun main_loop (state iter)
  (if (eq iter iterations)
      (exit)
      (progn (reset_cursor)
             (print 'iteration: iter)
             (write-char newline)
             (print_state state)
             (main_loop (compute_cell_state state nil nil (- screen_width 1) (- screen_height 1)) (+ iter 1)))))

;; Compile all of the functions

(compile wrap)
(compile get_xy)
(compile compute_cell_result)
(compile get_neighbours)
(compile compute_cell)
(compile compute_cell_state)
(compile create_start_state)
(compile main_loop)
(compile clear_screen)
(compile reset_cursor)
(compile print_state)
(compile print_row)

;; Start the program
(clear_screen)
(main_loop (create_start_state) 0)
