#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#include "input.h"
#include "buffer.h"
#include "command.h"
#include "editor.h"
#include "lsp.h"
#include "pane.h"
#include "render.h"
#include "terminal.h"
#include "undo.h"
#include "wrappers.h"

#define WHEEL_LINES 3           // lines per wheel notch

/* ------------------------------------------------------------ key input --- */

// where the last click, drag or wheel notch landed, in 1-based screen coordinates
static int mouse_x = 0;
static int mouse_y = 0;

// whether Shift was held on the last key read, which turns movement into selecting
static int key_shift = 0;

// the text of the last bracketed paste, for the PASTE key to put in
static char *pasted = NULL;
static size_t npasted = 0;
static size_t pasted_cap = 0;

/**
 * An SGR mouse report is "\x1b[<button;col;row" followed by M for press or m
 * for release. Returns the key value, or -1 for events we don't act on, which
 * the caller already treats as "nothing happened".
 */
static int read_mouse(void) {
    char buf[32];
    unsigned int i = 0;
    char final = 0;

    while (i < sizeof(buf) - 1) {
        unsigned char ch;
        if (Read(STDIN_FILENO, &ch, 1) != 1) break;
        if (ch == 'M' || ch == 'm') {
            final = (char)ch;
            break;
        }
        buf[i++] = (char)ch;
    }
    buf[i] = '\0';

    int button, x, y;
    if (!final || sscanf(buf, "%d;%d;%d", &button, &x, &y) != 3) return -1;

    // low two bits pick the button, 32 means it moved while held; only the left
    // button's press and drag do anything
    int key = button == 64 ? WHEEL_UP
            : button == 65 ? WHEEL_DOWN
            : button == 32 ? MOUSE_DRAG
            : final == 'M' && (button & 3) == 0 ? MOUSE_CLICK : -1;
    if (key != -1) {
        mouse_x = x;
        mouse_y = y;
    }
    return key;
}

/**
 * A key the terminal reported as a codepoint plus modifiers, back in the terms
 * the rest of the editor uses. Modifier values are 1 + a bitmask: 1 Shift,
 * 2 Alt, 4 Ctrl. Ctrl-letter comes back as its old control byte, so turning the
 * extended reporting on changes nothing downstream.
 */
static int modified_key(int code, int mod) {
    int bits = mod - 1;

    switch (code) {                                     // kitty sends these as CSI u too
        case 27:  return '\x1b';
        case 13:  return '\r';
        case 9:   return '\t';
        case 127: return BACKSPACE;
    }
    if (!(bits & 4)) return -1;                         // Alt combinations: nothing uses them

    if (code == ' ') return 0;                          // Ctrl-Space, as NUL like before
    if (code >= '[' && code <= '_') return code & 0x1F; // Ctrl-\ and Ctrl-], as their old control bytes
    if (code >= 'A' && code <= 'Z') code += 'a' - 'A';  // modifyOtherKeys reports the shifted letter
    if (code < 'a' || code > 'z') return -1;
    return (bits & 1) ? CTRL_SHIFT_KEY(code) : CTRL_KEY(code);
}

/**
 * Everything up to the terminal's end-of-paste marker, read raw: inside a paste
 * an escape byte is just text. Terminals strip the marker from what they paste,
 * so the first one really is the end. A terminal that never sends it costs one
 * read timeout.
 */
static int read_paste(void) {
    static const char END[] = "\x1b[201~";
    const size_t endlen = sizeof(END) - 1;

    npasted = 0;
    unsigned char c;
    while (Read(STDIN_FILENO, &c, 1) == 1) {
        if (npasted == pasted_cap) {
            pasted_cap = pasted_cap ? pasted_cap * 2 : 256;
            pasted = Realloc(pasted, pasted_cap);
        }
        pasted[npasted++] = (char)c;
        if (npasted >= endlen && memcmp(pasted + npasted - endlen, END, endlen) == 0) {
            npasted -= endlen;
            break;
        }
    }
    return PASTE;
}

