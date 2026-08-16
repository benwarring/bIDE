#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "undo.h"
#include "buffer.h"
#include "editor.h"
#include "wrappers.h"

#define MAX_UNDO 4096           // a long session must not grow without bound

typedef struct {
    undo_type type;
    int cy, cx;                 // position the operation acted on
    char c;                     // character inserted or deleted; unused for split/join
    int pre_cy, pre_cx;         // cursor before the edit, so undo can put it back
    int group;                  // records sharing a group undo as one action
} undo_rec;

typedef struct {
    undo_rec *recs;
    int len;
    int cap;
} undo_stack;

struct UndoState {
    undo_stack undo_st;
    undo_stack redo_st;
    int cur_group;
    int explicit_depth;         // nesting depth of begin/end group
    int force_break;            // next record starts a new group
    int saved_depth;            // undo depth at the last successful save; -1 means unreachable
};

// only one undo or redo can be running at a time, so this stays global
static int suppressed = 0;

UndoState *undo_create(void) {
    UndoState *u = Malloc(sizeof(UndoState));
    u->undo_st = (undo_stack){NULL, 0, 0};
    u->redo_st = (undo_stack){NULL, 0, 0};
    u->cur_group = 0;
    u->explicit_depth = 0;
    u->force_break = 1;
    u->saved_depth = 0;
    return u;
}

/**
 * The current buffer's undo state. Every entry point goes through this, which is
 * why moving undo per-buffer did not change a single call site.
 */
static UndoState *st(void) {
    Buffer *b = editor_buffer();
    return b ? b->undo : NULL;
}

/* ---------------------------------------------------------------- stack --- */

static void stack_push(undo_stack *st, undo_rec rec) {
    if (st->len == st->cap) {
        st->cap = st->cap ? st->cap * 2 : 64;
        st->recs = Realloc(st->recs, sizeof(undo_rec) * (size_t)st->cap);
    }
    st->recs[st->len++] = rec;
}

static void stack_clear(undo_stack *st) {
    st->len = 0;
}

static void stack_free(undo_stack *st) {
    free(st->recs);
    st->recs = NULL;
    st->len = 0;
    st->cap = 0;
}

/**
 * Drops the oldest record once the stack is full. The saved-depth marker refers
 * to a position in this array, so it has to shift too.
 */
static void drop_oldest(UndoState *u, undo_stack *s) {
    memmove(&s->recs[0], &s->recs[1], sizeof(undo_rec) * (size_t)(s->len - 1));
    s->len--;
    if (u->saved_depth > 0) u->saved_depth--;
    else if (u->saved_depth == 0) u->saved_depth = -1;  // the saved state fell off the end
}

void undo_destroy(UndoState *u) {
    if (u == NULL) return;
    stack_free(&u->undo_st);
    stack_free(&u->redo_st);
    free(u);
}

/* -------------------------------------------------------------- grouping --- */

/**
 * Whether this edit continues the run the previous one started: same kind of
 * operation, adjacent position. Typing a word is one group; moving the cursor
 * or changing operation starts another.
 */
static int continues_group(const undo_rec *last, undo_type type, int cy, int cx) {
    if (last->type != type || last->cy != cy) return 0;

    switch (type) {
        case UNDO_INSERT_CHAR:
            return cx == last->cx + 1;                  // typing forward
        case UNDO_DELETE_CHAR:
            return cx == last->cx - 1 || cx == last->cx; // backspace runs, or forward-delete runs
        default:
            return 0;                                   // every Enter and every join stands alone
    }
}

void undo_begin_group(void) {
    UndoState *u = st();
    if (u == NULL) return;

    if (u->explicit_depth == 0) {
        u->cur_group++;
        u->force_break = 0;
    }
    u->explicit_depth++;
}

void undo_end_group(void) {
    UndoState *u = st();
    if (u == NULL) return;

    if (u->explicit_depth > 0) u->explicit_depth--;
    if (u->explicit_depth == 0) u->force_break = 1;
}

void undo_break(void) {
    UndoState *u = st();
    if (u == NULL) return;
    if (u->explicit_depth == 0) u->force_break = 1;
}

/* -------------------------------------------------------------- recording --- */

