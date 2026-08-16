#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

#include "render.h"
#include "buffer.h"
#include "editor.h"
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

static void ab_free(abuf *ab) {
    free(ab->b);
    ab->b = NULL;
    ab->len = 0;
    ab->cap = 0;
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
    if (w > E.screencols - 1) w = E.screencols - 1;     // pathologically narrow window
    if (w < 0) w = 0;
    return w;
}

// width available for line content, once the gutter has taken its share
static int text_cols(void) {
    int w = E.screencols - E.gutter;
    return w < 1 ? 1 : w;
}

void editor_scroll(void) {
    View *v = editor_view();

    // deleting rows can leave the cursor past the end; clamp before anything
    // reads through it, since this runs before every render
    if (v->cy > buffer_num_lines() - 1) v->cy = buffer_num_lines() - 1;
    if (v->cy < 0) v->cy = 0;
    if (v->rowoff > v->cy) v->rowoff = v->cy;

    E.gutter = compute_gutter();                        // before any screen-column arithmetic
    v->rx = buffer_cx_to_rx(v->cy, v->cx);

    if (v->cy < v->rowoff) {
        v->rowoff = v->cy;
    }
    if (v->cy >= v->rowoff + E.screenrows) {
        v->rowoff = v->cy - E.screenrows + 1;
    }
    if (v->rx < v->coloff) {
        v->coloff = v->rx;
    }
    if (v->rx >= v->coloff + text_cols()) {
        v->coloff = v->rx - text_cols() + 1;
    }
}

static void draw_welcome(abuf *ab) {
    char welcome[80];
    int len = snprintf(welcome, sizeof(welcome), "bIDE editor -- version %s", BIDE_VERSION);
    if (len > text_cols()) len = text_cols();

    int padding = (text_cols() - len) / 2;
    while (padding--) ab_appendstr(ab, " ");
    ab_append(ab, welcome, (size_t)len);
}

/**
 * Right-aligned line number, then " | " to hold the text clear of the numbers.
 * Dimmed so it recedes, with the cursor's own line left undimmed. The separator
 * is ASCII on purpose: one byte per column keeps the gutter width and the screen
 * column arithmetic the same number.
 *
 * The reset is unconditional: skip it and the color bleeds into the line's text.
 */
static void draw_gutter(abuf *ab, int filerow) {
    if (E.gutter == 0) return;

    char num[32];
    if (filerow < 0) {
        snprintf(num, sizeof(num), "%*s", E.gutter, "");    // blank gutter for tilde rows
        ab_appendstr(ab, num);
        return;
    }

    snprintf(num, sizeof(num), "%*d | ", E.gutter - GUTTER_SEP_W, filerow + 1);
    int current = (filerow == editor_view()->cy);
    if (!current) ab_appendstr(ab, "\x1b[90m");
    ab_appendstr(ab, num);
    if (!current) ab_appendstr(ab, "\x1b[39m");
}

/**
 * Paints the extra cursors onto the *copy* of the highlight array, never the
 * buffer's own. They are view state, not file state, and this is the same
 * override mechanism search matches use — its third consumer.
 */
static void mark_extra_cursors(int filerow, int rsize) {
    View *v = editor_view();

    for (int i = 0; i < v->nextra; i++) {
        if (v->extra[i].cy != filerow) continue;

        int rx = buffer_cx_to_rx(filerow, v->extra[i].cx);
        if (rx >= 0 && rx < rsize) hlbuf[rx] = HL_CURSOR;
    }
}

/**
 * Whether an extra cursor sits just past the last character of the row. There is
 * no character there to invert, so the highlight array cannot carry it and the
 * row loop has to draw a cell of its own — without which a cursor added onto a
 * shorter line simply vanishes while still editing.
 */
static int extra_cursor_at_eol(int filerow, int rsize) {
    View *v = editor_view();

    for (int i = 0; i < v->nextra; i++) {
        if (v->extra[i].cy != filerow) continue;
        if (buffer_cx_to_rx(filerow, v->extra[i].cx) == rsize) return 1;
    }
    return 0;
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
    for (int i = len; i < E.screencols; i++) ab_appendstr(ab, " ");
    ab_appendstr(ab, "\x1b[m");
}

