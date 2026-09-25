#ifndef PANE_H
#define PANE_H

#include "editor.h"

/**
 * Panes, tmux style: any pane splits side by side or top and bottom, and splits
 * nest. They form a binary tree whose leaves each hold one View. The focused
 * one is E.view, which is all the rest of the editor ever looks at, so nothing
 * outside this file and the renderer knows there is more than one.
 */

/** The single pane everything starts in. Must run before the first buffer opens. */
void pane_init(void);

/**
 * Splits the focused pane in two and focuses the new half (right, or below),
 * which starts as a second view onto the same buffer at the same place.
 */
void pane_split(int side_by_side);

/** Closes the focused pane. Its buffer stays open. */
void pane_close(void);

/** Focuses the pane beside this one, level with the cursor, if there is one. */
void pane_focus_dir(int drow, int dcol);

void pane_focus(View *v);

/**
 * Every pane's view, in screen order, each with its rectangle freshly laid out
 * from the terminal size. Laying out on every call is cheap and means no caller
 * ever sees rectangles from before a resize. Valid until the next split or close.
 */
View **pane_views(int *n);

/** The view whose pane covers screen cell (row, col), 0-indexed, status bar and
 * divider included, or NULL for the message line. */
View *pane_at(int row, int col);

void pane_free(void);

#endif