/**
 * "\x1b[<params><final>" with numeric parameters: the ~ keys ("5~"), modified
 * arrows ("1;5C"), and modified keys in either extended form, xterm's
 * "27;<mod>;<code>~" or kitty's "<code>;<mod>u". "200~" opens a paste.
 */
static int read_csi(unsigned char first) {
    char buf[32];
    unsigned int i = 0;
    buf[i++] = (char)first;

    unsigned char final;
    while (1) {
        if (Read(STDIN_FILENO, &final, 1) != 1) return '\x1b';
        if (final >= 0x40 && final <= 0x7E) break;      // the final byte ends every CSI sequence
        if (i < sizeof(buf) - 1) buf[i++] = (char)final;
    }
    buf[i] = '\0';

    int p0 = 0, mod = 1, p2 = 0;                        // no modifier parameter means none
    int np = sscanf(buf, "%d;%d;%d", &p0, &mod, &p2);
    int ctrl = (mod - 1) & 4;
    key_shift = (mod - 1) & 1;

    switch (final) {
        case 'u': return modified_key(p0, mod);
        case '~':
            if (np == 3 && p0 == 27) return modified_key(p2, mod);
            if (p0 == 200) return read_paste();
            switch (p0) {
                case 1: case 7: return HOME_KEY;
                case 3:         return DEL_KEY;
                case 4: case 8: return END_KEY;
                case 5:         return PAGE_UP;
                case 6:         return PAGE_DOWN;
            }
            return -1;
        case 'A': return ctrl ? CTRL_ARROW_UP : ARROW_UP;
        case 'B': return ctrl ? CTRL_ARROW_DOWN : ARROW_DOWN;
        case 'C': return ctrl ? CTRL_ARROW_RIGHT : ARROW_RIGHT;
        case 'D': return ctrl ? CTRL_ARROW_LEFT : ARROW_LEFT;
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
    }
    return -1;                                          // something we don't use; ignore it whole
}

