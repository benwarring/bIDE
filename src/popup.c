#include <stdlib.h>
#include <string.h>

#include "popup.h"
#include "buffer.h"
#include "editor.h"
#include "fuzzy.h"
#include "input.h"
#include "render.h"
#include "wrappers.h"

#define POPUP_MAX_HEIGHT 10
#define POPUP_MAX_WIDTH  60
#define POPUP_MIN_WIDTH  16

typedef struct {
    int index;                  // into the caller's array
    int score;
} Candidate;

static char **all = NULL;       // borrowed from the caller
static int nall = 0;

static Candidate *cands = NULL;
static int ncands = 0;
static int cands_cap = 0;

static char **display = NULL;   // borrowed pointers, in filtered order
static int display_cap = 0;

static int selected = 0;
static int top = 0;
static int anchor_cy = 0, anchor_cx = 0;
static int active = 0;

static Popup pv;

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
    return x->index - y->index;                         // the server's own order breaks ties
}

/**
 * Recomputes where the box sits and hands it to the renderer. The anchor is a
 * buffer position, so this has to go through the same rx conversion and the same
 * scroll offsets the text does, or the box drifts away from the word it belongs
 * to the moment a line contains a tab.
 */
static void sync_render(void) {
    if (!active || ncands == 0) {
        render_set_popup(NULL);
        return;
    }

    int height = ncands < POPUP_MAX_HEIGHT ? ncands : POPUP_MAX_HEIGHT;

    if (selected < 0) selected = 0;
    if (selected >= ncands) selected = ncands - 1;
    if (selected < top) top = selected;
    if (selected >= top + height) top = selected - height + 1;
    if (top < 0) top = 0;

    int width = POPUP_MIN_WIDTH;
    for (int i = 0; i < ncands; i++) {
        int len = (int)strlen(display[i]) + 2;           // the leading pad, and one after
        if (len > width) width = len;
    }
    if (width > POPUP_MAX_WIDTH) width = POPUP_MAX_WIDTH;

    View *v = editor_view();

    memset(&pv, 0, sizeof(pv));
    pv.active = 1;
    pv.items = display;
    pv.nitems = ncands;
    pv.selected = selected;
    pv.top = top;
    pv.height = height;
    pv.width = width;
    pv.row = v->top + anchor_cy - v->rowoff;
    pv.col = v->left + v->gutter + buffer_cx_to_rx(anchor_cy, anchor_cx) - v->coloff;

    render_set_popup(&pv);
}

void popup_open(char **items, int nitems, int cy, int cx) {
    all = items;
    nall = nitems;
    anchor_cy = cy;
    anchor_cx = cx;
    selected = 0;
    top = 0;
    active = 1;

    popup_filter("");
}

int popup_active(void) {
    return active;
}

void popup_filter(const char *prefix) {
    if (!active) return;

    reserve(nall);
    ncands = 0;

    for (int i = 0; i < nall; i++) {
        int score;
        if (fuzzy_match(prefix, all[i], &score)) {
            cands[ncands].index = i;
            cands[ncands].score = score;
            ncands++;
        }
    }

    qsort(cands, (size_t)ncands, sizeof(Candidate), by_score);
    for (int i = 0; i < ncands; i++) display[i] = all[cands[i].index];

    // nothing left to choose from: get out of the way rather than sit there empty
    if (ncands == 0) {
        popup_close();
        return;
    }

    selected = 0;
    top = 0;
    sync_render();
}

int popup_key(int c) {
    if (!active) return POPUP_IGNORED;

    if (c == ARROW_UP || c == WHEEL_UP) {
        selected--;
        sync_render();
        return POPUP_CONSUMED;
    }
    if (c == ARROW_DOWN || c == WHEEL_DOWN) {
        selected++;
        sync_render();
        return POPUP_CONSUMED;
    }
    if (c == '\t' || c == '\r') {
        return POPUP_ACCEPTED;
    }
    if (c == '\x1b') {
        popup_close();
        return POPUP_CONSUMED;                          // dismissing is the whole action
    }
    return POPUP_IGNORED;
}

int popup_selection(void) {
    if (!active || ncands == 0) return -1;
    if (selected < 0 || selected >= ncands) return -1;
    return cands[selected].index;
}

int popup_anchor_cy(void) { return anchor_cy; }
int popup_anchor_cx(void) { return anchor_cx; }

void popup_close(void) {
    active = 0;
    ncands = 0;
    all = NULL;
    nall = 0;
    render_set_popup(NULL);
}

void popup_free(void) {
    popup_close();

    free(cands);
    cands = NULL;
    cands_cap = 0;

    free(display);
    display = NULL;
    display_cap = 0;
}
