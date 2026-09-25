#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

#include "render.h"
#include "buffer.h"
#include "command.h"
#include "editor.h"
#include "input.h"
#include "lsp.h"
#include "pane.h"
#include "syntax.h"
#include "undo.h"
#include "wrappers.h"

#define BIDE_VERSION "0.1.0"
#define STATUSMSG_TIMEOUT 5

typedef struct {
    char *b;
    size_t len;
    size_t cap;
} abuf;

#define ABUF_INIT {NULL, 0, 0}

// grows geometrically: syntax coloring appends a character at a time, and a
// realloc per character would make every frame quadratic
static void ab_append(abuf *ab, const char *s, size_t len) {
    if (ab->len + len > ab->cap) {
        ab->cap = (ab->len + len) * 2;
        ab->b = Realloc(ab->b, ab->cap);
    }
    memcpy(&ab->b[ab->len], s, len);
    ab->len += len;
}

static void ab_appendstr(abuf *ab, const char *s) {
    ab_append(ab, s, strlen(s));
}

static void ab_pad(abuf *ab, int from, int to) {
    for (int i = from; i < to; i++) ab_appendstr(ab, " ");
}

// the one place 0-indexed screen cells become 1-indexed terminal coordinates
static void move_to(abuf *ab, int row, int col) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row + 1, col + 1);
    ab_append(ab, buf, (size_t)n);
}

// scratch space for materializing one line, and its highlights, per draw
static char *linebuf = NULL;
static size_t linebuf_cap = 0;
static unsigned char *hlbuf = NULL;
static size_t hlbuf_cap = 0;

static void linebuf_reserve(size_t need) {
    if (need <= linebuf_cap) return;
    linebuf = Realloc(linebuf, need);
    linebuf_cap = need;

    hlbuf = Realloc(hlbuf, need);
    hlbuf_cap = need;
}

static Overlay *overlay = NULL;

void render_set_overlay(Overlay *o) {
    overlay = (o && o->active) ? o : NULL;
    editor_request_redraw();
}

static Popup *popup = NULL;

void render_set_popup(Popup *p) {
    popup = (p && p->active) ? p : NULL;
    editor_request_redraw();
}

/**
 * Drawn last, by absolute cursor addressing, rather than spliced into the rows
 * as they are built: the rows carry color escapes, so there is no honest way to
 * overwrite "columns 12 through 30" of one after the fact. Positioning each box
 * row instead costs a few escape sequences and still leaves in the single write
 * the rest of the frame is composed for.
 */
static void draw_popup(abuf *ab) {
    if (popup == NULL || popup->nitems == 0) return;

    int width = popup->width;
    if (width > E.screencols) width = E.screencols;
    if (width < 1) return;

    int height = popup->height;
    if (height > popup->nitems - popup->top) height = popup->nitems - popup->top;
    if (height < 1) return;

    // below the anchor by default, flipped above when there is no room down there
    int row = popup->row + 1;
    if (row + height > E.screenrows) {
        row = popup->row - height;
        if (row < 0) {                                  // nowhere comfortable: clamp
            row = 0;
            if (height > E.screenrows) height = E.screenrows;
        }
    }

    int col = popup->col;
    if (col + width > E.screencols) col = E.screencols - width;
    if (col < 0) col = 0;

    for (int i = 0; i < height; i++) {
        int idx = popup->top + i;
        if (idx >= popup->nitems) break;

        move_to(ab, row + i, col);
        ab_appendstr(ab, idx == popup->selected ? "\x1b[7m" : "\x1b[100m");

        char line[512];
        snprintf(line, sizeof(line), " %s", popup->items[idx]);

        int len = (int)strlen(line);
        if (len > width) len = width;
        ab_append(ab, line, (size_t)len);
        ab_pad(ab, len, width);

        ab_appendstr(ab, "\x1b[m");
    }
}

static int redraw_pending = 1;                          // the first frame is always due

void editor_request_redraw(void) {
    redraw_pending = 1;
}