int editor_read_key(void) {
    unsigned char c;
    key_shift = 0;

    if (Read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c != '\x1b') return c;

    // an escape byte with nothing behind it is a real Escape keypress;
    // the VTIME timeout is what makes that distinguishable from a sequence
    unsigned char seq[2];
    if (Read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (Read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
        if (seq[1] == '<') return read_mouse();

        if (seq[1] >= '0' && seq[1] <= '9') {
            return read_csi(seq[1]);
        } else {
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
        }
    }

    return '\x1b';
}

/* -------------------------------------------------------- cursor set ------ */

/* A cursor plus where it came from, so positions can be written back after
 * being sorted into edit order. idx -1 is the primary. */
typedef struct {
    bpos pos;
    int idx;
} Cur;

int editor_has_extra_cursors(void) {
    return editor_view()->nextra > 0;
}

static int by_position_desc(const void *a, const void *b) {
    const Cur *x = a, *y = b;
    if (x->pos.cy != y->pos.cy) return y->pos.cy - x->pos.cy;
    return y->pos.cx - x->pos.cx;
}

/**
 * Collects every cursor, sorted last position first. Editing at a later
 * position never disturbs an earlier one, so applying edits in this order means
 * no position ever needs adjusting afterward. Patching positions up after each
 * edit instead is where the subtle three-cursor bugs live.
 */
static int gather(Cur *out) {
    View *v = editor_view();

    int n = 0;
    out[n].pos = (bpos){v->cy, v->cx};
    out[n].idx = -1;
    n++;

    for (int i = 0; i < v->nextra; i++) {
        out[n].pos = v->extra[i];
        out[n].idx = i;
        n++;
    }

    qsort(out, (size_t)n, sizeof(Cur), by_position_desc);
    return n;
}

static void scatter(Cur *arr, int n) {
    View *v = editor_view();

    for (int i = 0; i < n; i++) {
        if (arr[i].idx < 0) {
            v->cy = arr[i].pos.cy;
            v->cx = arr[i].pos.cx;
        } else {
            v->extra[arr[i].idx] = arr[i].pos;
        }
    }
}

/**
 * Two cursors landing on the same spot must collapse, or every later edit gets
 * applied twice there.
 */
static void merge_cursors(void) {
    View *v = editor_view();

    int out = 0;
    for (int i = 0; i < v->nextra; i++) {
        if (v->extra[i].cy == v->cy && v->extra[i].cx == v->cx) continue;

        int dup = 0;
        for (int j = 0; j < out; j++) {
            if (v->extra[j].cy == v->extra[i].cy && v->extra[j].cx == v->extra[i].cx) {
                dup = 1;
                break;
            }
        }
        if (!dup) v->extra[out++] = v->extra[i];
    }
    v->nextra = out;
}

static void add_cursor(int cy, int cx) {
    View *v = editor_view();
    if (v->nextra >= MAX_EXTRA_CURSORS) return;

    v->extra = Realloc(v->extra, sizeof(bpos) * (size_t)(v->nextra + 1));
    v->extra[v->nextra].cy = cy;
    v->extra[v->nextra].cx = cx;
    v->nextra++;
    merge_cursors();
}

void editor_cursor_collapse(void) {
    editor_view()->nextra = 0;
}

/* ------------------------------------------------------------ movement --- */

static void clamp_pos(bpos *p) {
    int last = buffer_num_lines() - 1;
    if (p->cy < 0) p->cy = 0;
    if (p->cy > last) p->cy = last;

    int len = buffer_line_length(p->cy);
    if (len < 0) len = 0;
    if (p->cx > len) p->cx = len;
    if (p->cx < 0) p->cx = 0;
}

void editor_clamp_cursor(void) {
    View *v = editor_view();

    bpos p = {v->cy, v->cx};
    clamp_pos(&p);
    v->cy = p.cy;
    v->cx = p.cx;
}

/* ----------------------------------------------------------- selection --- */

// set by whatever wants the selection to outlive the key being handled
static int keep_selection = 0;

int editor_selection(bpos *start, bpos *end) {
    View *v = editor_view();
    if (!v->selecting) return 0;

    bpos a = v->anchor, c = {v->cy, v->cx};
    clamp_pos(&a);                                      // another pane may have edited under it

    int anchor_first = a.cy < c.cy || (a.cy == c.cy && a.cx < c.cx);
    *start = anchor_first ? a : c;
    *end = anchor_first ? c : a;
    return start->cy != end->cy || start->cx != end->cx;
}

/* The cursor is about to move and drag the selection with it, anchored where it
 * stands now. A selection belongs to one cursor, so any extras collapse. */
static void extend_selection(void) {
    View *v = editor_view();
    keep_selection = 1;
    if (v->selecting) return;

    editor_cursor_collapse();
    v->anchor = (bpos){v->cy, v->cx};
    v->selecting = 1;
}

static void op_left(bpos *p) {
    if (p->cx > 0) p->cx--;
    else if (p->cy > 0) {
        p->cy--;
        p->cx = buffer_line_length(p->cy);
    }
}

static void op_right(bpos *p) {
    int len = buffer_line_length(p->cy);
    if (len >= 0 && p->cx < len) p->cx++;
    else if (p->cy < buffer_num_lines() - 1) {
        p->cy++;
        p->cx = 0;
    }
}

static int is_word_byte(int c) {
    return c != -1 && (isalnum(c) || c == '_');
}

/* 0 whitespace, 1 word, 2 punctuation: a word jump skips whitespace, then one run
 * of a single class, so "foo->bar" stops at the arrow instead of leaping it. */
static int char_class(int c) {
    if (c == -1 || isspace(c)) return 0;
    return is_word_byte(c) ? 1 : 2;
}

static void op_word_left(bpos *p) {
    if (p->cx == 0) { op_left(p); return; }             // start of line: wrap to previous
    while (p->cx > 0 && char_class(buffer_char_at(p->cy, p->cx - 1)) == 0) p->cx--;
    if (p->cx == 0) return;
    int cls = char_class(buffer_char_at(p->cy, p->cx - 1));
    while (p->cx > 0 && char_class(buffer_char_at(p->cy, p->cx - 1)) == cls) p->cx--;
}

static void op_word_right(bpos *p) {
    int len = buffer_line_length(p->cy);
    if (p->cx >= len) { op_right(p); return; }          // end of line: wrap to next
    while (p->cx < len && char_class(buffer_char_at(p->cy, p->cx)) == 0) p->cx++;
    if (p->cx == len) return;
    int cls = char_class(buffer_char_at(p->cy, p->cx));
    while (p->cx < len && char_class(buffer_char_at(p->cy, p->cx)) == cls) p->cx++;
}

static void op_up(bpos *p)    { if (p->cy > 0) p->cy--; clamp_pos(p); }
static void op_down(bpos *p)  { if (p->cy < buffer_num_lines() - 1) p->cy++; clamp_pos(p); }
static void op_start(bpos *p) { p->cx = 0; }
static void op_end(bpos *p)   { p->cx = buffer_line_length(p->cy); }

/** Movement applies to every cursor, so a multi-cursor selection survives it.
 * With Shift held it selects instead. */
static void multi_move(void (*op)(bpos *)) {
    View *v = editor_view();
    undo_break();                                       // edits either side of a move are separate
    if (key_shift) extend_selection();

    bpos p = {v->cy, v->cx};
    op(&p);
    clamp_pos(&p);
    v->cy = p.cy;
    v->cx = p.cx;

    for (int i = 0; i < v->nextra; i++) {
        op(&v->extra[i]);
        clamp_pos(&v->extra[i]);
    }
    merge_cursors();
}

void editor_move_left(void)  { multi_move(op_left); }
void editor_move_right(void) { multi_move(op_right); }
void editor_move_up(void)    { multi_move(op_up); }
void editor_word_left(void)  { multi_move(op_word_left); }
void editor_word_right(void) { multi_move(op_word_right); }
void editor_move_down(void)  { multi_move(op_down); }
void editor_line_start(void) { multi_move(op_start); }
void editor_line_end(void)   { multi_move(op_end); }

/* A jump is navigation, not a multi-cursor edit, so it collapses back to one. */
static void jump_to(int cy) {
    View *v = editor_view();
    undo_break();
    if (key_shift) extend_selection();                  // Shift-PgUp and Shift-PgDn select too
    editor_cursor_collapse();

    v->cy = cy;
    bpos p = {v->cy, v->cx};
    clamp_pos(&p);
    v->cy = p.cy;
    v->cx = p.cx;
}

/* The mouse acts on the pane under it, so that pane takes focus first. NULL for
 * the message line, which belongs to no pane. */
static View *focus_under_mouse(void) {
    View *v = pane_at(mouse_y - 1, mouse_x - 1);
    if (v) pane_focus(v);
    return v;
}

void editor_page_up(void)   { jump_to(editor_view()->cy - editor_view()->rows); }
void editor_page_down(void) { jump_to(editor_view()->cy + editor_view()->rows); }
void editor_wheel_up(void)   { if (focus_under_mouse()) jump_to(editor_view()->cy - WHEEL_LINES); }
void editor_wheel_down(void) { if (focus_under_mouse()) jump_to(editor_view()->cy + WHEEL_LINES); }

// the cursor to the text under a cell of pane v, counted from the pane's corner
static void cursor_to_cell(View *v, int screen_row, int screen_col) {
    jump_to(screen_row + v->rowoff);

    // strip the gutter, then convert the rendered column back to a byte index so
    // a click past a tab lands where it looks like it should
    int rx = screen_col - v->gutter + v->coloff;
    if (rx < 0) rx = 0;
    v->cx = buffer_rx_to_cx(v->cy, rx);

    bpos p = {v->cy, v->cx};
    clamp_pos(&p);
    v->cx = p.cx;
}

void editor_mouse_click(void) {
    View *v = focus_under_mouse();
    if (v == NULL) return;

    int screen_row = mouse_y - 1 - v->top;
    int screen_col = mouse_x - 1 - v->left;
    if (screen_row >= v->rows || screen_col >= v->cols) return;   // its status bar or divider

    cursor_to_cell(v, screen_row, screen_col);
}

/* Selects from where the button went down to wherever it is now. The drag stays
 * in the pane it started in, so a mouse that wanders off is held to its edge. */
void editor_mouse_drag(void) {
    View *v = editor_view();
    extend_selection();

    int screen_row = mouse_y - 1 - v->top;
    if (screen_row < 0) screen_row = 0;
    if (screen_row >= v->rows) screen_row = v->rows - 1;
    cursor_to_cell(v, screen_row, mouse_x - 1 - v->left);
}

/* ------------------------------------------------------------- editing --- */

/* Auto-closing pairs. Same index in both strings: '(' closes with ')'. Quotes
 * appear in both because they close with themselves. */
static const char PAIR_OPEN[]  = "([{\"'";
static const char PAIR_CLOSE[] = ")]}\"'";

static char closing_for(int c) {
    const char *p = strchr(PAIR_OPEN, c);
    return (p && *p) ? PAIR_CLOSE[p - PAIR_OPEN] : 0;
}

/**
 * Auto-closing helps when opening a pair in empty space, and gets in the way
 * mid-word: typing an apostrophe in "don't" should not produce "don''t".
 */
static int should_autoclose(const bpos *p, int open, int at) {
    if (is_word_byte(at)) return 0;                     // right before a word

    if (open == '"' || open == '\'') {
        int before = (p->cx > 0) ? buffer_char_at(p->cy, p->cx - 1) : -1;
        if (is_word_byte(before)) return 0;             // an apostrophe inside a word
    }
    return 1;
}

static void op_insert(bpos *p, int c) {
    int at = buffer_char_at(p->cy, p->cx);              // -1 at end of line

    // typing the closing half of a pair that is already sitting there steps
    // over it instead of doubling it
    if (c != 0 && strchr(PAIR_CLOSE, c) && at == c) {
        p->cx++;
        return;
    }

    char close = closing_for(c);
    if (close && should_autoclose(p, c, at)) {
        undo_begin_group();                             // the pair is one action to undo
        bpos q = buffer_insert_char(p->cy, p->cx, c);
        q = buffer_insert_char(q.cy, q.cx, close);
        undo_end_group();

        p->cy = q.cy;
        p->cx = q.cx - 1;                               // land between the two
        return;
    }

    *p = buffer_insert_char(p->cy, p->cx, c);
}

static void op_split(bpos *p, int c) {
    (void)c;
    *p = buffer_split_line(p->cy, p->cx);
}

/* The new line starts at the indentation of the one it came from, copied byte
 * for byte so tabs stay tabs. Only the indentation left of the cursor counts:
 * splitting inside it already carries the rest down. One undo step, like Enter
 * without it. */
static void op_newline(bpos *p, int c) {
    int indent = 0;
    while (indent < p->cx && (buffer_char_at(p->cy, indent) == ' ' ||
                              buffer_char_at(p->cy, indent) == '\t')) indent++;

    undo_begin_group();
    int from = p->cy;
    op_split(p, c);
    for (int i = 0; i < indent; i++) *p = buffer_insert_char(p->cy, p->cx, buffer_char_at(from, i));
    undo_end_group();
}

static void op_backspace(bpos *p, int c) {
    (void)c;
    if (p->cy == 0 && p->cx == 0) return;               // start of buffer: silently do nothing

    if (p->cx > 0) {
        *p = buffer_delete_char(p->cy, p->cx - 1);      // the character before the cursor
    } else {
        *p = buffer_join_lines(p->cy - 1);              // the seam comes back from the mutation
    }
}

static void op_delete_fwd(bpos *p, int c) {
    (void)c;
    int len = buffer_line_length(p->cy);

    if (p->cx < len) {
        *p = buffer_delete_char(p->cy, p->cx);
    } else if (p->cy < buffer_num_lines() - 1) {
        *p = buffer_join_lines(p->cy);                  // pull the next line up
    }
}

/**
 * Shifts one already-edited position to account for an edit that has just
 * landed at an earlier point in the buffer.
 *
 * Only positions strictly *after* the edit need this, which is exactly the set
 * multi_edit has already processed: the ones still pending are earlier, and an
 * edit never disturbs the text before it.
 */
static void shift_pos(bpos *p, undo_type op, int cy, int cx) {
    switch (op) {
        case UNDO_INSERT_CHAR:
            if (p->cy == cy && p->cx >= cx) p->cx++;
            break;
        case UNDO_DELETE_CHAR:
            if (p->cy == cy && p->cx > cx) p->cx--;
            break;
        case UNDO_SPLIT_LINE:
            // the tail of the split line became a new line; everything below moved down
            if (p->cy > cy) p->cy++;
            else if (p->cy == cy && p->cx >= cx) {
                p->cy = cy + 1;
                p->cx -= cx;
            }
            break;
        case UNDO_JOIN_LINES:                           // cx is the seam
            if (p->cy > cy + 1) p->cy--;
            else if (p->cy == cy + 1) {
                p->cy = cy;
                p->cx += cx;                            // now that far along the joined line
            }
            break;
    }
}

// the cursors already edited this keystroke, and so the ones needing shifts
static Cur *pending = NULL;
static int npending = 0;

static void on_mutation(undo_type op, int cy, int cx, void *ctx) {
    (void)ctx;
    for (int i = 0; i < npending; i++) shift_pos(&pending[i].pos, op, cy, cx);
}

/* Deletes from s to e, leaving the cursor at s: one forward delete per byte in
 * between, newlines included, all landing on s as the text closes up. */
static void delete_range(bpos s, bpos e) {
    View *v = editor_view();

    int n = e.cx - s.cx;
    for (int y = s.cy; y < e.cy; y++) n += buffer_line_length(y) + 1;
    for (bpos p = s; n > 0; n--) op_delete_fwd(&p, 0);

    v->cy = s.cy;
    v->cx = s.cx;
    v->selecting = 0;
}

/**
 * Runs one edit at every cursor. The whole set is a single undo group, because
 * the grouping heuristics see N edits scattered across the file and cannot know
 * they belong together — which is exactly why the markers are explicit.
 *
 * Over a selection, the edit replaces it: the selection goes first, and
 * Backspace or Delete stop there. Both halves undo as one.
 */
static void multi_edit(void (*op)(bpos *, int), int arg) {
    bpos s, e;
    int replacing = editor_selection(&s, &e);
    if (replacing) {
        undo_begin_group();
        delete_range(s, e);
        if (op == op_backspace || op == op_delete_fwd) {
            undo_end_group();
            return;
        }
    }

    Cur cur[MAX_EXTRA_CURSORS + 1];
    int n = gather(cur);

    // one cursor keeps the ordinary typing heuristics, which group a whole word
    if (n > 1) undo_begin_group();

    pending = cur;
    buffer_set_hook(on_mutation, NULL);

    for (int i = 0; i < n; i++) {
        // everything before i is already edited and must follow this edit;
        // cur[i] itself is placed by the mutation's own return value
        npending = i;
        op(&cur[i].pos, arg);
    }

    buffer_set_hook(NULL, NULL);                        // undo and redo must not fire it
    pending = NULL;
    npending = 0;

    if (n > 1) undo_end_group();
    if (replacing) undo_end_group();

    scatter(cur, n);
    merge_cursors();
}

void editor_insert_text(int c)     { multi_edit(op_insert, c); }
void editor_insert_newline(void)   { multi_edit(op_newline, 0); }
void editor_backspace(void)        { multi_edit(op_backspace, 0); }
void editor_delete_forward(void)   { multi_edit(op_delete_fwd, 0); }

/* ------------------------------------------------------ copy and paste --- */

// what the last copy took, for Ctrl-V where the terminal lets it through
static char *clip = NULL;
static size_t nclip = 0;

static void op_insert_raw(bpos *p, int c) {
    *p = buffer_insert_char(p->cy, p->cx, c);
}

/**
 * Pasted text goes in exactly as it came, at every cursor: no auto-closed
 * brackets, no auto-indent on top of the indentation it already carries, no
 * completion popping up, and a line break in any of the three conventions is
 * one line break. The whole paste is one undo step.
 */
static void paste(const char *text, size_t len) {
    undo_begin_group();
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' && i > 0 && text[i - 1] == '\r') continue;
        if (c == '\r' || c == '\n') multi_edit(op_split, 0);
        else if (c == '\t' || (c >= 32 && c != 127)) multi_edit(op_insert_raw, c);
    }
    undo_end_group();
}