void undo_record(undo_type type, int cy, int cx, char c) {
    if (suppressed) return;                             // an undo must not record itself

    UndoState *u = st();
    if (u == NULL) return;

    if (u->explicit_depth == 0) {
        if (u->force_break || u->undo_st.len == 0 ||
            !continues_group(&u->undo_st.recs[u->undo_st.len - 1], type, cy, cx)) {
            u->cur_group++;
        }
        u->force_break = 0;
    }

    // a new edit invalidates the redo history
    stack_clear(&u->redo_st);

    // if we undid past the last save and are now editing, that saved state is
    // no longer reachable by undoing, so the file can never look clean again
    if (u->saved_depth > u->undo_st.len) u->saved_depth = -1;

    if (u->undo_st.len == MAX_UNDO) drop_oldest(u, &u->undo_st);

    View *v = editor_view();
    undo_rec rec = {type, cy, cx, c, v->cy, v->cx, u->cur_group};
    stack_push(&u->undo_st, rec);
}

/* ------------------------------------------------------------ apply/undo --- */

static void apply_inverse(const undo_rec *r) {
    switch (r->type) {
        case UNDO_INSERT_CHAR: buffer_delete_char(r->cy, r->cx); break;
        case UNDO_DELETE_CHAR: buffer_insert_char(r->cy, r->cx, r->c); break;
        case UNDO_SPLIT_LINE:  buffer_join_lines(r->cy); break;
        case UNDO_JOIN_LINES:  buffer_split_line(r->cy, r->cx); break;
    }
}

static void apply_forward(const undo_rec *r) {
    switch (r->type) {
        case UNDO_INSERT_CHAR: buffer_insert_char(r->cy, r->cx, r->c); break;
        case UNDO_DELETE_CHAR: buffer_delete_char(r->cy, r->cx); break;
        case UNDO_SPLIT_LINE:  buffer_split_line(r->cy, r->cx); break;
        case UNDO_JOIN_LINES:  buffer_join_lines(r->cy); break;
    }
}

int undo_perform(void) {
    UndoState *u = st();
    if (u == NULL || u->undo_st.len == 0) return 0;

    int group = u->undo_st.recs[u->undo_st.len - 1].group;
    undo_rec last = u->undo_st.recs[u->undo_st.len - 1];

    suppressed = 1;
    while (u->undo_st.len > 0 && u->undo_st.recs[u->undo_st.len - 1].group == group) {
        undo_rec rec = u->undo_st.recs[--u->undo_st.len];
        apply_inverse(&rec);
        stack_push(&u->redo_st, rec);
        last = rec;
    }
    suppressed = 0;

    // land on the edit, which is most of what makes undo feel correct
    View *v = editor_view();
    v->cy = last.pre_cy;
    v->cx = last.pre_cx;
    u->force_break = 1;
    return 1;
}

int redo_perform(void) {
    UndoState *u = st();
    if (u == NULL || u->redo_st.len == 0) return 0;

    int group = u->redo_st.recs[u->redo_st.len - 1].group;
    undo_rec last = u->redo_st.recs[u->redo_st.len - 1];

    suppressed = 1;
    while (u->redo_st.len > 0 && u->redo_st.recs[u->redo_st.len - 1].group == group) {
        undo_rec rec = u->redo_st.recs[--u->redo_st.len];
        apply_forward(&rec);
        stack_push(&u->undo_st, rec);
        last = rec;
    }
    suppressed = 0;

    View *v = editor_view();
    v->cy = last.cy;
    v->cx = last.cx;
    u->force_break = 1;
    return 1;
}

/* ----------------------------------------------------------- dirty state --- */

void undo_mark_saved(void) {
    UndoState *u = st();
    if (u) u->saved_depth = u->undo_st.len;
}

int undo_is_dirty(void) {
    UndoState *u = st();
    return u ? u->saved_depth != u->undo_st.len : 0;
}

int undo_any_dirty(void) {
    for (int i = 0; i < E.nbufs; i++) {
        UndoState *u = E.bufs[i]->undo;
        if (u && u->saved_depth != u->undo_st.len) return 1;
    }
    return 0;
}
