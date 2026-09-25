#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>

#include "rain.h"
#include "editor.h"
#include "render.h"
#include "terminal.h"
#include "wrappers.h"

/* Tunables. A column waits up to MAX_DELAY_MS, then takes MIN..MAX_FALL_MS for
 * its tail to clear the bottom, so the whole effect lasts at most the sum. */
#define RAIN_FPS      30
#define MAX_DELAY_MS  700
#define MIN_FALL_MS   800
#define MAX_FALL_MS   1300
#define MIN_TRAIL     4
#define MAX_TRAIL     16
#define FLICKER_PCT   25        // chance per frame that a trail character changes

#define HEAD_COLOR    231       // 256-color white
static const int TRAIL_COLORS[] = {48, 46, 40, 34, 28, 22};   // bright green to dark
#define NTRAIL_COLORS (int)(sizeof(TRAIL_COLORS) / sizeof(TRAIL_COLORS[0]))

static const char GLYPHS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@#$%&?!*+=<>~^";

/* A cell's look, kept as the SGR numbers that produced it rather than resolved
 * colors: 31 and 38;5;1 can render differently (bold-as-bright applies to one
 * only), and replaying the same numbers is the only way to match exactly.
 * 256-color values are stored as 1000+n (foreground) and 2000+n (background). */
typedef struct {
    short fg, bg;
    unsigned char bold, dim, rev;
} Style;

typedef struct {
    char ch[5];                 // one column's UTF-8 bytes; "" means never drawn
    Style st;
} Cell;

static const Style PLAIN = {39, 49, 0, 0, 0};

static int style_eq(const Style *a, const Style *b) {
    return a->fg == b->fg && a->bg == b->bg &&
           a->bold == b->bold && a->dim == b->dim && a->rev == b->rev;
}

static int rnd(int lo, int hi) {
    return lo + rand() % (hi - lo + 1);
}

/* ------------------------------------------------------ frame to grid --- */

static void apply_sgr(Style *st, char *params) {
    int v[16], n = 0;
    char *p = params;
    while (n < 16) {                                    // "" parses as 0, which is a reset
        v[n++] = (int)strtol(p, &p, 10);
        if (*p != ';') break;
        p++;
    }

    for (int i = 0; i < n; i++) {
        int x = v[i];
        if (x == 0) *st = PLAIN;
        else if (x == 1) st->bold = 1;
        else if (x == 2) st->dim = 1;
        else if (x == 22) st->bold = st->dim = 0;
        else if (x == 7) st->rev = 1;
        else if (x == 27) st->rev = 0;
        else if ((x >= 30 && x <= 37) || (x >= 90 && x <= 97) || x == 39) st->fg = (short)x;
        else if ((x >= 40 && x <= 47) || (x >= 100 && x <= 107) || x == 49) st->bg = (short)x;
        else if ((x == 38 || x == 48) && i + 2 < n && v[i + 1] == 5) {
            if (x == 38) st->fg = (short)(1000 + v[i + 2]);
            else st->bg = (short)(2000 + v[i + 2]);
            i += 2;
        }
    }
}

/**
 * Plays the frame's bytes into a grid, the way a terminal would. Only has to
 * understand what render.c emits: text, CR/LF, cursor moves, erase-to-end-of-line
 * and SGR. Anything else in a CSI (cursor show/hide) draws nothing.
 */