/**
 * The selection goes to the system clipboard through the terminal, so the
 * terminal's own paste (and every other program) gets it, and is kept here for
 * Ctrl-V in terminals that pass that key through. The selection stays, as it
 * does everywhere else.
 */
void editor_copy(void) {
    bpos s, e;
    if (!editor_selection(&s, &e)) {
        editor_set_status_message("Nothing selected");
        return;
    }
    keep_selection = 1;

    size_t cap = 1;
    for (int y = s.cy; y <= e.cy; y++) cap += (size_t)buffer_line_length(y) + 1;
    free(clip);
    clip = Malloc(cap);

    char *p = clip;
    for (int y = s.cy; y <= e.cy; y++) {
        int len = buffer_get_line(y, p, cap - (size_t)(p - clip));  // the whole line, then trimmed
        int from = y == s.cy ? s.cx : 0;
        int to = y == e.cy ? e.cx : len;
        memmove(p, p + from, (size_t)(to - from));
        p += to - from;
        if (y < e.cy) *p++ = '\n';
    }
    nclip = (size_t)(p - clip);

    terminal_set_clipboard(clip, nclip);
    editor_set_status_message("Copied %zu byte%s", nclip, nclip == 1 ? "" : "s");
}

void editor_cut(void) {
    bpos s, e;
    if (!editor_selection(&s, &e)) {
        editor_set_status_message("Nothing selected");
        return;
    }

    editor_copy();
    undo_begin_group();
    delete_range(s, e);
    undo_end_group();
    editor_set_status_message("Cut %zu byte%s", nclip, nclip == 1 ? "" : "s");
}

