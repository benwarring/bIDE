#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>

/**
 * A list overlay drawn on top of the text area: a query line plus a scrollable
 * list of candidates. The list widget fills this in and hands it to the
 * renderer, which draws it *inside* the normal frame construction so the whole
 * screen still leaves in a single write. Drawing it as its own write would
 * reintroduce exactly the flicker the append buffer exists to prevent.
 */
typedef struct {
    int active;
    const char *title;
    const char *query;
    char **items;               // display strings, already filtered by the caller
    int nitems;
    int selected;               // index into items
    int top;                    // first visible item, for scrolling
    int height;                 // rows of items to draw
} Overlay;

void render_set_overlay(Overlay *o);

/**
 * A box anchored to a screen cell, floating *over* the text instead of replacing
 * rows of it, with the real cursor left in the buffer where the user put it.
 *
 * The overlay above owns whole rows because it owns the keyboard too. This one
 * appears while the user is still typing into the text underneath, so it has to
 * cover as little as it can and must not move the cursor.
 */
typedef struct {
    int active;
    char **items;               // display strings, already filtered by the caller
    int nitems;
    int selected;
    int top;                    // first visible item, for scrolling
    int height;                 // rows of items to draw
    int width;
    int row, col;               // the screen cell it hangs from, 0-indexed
} Popup;

void render_set_popup(Popup *p);

void editor_scroll(void);

void editor_refresh_screen(void);

/**
 * The frame editor_refresh_screen would write, as a malloc'd buffer the caller
 * frees, without writing it. The startup effect needs the first frame in hand
 * before it is allowed onto the screen.
 */
char *render_frame(size_t *len);

void editor_set_status_message(const char *fmt, ...);

/**
 * Marks the screen stale.
 *
 * The loop used to render every iteration, which was free when the only way to
 * reach the top of it was a keypress. Once poll wakes on a timer or a server
 * message it is no longer free, and redrawing on every wake makes the cursor
 * visibly flicker. So: anything that changes what the screen should show says
 * so, and the loop renders only when something has.
 */
void editor_request_redraw(void);

/** Returns whether a redraw is due, clearing the flag. */
int editor_take_redraw(void);

/**
 * Milliseconds until the screen needs redrawing on its own with no input at all,
 * or -1 if never. Only the status message expiring qualifies: it disappears
 * after a timeout, which nothing else would wake the loop to notice.
 */
int render_next_timeout_ms(void);

void render_free(void);

#endif