static void parse_frame(const char *f, size_t len, Cell *grid, int rows, int cols) {
    int r = 0, c = 0;
    Style st = PLAIN;

    for (int i = 0; i < rows * cols; i++) {             // what a freshly cleared screen holds
        strcpy(grid[i].ch, " ");
        grid[i].st = PLAIN;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)f[i];

        if (b == 0x1b && i + 1 < len && f[i + 1] == '[') {
            char params[64];
            size_t j = i + 2, pn = 0;
            while (j < len && !(f[j] >= 0x40 && f[j] <= 0x7E)) {
                if (pn < sizeof(params) - 1) params[pn++] = f[j];
                j++;
            }
            params[pn] = '\0';
            if (j >= len) break;

            if (params[0] != '?') {
                if (f[j] == 'm') {
                    apply_sgr(&st, params);
                } else if (f[j] == 'H') {
                    int y = 1, x = 1;
                    sscanf(params, "%d;%d", &y, &x);
                    r = y - 1;
                    c = x - 1;
                } else if (f[j] == 'K' && r >= 0 && r < rows) {
                    for (int x = c < 0 ? 0 : c; x < cols; x++) {
                        strcpy(grid[r * cols + x].ch, " ");
                        grid[r * cols + x].st = PLAIN;
                        grid[r * cols + x].st.bg = st.bg;   // erase keeps only the background
                    }
                }
            }
            i = j;
            continue;
        }

        if (b == '\r') { c = 0; continue; }
        if (b == '\n') { if (r < rows - 1) r++; continue; }

        if ((b & 0xC0) == 0x80) {                       // UTF-8 continuation: same column
            if (r >= 0 && r < rows && c > 0 && c <= cols) {
                char *ch = grid[r * cols + c - 1].ch;
                size_t n = strlen(ch);
                if (n < sizeof(grid[0].ch) - 1) { ch[n] = (char)b; ch[n + 1] = '\0'; }
            }
            continue;
        }

        if (r >= 0 && r < rows && c >= 0 && c < cols) {
            Cell *cell = &grid[r * cols + c];
            cell->ch[0] = (char)b;
            cell->ch[1] = '\0';
            cell->st = st;
        }
        c++;
    }
}

/* ------------------------------------------------------------- output --- */

static char *out = NULL;
static size_t outlen = 0;

static void emit(const char *s, size_t n) {
    memcpy(out + outlen, s, n);
    outlen += n;
}

static void emit_style(const Style *s) {
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "\x1b[0%s%s%s", s->bold ? ";1" : "",
                     s->dim ? ";2" : "", s->rev ? ";7" : "");
    if (s->fg >= 1000) n += snprintf(buf + n, sizeof(buf) - (size_t)n, ";38;5;%d", s->fg - 1000);
    else if (s->fg != 39) n += snprintf(buf + n, sizeof(buf) - (size_t)n, ";%d", s->fg);
    if (s->bg >= 2000) n += snprintf(buf + n, sizeof(buf) - (size_t)n, ";48;5;%d", s->bg - 2000);
    else if (s->bg != 49) n += snprintf(buf + n, sizeof(buf) - (size_t)n, ";%d", s->bg);
    buf[n++] = 'm';
    emit(buf, (size_t)n);
}

/* ------------------------------------------------------------ the rain --- */

typedef struct {
    int delay_ms, fall_ms, trail;
} Drop;