void editor_paste(void) {
    if (clip == NULL) {
        editor_set_status_message("Nothing copied yet");
        return;
    }
    paste(clip, nclip);
}

/* ------------------------------------------------------- extra cursors --- */

/**
 * The new cursor takes its column from the *primary* cursor, not from the row it
 * grows out of. Inheriting the neighbour's column instead means one short line
 * anywhere in the run drags every cursor below it left permanently, which is
 * what a column of cursors is least able to tolerate.
 */
void editor_cursor_add_below(void) {
    View *v = editor_view();

    int cy = v->cy;
    for (int i = 0; i < v->nextra; i++) {
        if (v->extra[i].cy > cy) cy = v->extra[i].cy;
    }
    if (cy + 1 > buffer_num_lines() - 1) return;

    bpos p = {cy + 1, v->cx};
    clamp_pos(&p);
    add_cursor(p.cy, p.cx);
}

void editor_cursor_add_above(void) {
    View *v = editor_view();

    int cy = v->cy;
    for (int i = 0; i < v->nextra; i++) {
        if (v->extra[i].cy < cy) cy = v->extra[i].cy;
    }
    if (cy - 1 < 0) return;

    bpos p = {cy - 1, v->cx};
    clamp_pos(&p);
    add_cursor(p.cy, p.cx);
}