int editor_take_redraw(void) {
    int due = redraw_pending;
    redraw_pending = 0;
    return due;
}

int render_next_timeout_ms(void) {
    if (E.statusmsg[0] == '\0') return -1;

    long age = (long)(time(NULL) - E.statusmsg_time);
    if (age >= STATUSMSG_TIMEOUT) return -1;            // already gone; nothing left to wait for

    // +1 so the wake lands after the second ticks over rather than on the boundary
    return (int)((STATUSMSG_TIMEOUT - age) * 1000 + 1);
}

void render_free(void) {
    free(linebuf);
    linebuf = NULL;
    linebuf_cap = 0;

    free(hlbuf);
    hlbuf = NULL;
    hlbuf_cap = 0;
}

/**
 * Digits needed for the highest line number, plus one space of separation,
 * floored at GUTTER_MIN so the text area does not shift as the file grows.
 */
static int compute_gutter(void) {
    int n = buffer_num_lines();
    int digits = 1;
    while (n >= 10) {
        n /= 10;
        digits++;
    }

    int w = digits + GUTTER_SEP_W;
    if (w < GUTTER_MIN) w = GUTTER_MIN;
    if (w > editor_view()->cols - 1) w = editor_view()->cols - 1;   // pathologically narrow pane
    if (w < 0) w = 0;
    return w;
}

// width available for line content, once the gutter has taken its share
static int text_cols(void) {
    View *v = editor_view();
    int w = v->cols - v->gutter;
    return w < 1 ? 1 : w;
}

void editor_scroll(void) {
    View *v = editor_view();

    // deleting text, here or in another pane onto the same file, can leave the
    // cursor past the end; clamp before anything reads through it, since this
    // runs for every pane before every render
    if (v->cy > buffer_num_lines() - 1) v->cy = buffer_num_lines() - 1;
    if (v->cy < 0) v->cy = 0;
    if (v->cx > buffer_line_length(v->cy)) v->cx = buffer_line_length(v->cy);
    if (v->rowoff > v->cy) v->rowoff = v->cy;

    v->gutter = compute_gutter();                       // before any screen-column arithmetic
    v->rx = buffer_cx_to_rx(v->cy, v->cx);

    if (v->cy < v->rowoff) {
        v->rowoff = v->cy;
    }
    if (v->cy >= v->rowoff + v->rows) {
        v->rowoff = v->cy - v->rows + 1;
    }
    if (v->rx < v->coloff) {
        v->coloff = v->rx;
    }
    if (v->rx >= v->coloff + text_cols()) {
        v->coloff = v->rx - text_cols() + 1;
    }
}

/**
 * Right-aligned line number, then " | " to hold the text clear of the numbers.
 * Dimmed so it recedes, with the cursor's own line left undimmed. The separator
 * is ASCII on purpose: one byte per column keeps the gutter width and the screen
 * column arithmetic the same number.
 *
 * The reset is unconditional: skip it and the color bleeds into the line's text.
 */
/* A letter rather than an underline: true squiggles need undercurl, which many
 * terminals do not support, and a marker in a column that already exists needs
 * nothing at all. */
static char diag_marker(int severity) {
    switch (severity) {
        case LSP_ERROR:   return 'E';
        case LSP_WARNING: return 'W';
        case LSP_INFO:    return 'i';
        default:          return 'h';
    }
}

static int diag_color(int severity) {
    switch (severity) {
        case LSP_ERROR:   return 31;                    // red
        case LSP_WARNING: return 33;                    // yellow
        case LSP_INFO:    return 34;                    // blue
        default:          return 90;                    // grey
    }
}

