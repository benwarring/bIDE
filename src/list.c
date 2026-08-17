#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "list.h"
#include "editor.h"
#include "fuzzy.h"
#include "input.h"
#include "popup.h"
#include "render.h"
#include "terminal.h"
#include "wrappers.h"

#define LIST_MAX_HEIGHT 12

typedef struct {
    int index;                  // into the caller's array
    int score;
} Candidate;

// filtered view of the caller's list, rebuilt on every keystroke
static Candidate *cands = NULL;
static int ncands = 0;
static int cands_cap = 0;

static char **display = NULL;   // borrowed pointers, in filtered order
static int display_cap = 0;

static void reserve(int n) {
    if (n <= cands_cap) return;
    cands_cap = n;
    cands = Realloc(cands, sizeof(Candidate) * (size_t)n);

    display_cap = n;
    display = Realloc(display, sizeof(char *) * (size_t)n);
}

static int by_score(const void *a, const void *b) {
    const Candidate *x = a, *y = b;
    if (x->score != y->score) return y->score - x->score;
    return x->index - y->index;                         // stable for equal scores
}

static void refilter(const char *query, char **items, int nitems) {
    reserve(nitems);
    ncands = 0;

    for (int i = 0; i < nitems; i++) {
        int score;
        if (fuzzy_match(query, items[i], &score)) {
            cands[ncands].index = i;
            cands[ncands].score = score;
            ncands++;
        }
    }

    qsort(cands, (size_t)ncands, sizeof(Candidate), by_score);
    for (int i = 0; i < ncands; i++) display[i] = items[cands[i].index];
}

int list_select(const char *title, char **items, int nitems) {
    popup_close();                                      // a modal box supersedes the floating one
    char query[256];
    size_t qlen = 0;
    query[0] = '\0';

    int selected = 0;
    int top = 0;

    Overlay ov;
    ov.active = 1;
    ov.title = title;
    ov.items = NULL;
    ov.nitems = 0;
    ov.selected = 0;
    ov.top = 0;

    refilter(query, items, nitems);

    while (1) {
        if (resize_pending) {
            resize_pending = 0;
            update_window_size();
        }

        int height = ncands < LIST_MAX_HEIGHT ? ncands : LIST_MAX_HEIGHT;
        if (height > E.screenrows - 1) height = E.screenrows - 1;
        if (height < 1) height = 1;

        // same clamping as buffer scrolling, one level down
        if (selected < 0) selected = 0;
        if (selected >= ncands) selected = ncands - 1;
        if (selected < 0) selected = 0;
        if (selected < top) top = selected;
        if (selected >= top + height) top = selected - height + 1;
        if (top < 0) top = 0;

        ov.query = query;
        ov.items = display;
        ov.nitems = ncands;
        ov.selected = selected;
        ov.top = top;
        ov.height = height;

        render_set_overlay(&ov);
        editor_refresh_screen();

        int c = editor_read_key();
        if (c == -1) continue;

        if (c == '\x1b') {
            render_set_overlay(NULL);
            return -1;
        }
        if (c == '\r') {
            render_set_overlay(NULL);
            return ncands > 0 ? cands[selected].index : -1;
        }
        if (c == ARROW_UP || c == WHEEL_UP) {
            selected--;
            continue;
        }
        if (c == ARROW_DOWN || c == WHEEL_DOWN) {
            selected++;
            continue;
        }
        if (c == BACKSPACE || c == CTRL_KEY('h') || c == DEL_KEY) {
            if (qlen > 0) query[--qlen] = '\0';
            refilter(query, items, nitems);
            selected = 0;
            top = 0;
            continue;
        }
        if (!iscntrl(c) && c < 128 && qlen < sizeof(query) - 1) {
            query[qlen++] = (char)c;
            query[qlen] = '\0';
            refilter(query, items, nitems);
            selected = 0;
            top = 0;
        }
    }
}

void list_message(const char *title, char **lines, int nlines) {
    if (nlines <= 0) return;
    popup_close();

    int top = 0;

    Overlay ov;
    memset(&ov, 0, sizeof(ov));
    ov.active = 1;
    ov.title = title;
    ov.query = "";                                      // nothing is being typed into this one
    ov.items = lines;
    ov.nitems = nlines;
    ov.selected = -1;                                   // no selection: this is text, not a menu

    while (1) {
        if (resize_pending) {
            resize_pending = 0;
            update_window_size();
        }

        int height = nlines < LIST_MAX_HEIGHT ? nlines : LIST_MAX_HEIGHT;
        if (height > E.screenrows - 1) height = E.screenrows - 1;
        if (height < 1) height = 1;

        if (top > nlines - height) top = nlines - height;
        if (top < 0) top = 0;

        ov.top = top;
        ov.height = height;

        render_set_overlay(&ov);
        editor_refresh_screen();

        int c = editor_read_key();
        if (c == -1) continue;

        if (c == ARROW_DOWN || c == WHEEL_DOWN) { top++; continue; }
        if (c == ARROW_UP || c == WHEEL_UP)     { top--; continue; }
        if (c == PAGE_DOWN) { top += height; continue; }
        if (c == PAGE_UP)   { top -= height; continue; }

        break;                                          // any other key dismisses it
    }

    render_set_overlay(NULL);
}

void list_free(void) {
    free(cands);
    cands = NULL;
    ncands = 0;
    cands_cap = 0;

    free(display);
    display = NULL;
    display_cap = 0;
}