/**
 * Adds a cursor at the next occurrence of the word under the primary cursor,
 * wrapping at the end of the file. This is the multi-cursor operation actually
 * worth having.
 */
void editor_cursor_add_next(void) {
    View *v = editor_view();

    static char word[256];
    static char line[4096];

    int len = buffer_get_line(v->cy, line, sizeof(line));
    if (len <= 0) return;

    int start = v->cx, end = v->cx;
    while (start > 0 && is_word_byte((unsigned char)line[start - 1])) start--;
    while (end < len && is_word_byte((unsigned char)line[end])) end++;
    if (end - start <= 0 || end - start >= (int)sizeof(word)) return;

    memcpy(word, &line[start], (size_t)(end - start));
    word[end - start] = '\0';

    // start looking after the last cursor we already placed
    int from_y = v->cy, from_x = start;
    for (int i = 0; i < v->nextra; i++) {
        if (v->extra[i].cy > from_y ||
            (v->extra[i].cy == from_y && v->extra[i].cx > from_x)) {
            from_y = v->extra[i].cy;
            from_x = v->extra[i].cx;
        }
    }

    int nrows = buffer_num_lines();
    for (int step = 0; step <= nrows; step++) {
        int row = (from_y + step) % nrows;
        int rlen = buffer_get_line(row, line, sizeof(line));
        if (rlen < 0) continue;

        int begin = (step == 0) ? from_x + 1 : 0;
        for (int i = begin; i + (end - start) <= rlen; i++) {
            if (strncmp(&line[i], word, (size_t)(end - start)) != 0) continue;

            int before = (i > 0) ? (unsigned char)line[i - 1] : -1;
            int after = (i + (end - start) < rlen) ? (unsigned char)line[i + (end - start)] : -1;
            if (is_word_byte(before) || is_word_byte(after)) continue;   // whole words only

            add_cursor(row, i);
            return;
        }
    }
}