static void draw_gutter(abuf *ab, int filerow) {
    int gutter = editor_view()->gutter;
    if (gutter == 0) return;

    char num[32];
    if (filerow < 0) {
        snprintf(num, sizeof(num), "%*s", gutter, "");      // blank gutter for tilde rows
        ab_appendstr(ab, num);
        return;
    }

    int current = (filerow == editor_view()->cy);
    int severity = lsp_line_severity(editor_buffer(), filerow);

    snprintf(num, sizeof(num), "%*d ", gutter - GUTTER_SEP_W, filerow + 1);
    if (!current) ab_appendstr(ab, "\x1b[90m");
    ab_appendstr(ab, num);
    if (!current) ab_appendstr(ab, "\x1b[39m");

    /* The separator column doubles as the diagnostic marker. It costs no extra
     * width — so nothing shifts when a diagnostic appears or clears — and it
     * sits right where the eye already is when reading a line number. */
    if (severity) {
        char buf[24];
        int n = snprintf(buf, sizeof(buf), "\x1b[1;%dm%c\x1b[m ",
                         diag_color(severity), diag_marker(severity));
        ab_append(ab, buf, (size_t)n);
    } else {
        if (!current) ab_appendstr(ab, "\x1b[90m");
        ab_appendstr(ab, "| ");
        if (!current) ab_appendstr(ab, "\x1b[39m");
    }
}

/**
 * Paints the extra cursors and the selection onto the *copy* of the highlight
 * array, never the buffer's own. They are view state, not file state, and this
 * is the same override mechanism search matches use.
 */
static void mark_extra_cursors(int filerow, int rsize) {
    View *v = editor_view();

    for (int i = 0; i < v->nextra; i++) {
        if (v->extra[i].cy != filerow) continue;

        int rx = buffer_cx_to_rx(filerow, v->extra[i].cx);
        if (rx >= 0 && rx < rsize) hlbuf[rx] = HL_CURSOR;
    }

    bpos s, e;
    if (!editor_selection(&s, &e) || filerow < s.cy || filerow > e.cy) return;

    int from = filerow == s.cy ? buffer_cx_to_rx(filerow, s.cx) : 0;
    int to = filerow == e.cy ? buffer_cx_to_rx(filerow, e.cx) : rsize;
    for (int rx = from; rx < to && rx < rsize; rx++) hlbuf[rx] = HL_CURSOR;
}

/**
 * Whether the cell just past the last character of the row is inverted: an extra
 * cursor parked there, or a selection carrying on past the line break. There is
 * no character there to invert, so the highlight array cannot carry it and the
 * row loop has to draw a cell of its own — without which a cursor added onto a
 * shorter line vanishes, and a selected blank line looks unselected.
 */
static int extra_cursor_at_eol(int filerow, int rsize) {
    View *v = editor_view();

    for (int i = 0; i < v->nextra; i++) {
        if (v->extra[i].cy != filerow) continue;
        if (buffer_cx_to_rx(filerow, v->extra[i].cx) == rsize) return 1;
    }

    bpos s, e;
    return editor_selection(&s, &e) && filerow >= s.cy && filerow < e.cy;
}

static void draw_overlay_row(abuf *ab, int y) {
    char line[512];

    if (y == 0) {
        snprintf(line, sizeof(line), " %s > %s", overlay->title, overlay->query);
    } else {
        int idx = overlay->top + (y - 1);
        if (idx >= overlay->nitems) {
            snprintf(line, sizeof(line), " ");
        } else {
            snprintf(line, sizeof(line), " %s %s",
                     idx == overlay->selected ? ">" : " ", overlay->items[idx]);
        }
    }

    int len = (int)strlen(line);
    if (len > E.screencols) len = E.screencols;

    int selected = (y > 0 && overlay->top + (y - 1) == overlay->selected &&
                    overlay->top + (y - 1) < overlay->nitems);

    ab_appendstr(ab, selected || y == 0 ? "\x1b[7m" : "\x1b[100m");
    ab_append(ab, line, (size_t)len);
    ab_pad(ab, len, E.screencols);
    ab_appendstr(ab, "\x1b[m");
}

/* Screen rows the overlay owns this frame, full width, over the top of every
 * pane. Panes leave these rows alone rather than drawing text for the overlay to
 * paint over: no cell may be written twice in one frame, because some terminals
 * (Windows Terminal, through ConPTY) show a frame before it is finished, and the
 * list redraws ten times a second, so the text underneath would flash through. */
