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
(defvar lbracket 91)
(defvar H 72)
(defvar J 74)

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
  (progn (write-char escape)
         (write-char lbracket)
         (write-char H)))

;; Clears the screen and moves the cursor to the top left
(defun clear_screen ()
  (progn (write-char escape)
         (write-char lbracket)
         (write-char 50)
         (write-char J)
         (reset_cursor)))

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

;; Sums up three adjacent values in a row
(defun triplet (row x)
  (if (eq x 0)
      (+ (car row) (car (cdr row)) (car (cdr (cdr row))))
      (triplet (cdr row) (- x 1))))

;; Get the next state of this cell based on the sum of neighbors and 
(defun compute_cell_result (sum self)
  (if (eq self 1)
      (if (or (eq sum 2) (eq sum 3)) 1 0)
      (if (eq sum 3) 1 0)))

;; Slow approach for the corner cases
(defun get_neighbours_slow (st x y)
  (+ (get_xy st (- x 1) (- y 1))
     (get_xy st x       (- y 1))
     (get_xy st (+ x 1) (- y 1))
     (get_xy st (- x 1) y)
     (get_xy st (+ x 1) y)
     (get_xy st (- x 1) (+ y 1))
     (get_xy st x       (+ y 1))
     (get_xy st (+ x 1) (+ y 1))))

;; For cases where there's no wrapping, can be calculated by traversing the list once
(defun get_neighbours_fast (st x y)
   (if (eq y 0)
       (+ (triplet (car st) x)
          (nth (car (cdr st)) x)
          (nth (car (cdr st)) (+ x 2))
          (triplet (car (cdr (cdr st))) x))
       (get_neighbours_fast (cdr st) x (- y 1))))

;; Checks if this coordinate can be evaluated by traversing the list only once
(defun not_fast (x y x_lim y_lim)
  (if (< (- y_lim 4) y)
      t
      (if (< y 1)
          t
          (if (< (- x_lim 4) x)
              t
              (< x 1)))))

;; The values of all neighbor cells
(defun get_neighbours (st x y)
  (if (not_fast x y screen_width screen_height)
      (get_neighbours_slow st x y)
      (get_neighbours_fast st (- x 1) (- y 1))))

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
(compile triplet)
(compile compute_cell_result)
(compile get_neighbours_fast)
(compile get_neighbours_slow)
(compile not_fast)
(compile get_neighbours)
(compile compute_cell)
(compile compute_cell_state)
(compile create_start_state)
(compile main_loop)
(compile reset_cursor)
(compile clear_screen)
(compile print_row)
(compile print_state)

;; Start the program
(clear_screen)
(main_loop (create_start_state) 0)