/* ---------------------------------------------------------------- misc --- */

void editor_undo(void) {
    editor_cursor_collapse();
    if (!undo_perform()) editor_set_status_message("Nothing to undo");
}

void editor_redo(void) {
    editor_cursor_collapse();
    if (!redo_perform()) editor_set_status_message("Nothing to redo");
}

void editor_save(void) {
    undo_break();

    if (editor_buffer()->filename == NULL) {
        char *name = editor_prompt("Save as: %s (ESC to cancel)", NULL);
        if (name == NULL) {
            editor_set_status_message("Save aborted");
            return;
        }
        buffer_set_filename(name);
        free(name);
    }

    long written = buffer_save();
    if (written == -1) {
        editor_set_status_message("Can't save! I/O error: %s", strerror(errno));
    } else {
        editor_set_status_message("%ld bytes written to disk", written);
    }
}

/* ------------------------------------------------------------- prompt --- */

char *editor_prompt(const char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = Malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        if (resize_pending) {                           // the nested loop owns redraws while it runs
            resize_pending = 0;
            update_window_size();
        }
        editor_set_status_message(prompt, buf);
        editor_refresh_screen();

        int c = editor_read_key();
        if (c == -1) continue;                          // read timeout, not a keypress

        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editor_set_status_message("");
            if (callback) callback(buf, c);
            free(buf);                                  // cancel frees the partial input
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editor_set_status_message("");
                if (callback) callback(buf, c);
                return buf;                             // caller owns it now
            }
            continue;
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = Realloc(buf, bufsize);
            }
            buf[buflen++] = (char)c;
            buf[buflen] = '\0';
        }

        if (callback) callback(buf, c);
    }
}