static int overlay_rows(void) {
    if (overlay == NULL) return 0;
    return overlay->height + 1 < E.screenrows ? overlay->height + 1 : E.screenrows;
}

static void draw_overlay(abuf *ab) {
    for (int y = 0; y < overlay_rows(); y++) {
        move_to(ab, y, 0);
        draw_overlay_row(ab, y);
    }
}

/**
 * The text rows of the pane being drawn. Each row is placed by absolute addressing and
 * padded out to the pane's width, because a pane beside another shares its
 * screen lines: "\r\n" and erase-to-end-of-line would trample the neighbour.
 * The padding does the erasing instead, and still avoids a full-screen flash.
 */
static void draw_rows(abuf *ab, const char *divider) {
    View *v = editor_view();
    int covered = overlay_rows();

    for (int y = 0; y < v->rows; y++) {
        if (v->top + y < covered) continue;
        move_to(ab, v->top + y, v->left);
        int filerow = y + v->rowoff;
        int used = v->gutter;

        if (filerow >= buffer_num_lines()) {
            draw_gutter(ab, -1);
        } else {
            draw_gutter(ab, filerow);

            int linelen = buffer_line_rlength(filerow);
            linebuf_reserve((size_t)linelen + 1);
            buffer_get_rline(filerow, linebuf, linebuf_cap);
            buffer_get_hl(filerow, hlbuf, hlbuf_cap);
            mark_extra_cursors(filerow, linelen);

            int visible = linelen - v->coloff;
            if (visible < 0) visible = 0;
            if (visible > text_cols()) visible = text_cols();

            // emit a color escape only where the type changes: one per
            // character would multiply the size of every frame several times
            int current_color = -1;
            for (int j = 0; j < visible; j++) {
                int at = v->coloff + j;
                int color = syntax_color(hlbuf[at]);

                if (color != current_color) {
                    current_color = color;
                    if (color == 7) {
                        ab_appendstr(ab, "\x1b[7m");    // reverse video, an extra cursor
                    } else {
                        char cbuf[16];
                        int clen = snprintf(cbuf, sizeof(cbuf), "\x1b[27m\x1b[%dm", color);
                        ab_append(ab, cbuf, (size_t)clen);
                    }
                }
                ab_append(ab, &linebuf[at], 1);
            }
            if (current_color != -1) ab_appendstr(ab, "\x1b[27m\x1b[39m");

            used += visible;

            // the cell just past the end of the line, for a cursor parked there
            int eol_col = linelen - v->coloff;
            if (eol_col >= 0 && eol_col < text_cols() && extra_cursor_at_eol(filerow, linelen)) {
                ab_appendstr(ab, "\x1b[7m \x1b[27m");
                used++;
            }
        }

        ab_pad(ab, used, v->cols);
        ab_appendstr(ab, divider);
    }
}

/* Inverted for the focused pane; the others' go grey, which is how you tell
 * which one the keyboard is talking to. */
static void draw_status_bar(abuf *ab, int focused, const char *divider) {
    View *v = editor_view();
    if (v->top + v->rows < overlay_rows()) return;
    move_to(ab, v->top + v->rows, v->left);
    ab_appendstr(ab, focused ? "\x1b[7m" : "\x1b[90;7m");

    Buffer *b = editor_buffer();

    char status[120], rstatus[80];
    int len = snprintf(status, sizeof(status), "[%d/%d] %.20s - %d lines %s",
                       buffer_active() + 1, buffer_count(),
                       b->filename ? b->filename : "[No Name]", buffer_num_lines(),
                       undo_is_dirty() ? "(modified)" : "");

    const char *lsp = lsp_status(b);
    char cursors[32] = "";
    if (v->nextra > 0) snprintf(cursors, sizeof(cursors), "%d cursors | ", v->nextra + 1);

    int rlen = snprintf(rstatus, sizeof(rstatus), "%s%s%s%s | %d/%d",
                        cursors,
                        lsp ? lsp : "", lsp ? " | " : "",
                        syntax_filetype(), v->cy + 1, buffer_num_lines());

    if (len > v->cols) len = v->cols;
    ab_append(ab, status, (size_t)len);

    while (len < v->cols) {
        if (v->cols - len == rlen) {
            ab_append(ab, rstatus, (size_t)rlen);
            break;
        }
        ab_appendstr(ab, " ");
        len++;
    }

    ab_appendstr(ab, "\x1b[m");
    ab_appendstr(ab, divider);
}

