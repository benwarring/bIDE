#ifndef INPUT_H
#define INPUT_H

/**
 * Returns a key: an ordinary byte value, one of the editorKey enum values for
 * special keys, or -1 if the read timed out with no input.
 */
int editor_read_key(void);

/**
 * Returns 1 to keep running, 0 if the user asked to quit.
 */
int editor_process_keypress(int c);

#endif
