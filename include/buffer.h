#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>

#include "editor.h"
#include "undo.h"               // undo_type: the four mutations are the four operations

/**
 * Called by each mutation with the operation it just applied, so positions held
 * outside the buffer can be shifted to stay valid.
 *
 * Applying multi-cursor edits last-position-first keeps every *edit* position
 * valid as it goes, but not the cursors already edited: an insert at an earlier
 * point pushes everything after it one byte right. The mutations report what
 * they did rather than leaving the caller to infer it, because one keystroke is
 * not one mutation — an auto-closed bracket is two inserts, and a backspace is
 * a delete or a join depending on where it lands.
 *
 * `cx` is the column for insert, delete and split, and the seam for join.
 * Passing NULL clears the hook; it is set only for the duration of one
 * multi-cursor edit, which is what keeps undo and redo from firing it.
 */
typedef void (*buffer_hook_fn)(undo_type op, int cy, int cx, void *ctx);

void buffer_set_hook(buffer_hook_fn fn, void *ctx);

/**
 * Opens a file into a new buffer and makes it active. If that file is already
 * open (compared by resolved path), switches to the existing buffer instead of
 * opening a divergent second copy. Returns the buffer index.
 */
int buffer_open(const char *filename);

int buffer_count(void);
int buffer_active(void);
const char *buffer_name(int idx);
int buffer_is_dirty(int idx);

void buffer_switch(int idx);
void buffer_cycle(int delta);

/**
 * Frees everything the buffer owns and drops it from the list. Closing the last
 * buffer leaves a fresh empty one, so the active index is never dangling.
 */
void buffer_close(int idx);

void buffer_free(void);

/**
 * Sets the name save writes to, copying the string. Used by save-as.
 */
void buffer_set_filename(const char *filename);

/**
 * Writes the buffer back to E.filename, restoring the file's original line
 * endings. Returns the number of bytes written, or -1 on failure (in which case
 * errno is set and the dirty counter is left alone).
 */
long buffer_save(void);

int buffer_num_lines(void);

int buffer_line_length(int idx);

int buffer_line_rlength(int idx);

/**
 * Copies line `idx` into the caller's buffer, NUL-terminated and truncated to
 * fit. Returns the number of bytes written, or -1 if `idx` is out of range.
 *
 * Deliberately materializes rather than handing back a pointer into internal
 * storage: a piece table could not honor a borrowed pointer, so establishing
 * the copying signature now keeps the later swap a swap and not a rewrite.
 */
int buffer_get_line(int idx, char *dst, size_t dstsize);

/* ------------------------------------------------------------------------
 * The same reads, against a named buffer rather than the active one.
 *
 * The language server talks about documents that are not necessarily the one
 * on screen — a diagnostic arrives for whichever file it concerns, and a
 * didChange has to serialize a buffer the user has already switched away from.
 * These keep that code out of the row array just as the active-buffer versions
 * do.
 * ------------------------------------------------------------------------ */

int buffer_num_lines_of(const Buffer *b);

int buffer_get_line_of(const Buffer *b, int idx, char *dst, size_t dstsize);

/**
 * The whole buffer as one NUL-terminated string with '\n' terminators, which is
 * the form the protocol wants regardless of the file's own line endings. Caller
 * frees. `len` receives the byte length.
 */
char *buffer_text_of(const Buffer *b, size_t *len);

/**
 * Same, but the tab-expanded render string. This is what the renderer draws.
 */
int buffer_get_rline(int idx, char *dst, size_t dstsize);

/**
 * Copies the highlight array for line `idx`, parallel to its render string.
 */
int buffer_get_hl(int idx, unsigned char *dst, size_t dstsize);

/**
 * Overwrites a row's highlight array wholesale. Used to restore what search
 * saved before it painted a match.
 */
void buffer_set_hl(int idx, const unsigned char *src, int len);

/**
 * Paints HL_MATCH over a range of rendered columns.
 */
void buffer_highlight_match(int idx, int rstart, int rlen);

/**
 * The byte at (cy, cx), or -1 if that position is past the end of the line or
 * out of range. Used to look at what the cursor is sitting on.
 */
int buffer_char_at(int cy, int cx);

int buffer_cx_to_rx(int idx, int cx);

int buffer_rx_to_cx(int idx, int rx);

/* ------------------------------------------------------------------------
 * The mutation API.
 *
 * These four functions are the ONLY code permitted to touch a line's bytes or
 * the row array. Not the input handler, not the renderer, not a convenience
 * helper. Every editing action decomposes into them: typing is insert, Enter is
 * split, Backspace is delete-or-join, paste is a run of inserts.
 *
 * Two constraints that look like ceremony and are not:
 *
 *   1. Position is a parameter, never a global read. With one cursor that
 *      changes nothing; with N cursors in a later milestone it is the
 *      difference between a loop and a rewrite.
 *   2. They return the resulting position, so "where does the cursor end up"
 *      lives in one place and cannot drift between call sites. In particular
 *      join_lines captures the seam before appending, which is the bug the
 *      caller would otherwise write.
 *
 * Failure policy: allocation failure calls die() inside the Malloc/Realloc
 * wrappers. These signatures stay simple; there is no status code to
 * propagate for an event that does not happen on a modern system.
 *
 * Each of the four records an inverse operation for undo.
 * ------------------------------------------------------------------------ */

bpos buffer_insert_char(int cy, int cx, int c);

bpos buffer_delete_char(int cy, int cx);

bpos buffer_split_line(int cy, int cx);

bpos buffer_join_lines(int cy);

#endif
