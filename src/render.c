#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

#include "render.h"
#include "buffer.h"
#include "editor.h"
#include "wrappers.h"

#define BIDE_VERSION "0.1.0"
#define STATUSMSG_TIMEOUT 5

typedef struct {
    char *b;
    size_t len;
} abuf;

#define ABUF_INIT {NULL, 0}

static void ab_append(abuf *ab, const char *s, size_t len) {
    ab->b = Realloc(ab->b, ab->len + len);
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
}

// scratch space for materializing one line out of the buffer per draw
static char *linebuf = NULL;
static size_t linebuf_cap = 0;

static void linebuf_reserve(size_t need) {
    if (need <= linebuf_cap) return;
    linebuf = Realloc(linebuf, need);
    linebuf_cap = need;
}

void render_free(void) {
    free(linebuf);
    linebuf = NULL;
    linebuf_cap = 0;
}

void editor_scroll(void) {
    E.rx = E.cx;                                        // equal until tabs are rendered

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

static void draw_welcome(abuf *ab) {
    char welcome[80];
    int len = snprintf(welcome, sizeof(welcome), "bIDE editor -- version %s", BIDE_VERSION);
    if (len > E.screencols) len = E.screencols;

    int padding = (E.screencols - len) / 2;
    if (padding) {
        ab_appendstr(ab, "~");
        padding--;
    }
    while (padding--) ab_appendstr(ab, " ");
    ab_append(ab, welcome, (size_t)len);
}

static void draw_rows(abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;

        if (filerow >= buffer_num_lines()) {
            if (E.filename == NULL && buffer_num_lines() <= 1 && y == E.screenrows / 3) {
                draw_welcome(ab);
            } else {
                ab_appendstr(ab, "~");
            }
        } else {
            int linelen = buffer_line_length(filerow);
            linebuf_reserve((size_t)linelen + 1);
            buffer_get_line(filerow, linebuf, linebuf_cap);

            int visible = linelen - E.coloff;
            if (visible < 0) visible = 0;
            if (visible > E.screencols) visible = E.screencols;
            if (visible > 0) ab_append(ab, &linebuf[E.coloff], (size_t)visible);
        }

        ab_appendstr(ab, "\x1b[K");                     // clear to end of line, avoids a full-screen flash
        ab_appendstr(ab, "\r\n");
    }
}

static void draw_status_bar(abuf *ab) {
    ab_appendstr(ab, "\x1b[7m");                        // inverted video

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
                       E.filename ? E.filename : "[No Name]", buffer_num_lines());
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, buffer_num_lines());

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
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
             (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
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
