#ifndef UNDO_H
#define UNDO_H

/**
 * Undo is implemented entirely by recording inverse operations inside the four
 * mutation functions in buffer.c. Nothing else records, and nothing else needs
 * to know undo exists.
 *
 *   operation    inverse       record needs
 *   ---------    -------       ------------
 *   insert char  delete char   position (+ the char, for redo)
 *   delete char  insert char   position, the deleted character
 *   split line   join lines    position
 *   join lines   split line    position (the seam)
 */
#include "editor.h"

/**
 * Undo state is per-buffer: each open file has its own history, and switching
 * buffers must not let one file's undo apply to another. Created with the
 * buffer, destroyed with it. Every other function here acts on the *current*
 * buffer's state, so call sites did not have to change when this moved.
 */
UndoState *undo_create(void);
void undo_destroy(UndoState *u);

typedef enum {
    UNDO_INSERT_CHAR,
    UNDO_DELETE_CHAR,
    UNDO_SPLIT_LINE,
    UNDO_JOIN_LINES
} undo_type;

/**
 * Called by the mutation functions. Ignored while an undo or redo is in
 * progress, which is what stops undo from recording itself. Pushing a new
 * record clears the redo stack.
 */
void undo_record(undo_type type, int cy, int cx, char c);

/**
 * Explicit group markers. Everything recorded between them undoes as one
 * action. Implicit heuristics cannot express "these N edits are one action",
 * which is what deleting a selection and multi-cursor editing both need.
 */
void undo_begin_group(void);
void undo_end_group(void);

/**
 * Ends the current implicit group, so the next edit starts a new one. Call on
 * cursor movement and on save.
 */
void undo_break(void);

int undo_perform(void);
int redo_perform(void);

/**
 * Modified-state tracking by undo depth rather than a counter: the file is
 * clean when the stack is exactly as deep as it was at the last save, so
 * undoing back to the saved state correctly clears the marker.
 */
void undo_mark_saved(void);
int undo_is_dirty(void);

/**
 * True if any open buffer has unsaved changes.
 */
int undo_any_dirty(void);

#endif
