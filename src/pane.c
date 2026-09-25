#include <stdlib.h>
#include <string.h>

#include "pane.h"
#include "buffer.h"
#include "editor.h"
#include "popup.h"
#include "render.h"
#include "wrappers.h"

// each half of a split keeps a line of text and its status bar, or a gutter and a few columns
#define MIN_ROWS 2
#define MIN_COLS (GUTTER_MIN + 4)

/* A leaf holds a view; anything else holds two children and says which way the
 * line between them runs. Always a 50/50 split. */
typedef struct Pane {
    struct Pane *parent;
    struct Pane *kid[2];        // left and right, or top and bottom
    int side_by_side;
    View *view;                 // leaves only
} Pane;

static Pane *root = NULL;

// the leaves in screen order, rebuilt by every layout
static View **leaves = NULL;
static int nleaves = 0;
static int leaves_cap = 0;

static Pane *new_pane(View *v) {
    Pane *p = Malloc(sizeof(Pane));
    memset(p, 0, sizeof(*p));
    p->view = v;
    return p;
}

static Pane *find(Pane *p, const View *v) {
    if (p->view) return p->view == v ? p : NULL;
    Pane *hit = find(p->kid[0], v);
    return hit ? hit : find(p->kid[1], v);
}

void pane_init(void) {
    View *v = Malloc(sizeof(View));
    memset(v, 0, sizeof(*v));                           // no buffer yet: buffer_open supplies it
    root = new_pane(v);
    E.view = v;
}

/* ------------------------------------------------------------- layout --- */

static void push(View *v) {
    if (nleaves == leaves_cap) {
        leaves_cap = leaves_cap ? leaves_cap * 2 : 8;
        leaves = Realloc(leaves, sizeof(View *) * (size_t)leaves_cap);
    }
    leaves[nleaves++] = v;
}

/* A leaf's rectangle is its text rows with its status bar under them, which is
 * also what separates it from a pane below. Side by side, the halves give up
 * one column between them to a divider. */
static void lay_out(Pane *p, int top, int left, int height, int width) {
    if (p->view) {
        View *v = p->view;
        v->top = top;
        v->left = left;
        v->rows = height - 1 < 1 ? 1 : height - 1;
        v->cols = width < 1 ? 1 : width;
        push(v);
        return;
    }

    if (p->side_by_side) {
        int w = (width - 1) / 2;
        lay_out(p->kid[0], top, left, height, w);
        lay_out(p->kid[1], top, left + w + 1, height, width - w - 1);
    } else {
        int h = height / 2;
        lay_out(p->kid[0], top, left, h, width);
        lay_out(p->kid[1], top + h, left, height - h, width);
    }
}

View **pane_views(int *n) {
    nleaves = 0;
    lay_out(root, 0, 0, E.screenrows + 1, E.screencols); // every row but the message line
    *n = nleaves;
    return leaves;
}

View *pane_at(int row, int col) {
    int n;
    View **views = pane_views(&n);
    for (int i = 0; i < n; i++) {
        View *v = views[i];
        if (row >= v->top && row <= v->top + v->rows && col >= v->left && col <= v->left + v->cols) {
            return v;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------- focus --- */

void pane_focus(View *v) {
    if (v == E.view) return;

    // leaving a pane is navigation, which collapses cursors and drops the
    // selection the way a jump does; either would go stale while another pane
    // edits the same file
    if (E.view) {
        E.view->nextra = 0;
        E.view->selecting = 0;
    }
    popup_close();                                      // it hangs from the old pane's cursor
    E.view = v;
}

/**
 * Steps from the cursor's screen cell to just past this pane's edge, and focuses
 * whatever is there. Level with the cursor is what makes "left" from a tall pane
 * into a column of short ones land on the one you were looking at.
 */
void pane_focus_dir(int drow, int dcol) {
    View *v = E.view;
    int row = v->top + (v->cy - v->rowoff);
    int col = v->left + v->gutter + (v->rx - v->coloff);

    if (drow < 0) row = v->top - 1;                     // the status bar of the pane above
    if (drow > 0) row = v->top + v->rows + 1;           // past our own status bar
    if (dcol < 0) col = v->left - 1;                    // the left neighbour's divider
    if (dcol > 0) col = v->left + v->cols + 1;          // past our own divider

    View *to = pane_at(row, col);
    if (to) pane_focus(to);
}

/* ------------------------------------------------------- split, close --- */

void pane_split(int side_by_side) {
    View *v = E.view;
    if (side_by_side ? v->cols < 2 * MIN_COLS + 1 : v->rows + 1 < 2 * MIN_ROWS) {
        editor_set_status_message("No room to split this pane");
        return;
    }

    View *nv = Malloc(sizeof(View));
    *nv = *v;                                           // same buffer, same place
    nv->extra = NULL;
    nv->nextra = 0;

    // the leaf becomes the split in place, so its parent's pointer stays right
    Pane *p = find(root, v);
    p->kid[0] = new_pane(v);
    p->kid[1] = new_pane(nv);
    p->kid[0]->parent = p->kid[1]->parent = p;
    p->side_by_side = side_by_side;
    p->view = NULL;

    pane_focus(nv);
}

void pane_close(void) {
    Pane *p = find(root, E.view);
    Pane *parent = p->parent;
    if (parent == NULL) {
        editor_set_status_message("This is the only pane");
        return;
    }

    int was_second = parent->kid[1] == p;
    Pane *sib = parent->kid[!was_second];

    View *v = p->view;
    buffer_show(v, NULL);                               // the buffer remembers where this pane was
    E.view = NULL;
    free(v->extra);
    free(v);
    free(p);

    // the sibling moves up into the parent's place, in place, for the same reason
    *parent = (Pane){parent->parent, {sib->kid[0], sib->kid[1]}, sib->side_by_side, sib->view};
    for (int i = 0; i < 2; i++) {
        if (parent->kid[i]) parent->kid[i]->parent = parent;
    }
    free(sib);

    // focus the part of what is left that was nearest the closed pane
    Pane *q = parent;
    while (q->view == NULL) q = q->kid[was_second];
    pane_focus(q->view);
}

static void free_tree(Pane *p) {
    if (p == NULL) return;
    free_tree(p->kid[0]);
    free_tree(p->kid[1]);
    if (p->view) {
        free(p->view->extra);
        free(p->view);
    }
    free(p);
}

void pane_free(void) {
    free_tree(root);
    root = NULL;
    E.view = NULL;

    free(leaves);
    leaves = NULL;
    nleaves = leaves_cap = 0;
}