static void draw_rows(abuf *ab) {
    View *v = editor_view();
    int overlay_rows = overlay ? overlay->height + 1 : 0;

    for (int y = 0; y < E.screenrows; y++) {
        // the overlay is drawn as part of the same frame, not a second write
        if (overlay && y < overlay_rows) {
            draw_overlay_row(ab, y);
            ab_appendstr(ab, "\x1b[K");
            ab_appendstr(ab, "\r\n");
            continue;
        }

        int filerow = y + v->rowoff;

        if (filerow >= buffer_num_lines()) {
            draw_gutter(ab, -1);
            if (editor_buffer()->filename == NULL && buffer_num_lines() <= 1 &&
                y == E.screenrows / 3) {
                draw_welcome(ab);
            } else {
                ab_appendstr(ab, "~");
            }
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

            // the cell just past the end of the line, for a cursor parked there
            int eol_col = linelen - v->coloff;
            if (eol_col >= 0 && eol_col < text_cols() && extra_cursor_at_eol(filerow, linelen)) {
                ab_appendstr(ab, "\x1b[7m \x1b[27m");
            }
        }

        ab_appendstr(ab, "\x1b[K");                     // clear to end of line, avoids a full-screen flash
        ab_appendstr(ab, "\r\n");
    }
}

static void draw_status_bar(abuf *ab) {
    ab_appendstr(ab, "\x1b[7m");                        // inverted video

    View *v = editor_view();
    Buffer *b = editor_buffer();

    char status[120], rstatus[80];
    int len = snprintf(status, sizeof(status), "[%d/%d] %.20s - %d lines %s",
                       buffer_active() + 1, buffer_count(),
                       b->filename ? b->filename : "[No Name]", buffer_num_lines(),
                       undo_is_dirty() ? "(modified)" : "");

    View *vv = editor_view();
    int rlen = vv->nextra > 0
        ? snprintf(rstatus, sizeof(rstatus), "%d cursors | %s | %d/%d",
                   vv->nextra + 1, syntax_filetype(), v->cy + 1, buffer_num_lines())
        : snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
                   syntax_filetype(), v->cy + 1, buffer_num_lines());

    if (len > E.screencols) len = E.screencols;
    ab_append(ab, status, (size_t)len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            ab_append(ab, rstatus, (size_t)rlen);
            break;
        }
        ab_appendstr(ab, " ");
        len++;
    }

    ab_appendstr(ab, "\x1b[m");
    ab_appendstr(ab, "\r\n");
}

static void draw_message_bar(abuf *ab) {
    ab_appendstr(ab, "\x1b[K");

    int msglen = (int)strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < STATUSMSG_TIMEOUT) {
        ab_append(ab, E.statusmsg, (size_t)msglen);
    }
}

void editor_refresh_screen(void) {
    editor_scroll();

    abuf ab = ABUF_INIT;

    ab_appendstr(&ab, "\x1b[?25l");                     // hide cursor while drawing
    ab_appendstr(&ab, "\x1b[H");

    draw_rows(&ab);
    draw_status_bar(&ab);
    draw_message_bar(&ab);

    // the one place 0-indexed state becomes 1-indexed terminal coordinates
    View *v = editor_view();
    char buf[32];
    if (overlay) {
        snprintf(buf, sizeof(buf), "\x1b[1;%dH",
                 (int)(strlen(overlay->title) + strlen(overlay->query) + 5));
    } else {
        snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                 (v->cy - v->rowoff) + 1, (v->rx - v->coloff) + E.gutter + 1);
    }
    ab_appendstr(&ab, buf);

    ab_appendstr(&ab, "\x1b[?25h");

    Write(STDOUT_FILENO, ab.b, ab.len);                 // exactly one write per frame
    ab_free(&ab);
}

void editor_set_status_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}