/* ------------------------------------------------------------ dispatch --- */

static int dispatch(int c) {
    /* The completion popup gets first refusal, and refuses nearly everything: it
     * wants the arrows, Tab, Enter and Escape, and lets every other key straight
     * through to the buffer underneath. That is what makes it non-modal. */
    if (lsp_completion_key(c)) return 1;

    if (c == PASTE) {
        paste(pasted, npasted);
        command_reset_quit();
        lsp_completion_sync();
        return 1;
    }

    // key dispatch is now two lookups in data, not a switch: find the binding,
    // find its command, invoke it
    if (command_dispatch(c)) {
        lsp_completion_sync();                          // the cursor may have walked off
        return command_quit_requested() ? 0 : 1;
    }

    // tab is byte 9 and is text; everything else below 32, and 127, is not
    if (c == '\t' || (c >= 32 && c < 127)) {
        editor_insert_text(c);
        command_reset_quit();

        // the keystroke lands first, then the list catches up with it
        lsp_completion_sync();
        lsp_completion_maybe_trigger(c);
    }
    return 1;
}

int editor_process_keypress(int c) {
    keep_selection = 0;
    int keep_running = dispatch(c);

    // a selection lasts only as long as keys keep extending (or copying) it
    if (!keep_selection) editor_view()->selecting = 0;
    return keep_running;
}
