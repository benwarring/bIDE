#ifndef RENDER_H
#define RENDER_H

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

void editor_scroll(void);

void editor_refresh_screen(void);

void editor_set_status_message(const char *fmt, ...);

void render_free(void);

#endif