static void draw_message_bar(abuf *ab) {
    ab_appendstr(ab, "\x1b[K");

    const char *legend = command_legend();
    int llen = (int)strlen(legend);

    /* The right side is a fresh message, or with nothing else to say, the
     * diagnostic under the cursor: the gutter marker says *that* something is
     * wrong on a line, this says what. A message answers what you just did, so it
     * may push the legend off; a diagnostic is always there, so it gets only the
     * room the legend leaves. */
    const char *right = NULL;
    int color = 0;
    if (E.statusmsg[0] && time(NULL) - E.statusmsg_time < STATUSMSG_TIMEOUT) {
        right = E.statusmsg;
    } else {
        View *v = editor_view();
        right = lsp_line_message(editor_buffer(), v->cy);
        if (right) color = diag_color(lsp_line_severity(editor_buffer(), v->cy));
    }
    int rlen = right ? (int)strlen(right) : 0;

    if (llen > E.screencols) llen = E.screencols;
    int room = E.screencols - llen - 2;                 // two spaces keep them from running together
    if (rlen > room) {
        if (color) rlen = room > 0 ? room : 0;          // the diagnostic is cut short
        else llen = 0;                                  // the legend steps aside, whole
    }
    if (rlen > E.screencols) rlen = E.screencols;

    if (llen) {
        ab_appendstr(ab, "\x1b[2m");                    // dim, so a message stands out beside it
        ab_append(ab, legend, (size_t)llen);
        ab_appendstr(ab, "\x1b[22m");
    }
    if (rlen == 0) return;

    for (int col = llen; col < E.screencols - rlen; col++) ab_appendstr(ab, " ");

    if (color) {
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
        ab_append(ab, buf, (size_t)n);
    }
    ab_append(ab, right, (size_t)rlen);
    if (color) ab_appendstr(ab, "\x1b[39m");
}

void editor_refresh_screen(void) {
    size_t len;
    char *frame = render_frame(&len);
    Write(STDOUT_FILENO, frame, len);                   // exactly one write per frame
    free(frame);
}

char *render_frame(size_t *len) {
    abuf ab = ABUF_INIT;

    ab_appendstr(&ab, "\x1b[?25l");                     // hide cursor while drawing

    /* Everything that draws a pane reads it through editor_view(), so each pane
     * is drawn by making it the view for a moment, then focus is put back. */
    View *focus = editor_view();
    int n;
    View **views = pane_views(&n);
    for (int i = 0; i < n; i++) {
        E.view = views[i];
        editor_scroll();

        // a pane short of the right edge has its neighbour's divider just after it
        const char *divider = views[i]->left + views[i]->cols < E.screencols
                              ? "\x1b[90m\xe2\x94\x82\x1b[39m" : "";
        draw_rows(&ab, divider);
        draw_status_bar(&ab, views[i] == focus, divider);
    }
    E.view = focus;

    if (overlay) draw_overlay(&ab);
    move_to(&ab, E.screenrows + 1, 0);
    draw_message_bar(&ab);
    draw_popup(&ab);                                    // floats over what is already there

    View *v = editor_view();
    if (overlay) {
        move_to(&ab, 0, (int)(strlen(overlay->title) + strlen(overlay->query) + 4));
    } else {
        move_to(&ab, v->top + v->cy - v->rowoff, v->left + v->gutter + v->rx - v->coloff);
    }

    ab_appendstr(&ab, "\x1b[?25h");

    *len = ab.len;
    return ab.b;                                        // the caller owns it now
}

void editor_set_status_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
    editor_request_redraw();
}
