#ifndef INPUT_H
#define INPUT_H

#define CTRL_KEY(k) ((k) & 0x1F)

/**
 * Returns a key: an ordinary byte value, one of the editorKey enum values for
 * special keys, or -1 if the read timed out with no input.
 */
int editor_read_key(void);

/**
 * Returns 1 to keep running, 0 if the user asked to quit.
 */
int editor_process_keypress(int c);

/**
 * Modal input on the message line. `prompt` is a format string containing one
 * %s, which is replaced by what has been typed so far. Returns a malloc'd
 * string the caller owns and frees, or NULL if the user pressed Escape.
 *
 * `callback` is invoked on every keystroke with the current input and the key,
 * and may be NULL.
 */
char *editor_prompt(const char *prompt, void (*callback)(char *, int));

/* ---------------------------------------------------------------------------
 * Editor operations.
 *
 * These are the actions the command table points at. Every one takes nothing
 * and returns nothing — that uniformity is the whole reason a table is
 * possible. Anything needing input prompts for it itself.
 * ------------------------------------------------------------------------- */

void editor_move_left(void);
void editor_move_right(void);
void editor_move_up(void);
void editor_move_down(void);
void editor_page_up(void);
void editor_page_down(void);
void editor_line_start(void);
void editor_line_end(void);

void editor_insert_newline(void);
void editor_backspace(void);
void editor_delete_forward(void);
void editor_insert_text(int c);

void editor_save(void);
void editor_undo(void);
void editor_redo(void);

void editor_mouse_click(void);
void editor_wheel_up(void);
void editor_wheel_down(void);

void editor_cursor_add_above(void);
void editor_cursor_add_below(void);
void editor_cursor_add_next(void);
void editor_cursor_collapse(void);
int editor_has_extra_cursors(void);

#endif
