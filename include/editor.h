#ifndef EDITOR_H
#define EDITOR_H

#include <time.h>

#define TAB_STOP 4              // no config file by design: this is the configuration

/* Gutter layout: a right-aligned number, then " | ", so the numbers sit clear of
 * the text instead of crowding it. GUTTER_SEP_W is the width of that separator.
 *
 * The gutter is wide enough for the line count but never narrower than the
 * floor. Without a floor, crossing from 99 to 100 lines shifts the whole text
 * area one column right mid-typing, which is disorienting. A floor of 6 fits
 * three digits plus the separator, so every file under 1000 lines renders at the
 * same width and never shifts. */
#define GUTTER_SEP_W 3
#define GUTTER_MIN 6

#define MAX_EXTRA_CURSORS 256

typedef struct erow {
    int size;
    char *chars;                // raw bytes, what every non-render path works on
    int rsize;
    char *render;               // chars with tabs expanded, what the renderer draws
    unsigned char *hl;          // one highlight type per rendered character
    int hl_open_comment;        // ends inside a block comment: the next line's starting state
} erow;

/**
 * A position in the buffer: line index and byte index within that line.
 */
typedef struct {
    int cy;
    int cx;
} bpos;

typedef struct UndoState UndoState;         // opaque, owned by undo.c
typedef struct LspDoc LspDoc;               // opaque, owned by lsp.c
struct editorSyntax;                        // defined in syntax.h

/**
 * Per-file state. Everything here describes the contents of a file, and would be
 * shared by every view looking at it.
 */
typedef struct {
    erow *row;
    int numrows;
    char *filename;             // as the user typed it, for display
    char *path;                 // realpath, for deciding whether a file is already open
    int crlf;                   // the file's original line ending, restored on save
    const struct editorSyntax *syntax;
    UndoState *undo;
    LspDoc *lsp;                // server-side view of this file: version, diagnostics
    bpos last;                  // where the last view to leave this file had its cursor,
    int last_rowoff;            // and its scroll, so coming back lands where you left off
} Buffer;

/**
 * Per-view state: which buffer, where the cursor is, where the window is
 * scrolled to. One per pane.
 *
 * Cursor lives here rather than in Buffer because two panes onto the *same*
 * file each need their own cursor and scroll offset.
 *
 * (cy, cx) is the primary cursor: the one scrolling follows and the one that
 * survives collapsing. `extra` holds any additional cursors. A selection runs
 * from `anchor` to the primary cursor, and only exists with no extras.
 */
typedef struct {
    Buffer *buf;
    int cx, cy;
    int rx;                     // cursor's rendered column, with tabs expanded
    int rowoff, coloff;
    bpos *extra;
    int nextra;
    bpos anchor;                // where the selection started, while `selecting`
    int selecting;
    int top, left;              // the pane's text area on screen, 0-indexed, laid out each render;
    int rows, cols;             // its gutter is inside, its status bar just below
    int gutter;                 // width of the line number column, recomputed each render
} View;

typedef struct {
    int screenrows, screencols; // a lone pane's text area: the screen less its status bar and the message line

    Buffer **bufs;
    int nbufs;
    View *view;                 // the focused pane's; pane.c owns every view

    char statusmsg[128];
    time_t statusmsg_time;
} EditorConfig;

extern EditorConfig E;

View *editor_view(void);
Buffer *editor_buffer(void);

void editor_init(void);

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    CTRL_ARROW_UP,
    CTRL_ARROW_DOWN,
    CTRL_ARROW_LEFT,
    CTRL_ARROW_RIGHT,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    MOUSE_CLICK,
    MOUSE_DRAG,                 // moved with the left button held
    WHEEL_UP,
    WHEEL_DOWN,
    PASTE,                      // a bracketed paste arrived; its text is waiting in input.c
    CTRL_SHIFT_A                // through CTRL_SHIFT_A + 25; keep this last
};

/* Ctrl-Shift-letter has no byte of its own; it only exists when the terminal
 * reports it as an escape sequence (see editor_read_key). */
#define CTRL_SHIFT_KEY(k) (CTRL_SHIFT_A + (((k) & 0x1F) - 1))

#endif