void rain_play(void) {
    int rows = E.screenrows + 2;                        // the frame includes both bars
    int cols = E.screencols;

    size_t flen;
    char *frame = render_frame(&flen);

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || rows < 1 || cols < 1) {
        enter_alt_screen();
        Write(STDOUT_FILENO, frame, flen);
        free(frame);
        return;
    }

    Cell *grid = Malloc(sizeof(Cell) * (size_t)(rows * cols));
    Cell *shown = Malloc(sizeof(Cell) * (size_t)(rows * cols));
    char *glyph = Malloc((size_t)(rows * cols));
    Drop *drops = Malloc(sizeof(Drop) * (size_t)cols);
    out = Malloc((size_t)(rows * cols) * 48 + 64);      // a cursor move, a style and a char, per cell

    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    parse_frame(frame, flen, grid, rows, cols);
    for (int i = 0; i < rows * cols; i++) {
        shown[i].ch[0] = '\0';                          // the shell's text, which we can't know
        glyph[i] = GLYPHS[rnd(0, (int)sizeof(GLYPHS) - 2)];
    }

    for (int x = 0; x < cols; x++) {
        drops[x].delay_ms = rnd(0, MAX_DELAY_MS);
        drops[x].fall_ms = rnd(MIN_FALL_MS, MAX_FALL_MS);
        drops[x].trail = rnd(MIN_TRAIL, MAX_TRAIL);
    }

    Write(STDOUT_FILENO, "\x1b[?25l", 6);
    const Style head_style = {1000 + HEAD_COLOR, 49, 1, 0, 0};
    long start = MonotonicMs();
    int skipped = 0;

    for (int frameno = 1;; frameno++) {
        long t = MonotonicMs() - start;                 // clock time, so a slow terminal ends on time
        int done = 1;
        int cr = -1, cc = -1;                           // where the terminal's cursor is, if known
        Style cur = {-1, -1, 0, 0, 0};                  // matches nothing: first cell sets a style
        outlen = 0;

        for (int x = 0; x < cols; x++) {
            Drop *d = &drops[x];
            long p = t - d->delay_ms;
            if (p < 0) p = 0;
            if (p > d->fall_ms) p = d->fall_ms;
            int h = (int)((long)(rows + d->trail) * p / d->fall_ms);   // head sits at row h-1
            if (h < rows + d->trail) done = 0;

            for (int y = 0; y < rows && y < h; y++) {   // below the head: still the shell's
                int i = y * cols + x;
                int dist = h - 1 - y;                   // 0 at the head
                Cell want;

                if (dist >= d->trail) {
                    want = grid[i];                     // uncovered
                } else {
                    if (dist == 0 || rnd(1, 100) <= FLICKER_PCT) {
                        glyph[i] = GLYPHS[rnd(0, (int)sizeof(GLYPHS) - 2)];
                    }
                    want.ch[0] = glyph[i];
                    want.ch[1] = '\0';
                    if (dist == 0) {
                        want.st = head_style;
                    } else {
                        want.st = PLAIN;
                        want.st.fg = (short)(1000 + TRAIL_COLORS[dist * NTRAIL_COLORS / d->trail]);
                    }
                }

                if (strcmp(want.ch, shown[i].ch) == 0 && style_eq(&want.st, &shown[i].st)) continue;
                shown[i] = want;

                if (cr != y || cc != x) {
                    char pos[24];
                    int n = snprintf(pos, sizeof(pos), "\x1b[%d;%dH", y + 1, x + 1);
                    emit(pos, (size_t)n);
                }
                if (!style_eq(&cur, &want.st)) {
                    emit_style(&want.st);
                    cur = want.st;
                }
                emit(want.ch, strlen(want.ch));
                cr = y;
                cc = x + 1 < cols ? x + 1 : -1;         // past the last column the position is murky
            }
        }

        if (outlen) Write(STDOUT_FILENO, out, outlen); // one write per frame, changed cells only
        if (done) break;

        long wait = start + (long)frameno * 1000 / RAIN_FPS - MonotonicMs();
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        if (Poll(&pfd, 1, wait > 0 ? (int)wait : 0) > 0) {
            skipped = 1;
            break;
        }
    }

    if (skipped) tcflush(STDIN_FILENO, TCIFLUSH);       // the skip key is not also a keystroke

    // One write: wipe the main screen (so leaving shows a blank one, not rain),
    // swap to the alternate screen, and draw the frame the rain just finished.
    // The terminal takes it in one go, so nothing in between is ever shown.
    static const char HANDOFF[] = "\x1b[m\x1b[2J\x1b[H" SCREEN_ON;
    outlen = 0;
    free(out);
    out = Malloc(sizeof(HANDOFF) - 1 + flen);
    emit(HANDOFF, sizeof(HANDOFF) - 1);
    emit(frame, flen);
    Write(STDOUT_FILENO, out, outlen);

    free(out);
    out = NULL;
    free(frame);
    free(grid);
    free(shown);
    free(glyph);
    free(drops);
}
