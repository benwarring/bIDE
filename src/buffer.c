#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include "buffer.h"
#include "editor.h"
#include "lsp.h"
#include "syntax.h"
#include "terminal.h"
#include "undo.h"
#include "wrappers.h"

/* ---------------------------------------------------------------- rows --- */

/**
 * Regenerates a row's render string from its raw bytes.
 */
static void render_row(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = Malloc((size_t)row->size + (size_t)tabs * (TAB_STOP - 1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';                   // a tab is at least one column
            while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

/**
 * The single trigger for "this line changed": rebuild its render string, then
 * its highlight array. Keeping both behind one call is what stops them drifting
 * out of sync after an edit.
 */
static void update_row(int idx) {
    Buffer *b = editor_buffer();
    if (idx < 0 || idx >= b->numrows) return;
    render_row(&b->row[idx]);
    syntax_update_row(idx);
}

/**
 * Inserts a row at index `at`. The same operation as inserting a character,
 * one level up: grow, memmove the tail, write into the gap.
 */
static void insert_row(int at, const char *s, size_t len) {
    Buffer *b = editor_buffer();
    if (at < 0 || at > b->numrows) return;

    b->row = Realloc(b->row, sizeof(erow) * (size_t)(b->numrows + 1));
    memmove(&b->row[at + 1], &b->row[at], sizeof(erow) * (size_t)(b->numrows - at));

    erow *row = &b->row[at];
    row->size = (int)len;
    row->chars = Malloc(len + 1);
    memcpy(row->chars, s, len);
    row->chars[len] = '\0';
    row->rsize = 0;
    row->render = NULL;
    row->hl = NULL;
    row->hl_open_comment = 0;

    b->numrows++;                                       // count it before update_row indexes it
    update_row(at);
}

static void free_row(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

static void del_row(int at) {
    Buffer *b = editor_buffer();
    if (at < 0 || at >= b->numrows) return;

    free_row(&b->row[at]);                              // easy to forget; leaks only show under valgrind
    memmove(&b->row[at], &b->row[at + 1], sizeof(erow) * (size_t)(b->numrows - at - 1));
    b->numrows--;

    // the row that slid into this slot may now start from a different comment state
    if (at < b->numrows) syntax_update_row(at);
}

/* ---------------------------------------------------------- open/save/free - */

/* ------------------------------------------------- buffer list lifecycle --- */

/**
 * Resolved absolute path, so ./main.c, main.c and src/../src/main.c are all
 * recognized as the same file. realpath fails for a file that does not exist
 * yet, which is fine: a brand new file cannot already be open.
 */
static char *resolve(const char *filename) {
    if (filename == NULL) return NULL;

    char resolved[PATH_MAX];
    if (realpath(filename, resolved) != NULL) return strdup(resolved);
    return strdup(filename);
}

/* Zeroed wholesale rather than field by field. Every correct value here is a
 * zero or a NULL, and listing them individually means a field added later —
 * `lsp` was exactly this — is left holding garbage that only shows up as a
 * crash somewhere else entirely. */
static Buffer *buffer_new(void) {
    Buffer *b = Malloc(sizeof(Buffer));
    memset(b, 0, sizeof(*b));
    b->undo = undo_create();
    return b;
}

static void attach(Buffer *b) {
    E.bufs = Realloc(E.bufs, sizeof(Buffer *) * (size_t)(E.nbufs + 1));
    E.bufs[E.nbufs++] = b;

    View *v = Malloc(sizeof(View));
    v->buf = b;
    v->cx = v->cy = v->rx = 0;
    v->rowoff = v->coloff = 0;
    v->extra = NULL;
    v->nextra = 0;

    E.views = Realloc(E.views, sizeof(View *) * (size_t)(E.nviews + 1));
    E.views[E.nviews++] = v;

    E.active = E.nbufs - 1;                             // load into the new buffer
}

/**
 * Reads the file into the *current* buffer. Callers switch first, which is what
 * lets every row function work through editor_buffer() with no buffer argument.
 */
static void load_into_current(const char *filename) {
    Buffer *b = editor_buffer();

    b->filename = filename ? strdup(filename) : NULL;
    b->path = resolve(filename);
    syntax_select(b->filename);                         // before any row is scanned

    if (filename == NULL) {
        insert_row(0, "", 0);                           // never leave a zero-line buffer
        return;
    }

    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {                                   // a new file: start it empty
        insert_row(0, "", 0);
        return;
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            if (line[linelen - 1] == '\r') b->crlf = 1; // remembered so save can restore it
            linelen--;
        }
        insert_row(b->numrows, line, (size_t)linelen);
    }

    free(line);
    Fclose(fp);

    if (b->numrows == 0) insert_row(0, "", 0);
}

int buffer_open(const char *filename) {
    // already open? switch to it rather than opening a divergent second copy
    if (filename != NULL) {
        char *want = resolve(filename);
        for (int i = 0; i < E.nbufs; i++) {
            if (E.bufs[i]->path && strcmp(E.bufs[i]->path, want) == 0) {
                free(want);
                E.active = i;
                return i;
            }
        }
        free(want);
    }

    attach(buffer_new());
    load_into_current(filename);
    lsp_open_document(editor_buffer());                 // starts a server if this language has one
    return E.active;
}

int buffer_count(void) {
    return E.nbufs;
}

int buffer_active(void) {
    return E.active;
}

const char *buffer_name(int idx) {
    if (idx < 0 || idx >= E.nbufs) return "";
    return E.bufs[idx]->filename ? E.bufs[idx]->filename : "[No Name]";
}

int buffer_is_dirty(int idx) {
    if (idx < 0 || idx >= E.nbufs) return 0;

    int prev = E.active;
    E.active = idx;
    int dirty = undo_is_dirty();
    E.active = prev;
    return dirty;
}

void buffer_switch(int idx) {
    if (idx < 0 || idx >= E.nbufs) return;
    E.active = idx;                                     // cursor and scroll ride along in the View
}

void buffer_cycle(int delta) {
    if (E.nbufs == 0) return;
    E.active = ((E.active + delta) % E.nbufs + E.nbufs) % E.nbufs;
}

static void free_buffer(Buffer *b) {
    lsp_close_document(b);                              // tell the server before the contents go
    lsp_free_document(b);

    for (int i = 0; i < b->numrows; i++) {
        free_row(&b->row[i]);                           // chars, render and hl for every line
    }
    free(b->row);
    free(b->filename);
    free(b->path);
    undo_destroy(b->undo);                              // both stacks and their records
    free(b);
}

void buffer_close(int idx) {
    if (idx < 0 || idx >= E.nbufs) return;

    free_buffer(E.bufs[idx]);
    free(E.views[idx]->extra);
    free(E.views[idx]);

    memmove(&E.bufs[idx], &E.bufs[idx + 1], sizeof(Buffer *) * (size_t)(E.nbufs - idx - 1));
    memmove(&E.views[idx], &E.views[idx + 1], sizeof(View *) * (size_t)(E.nviews - idx - 1));
    E.nbufs--;
    E.nviews--;

    // closing the last buffer leaves an empty one rather than no buffer at all,
    // so nothing downstream has to cope with a dangling active index
    if (E.nbufs == 0) {
        E.active = 0;
        buffer_open(NULL);
        return;
    }
    if (E.active >= E.nbufs) E.active = E.nbufs - 1;
}

void buffer_free(void) {
    for (int i = 0; i < E.nbufs; i++) {
        free_buffer(E.bufs[i]);
        free(E.views[i]->extra);
        free(E.views[i]);
    }
    free(E.bufs);
    free(E.views);
    E.bufs = NULL;
    E.views = NULL;
    E.nbufs = E.nviews = E.active = 0;
}

void buffer_set_filename(const char *filename) {
    Buffer *b = editor_buffer();

    // the server knows this document by its old URI; that document is going away
    lsp_close_document(b);
    lsp_free_document(b);

    free(b->filename);
    free(b->path);
    b->filename = filename ? strdup(filename) : NULL;
    b->path = resolve(filename);

    syntax_select(b->filename);                         // save-as can change the language
    syntax_update_all();

    lsp_open_document(b);                               // and may be a different language entirely
}

/**
 * Serializes every line plus its terminator into one allocation, one pass.
 */
static char *rows_to_string(size_t *buflen) {
    Buffer *b = editor_buffer();
    size_t nl_len = b->crlf ? 2 : 1;

    size_t totlen = 0;
    for (int j = 0; j < b->numrows; j++) {
        totlen += (size_t)b->row[j].size + nl_len;
    }

    char *buf = Malloc(totlen);
    char *p = buf;
    for (int j = 0; j < b->numrows; j++) {
        memcpy(p, b->row[j].chars, (size_t)b->row[j].size);
        p += b->row[j].size;
        if (b->crlf) *p++ = '\r';
        *p++ = '\n';
    }

    *buflen = totlen;
    return buf;
}

long buffer_save(void) {
    Buffer *b = editor_buffer();
    if (b->filename == NULL) return -1;

    size_t len;
    char *buf = rows_to_string(&len);

    /* ftruncate to the exact length before writing, rather than O_TRUNC.
     * O_TRUNC empties the file and *then* writes, so a write that fails partway
     * has already destroyed the original. This shrinks the vulnerable window to
     * the write itself. The stronger fix is write-to-temp plus rename (atomic on
     * POSIX), but rename replaces the inode: it breaks hard links, resolves
     * symlinks to the wrong place, and drops ownership and permission bits
     * unless they are explicitly copied first. Revisit when this editor is
     * trusted with files that matter. */
    int fd = Open(b->filename, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        free(buf);
        return -1;
    }

    if (Ftruncate(fd, (off_t)len) == -1) {
        int saved = errno;
        Close(fd);
        free(buf);
        errno = saved;
        return -1;
    }

    if (Writen(fd, buf, len) == -1) {
        int saved = errno;
        Close(fd);
        free(buf);
        errno = saved;
        return -1;
    }

    if (Close(fd) == -1) {
        free(buf);
        return -1;
    }

    free(buf);
    undo_mark_saved();                                  // this undo depth is now the clean state
    return (long)len;
}

/* --------------------------------------------------------------- reads --- */

int buffer_num_lines(void) {
    Buffer *b = editor_buffer();
    return b ? b->numrows : 0;
}

int buffer_line_length(int idx) {
    Buffer *b = editor_buffer();
    if (idx < 0 || idx >= b->numrows) return -1;
    return b->row[idx].size;
}

int buffer_line_rlength(int idx) {
    Buffer *b = editor_buffer();
    if (idx < 0 || idx >= b->numrows) return -1;
    return b->row[idx].rsize;
}

static int copy_out(const char *src, int srclen, char *dst, size_t dstsize) {
    size_t len = (size_t)srclen;
    if (len > dstsize - 1) len = dstsize - 1;

    memcpy(dst, src, len);
    dst[len] = '\0';
    return (int)len;
}

int buffer_get_line(int idx, char *dst, size_t dstsize) {
    Buffer *b = editor_buffer();
    if (idx < 0 || idx >= b->numrows || dstsize == 0) return -1;
    return copy_out(b->row[idx].chars, b->row[idx].size, dst, dstsize);
}

int buffer_num_lines_of(const Buffer *b) {
    return b ? b->numrows : 0;
}

int buffer_get_line_of(const Buffer *b, int idx, char *dst, size_t dstsize) {
    if (b == NULL || idx < 0 || idx >= b->numrows || dstsize == 0) return -1;
    return copy_out(b->row[idx].chars, b->row[idx].size, dst, dstsize);
}

char *buffer_text_of(const Buffer *b, size_t *len) {
    if (b == NULL) {
        if (len) *len = 0;
        return strdup("");
    }

    size_t total = 0;
    for (int i = 0; i < b->numrows; i++) total += (size_t)b->row[i].size + 1;

    char *buf = Malloc(total + 1);
    char *p = buf;
    for (int i = 0; i < b->numrows; i++) {
        memcpy(p, b->row[i].chars, (size_t)b->row[i].size);
        p += b->row[i].size;
        *p++ = '\n';                                    // the protocol wants \n whatever the file uses
    }
    *p = '\0';

    if (len) *len = (size_t)(p - buf);
    return buf;
}

int buffer_get_rline(int idx, char *dst, size_t dstsize) {
    Buffer *b = editor_buffer();
    if (idx < 0 || idx >= b->numrows || dstsize == 0) return -1;
    return copy_out(b->row[idx].render, b->row[idx].rsize, dst, dstsize);
}

int buffer_get_hl(int idx, unsigned char *dst, size_t dstsize) {
    Buffer *b = editor_buffer();
    if (idx < 0 || idx >= b->numrows || dstsize == 0) return -1;

    size_t len = (size_t)b->row[idx].rsize;
    if (len > dstsize) len = dstsize;
    memcpy(dst, b->row[idx].hl, len);
    return (int)len;
}

void buffer_set_hl(int idx, const unsigned char *src, int len) {
    Buffer *b = editor_buffer();
    if (idx < 0 || idx >= b->numrows || src == NULL) return;
    if (len > b->row[idx].rsize) len = b->row[idx].rsize;
    memcpy(b->row[idx].hl, src, (size_t)len);
}

void buffer_highlight_match(int idx, int rstart, int rlen) {
    Buffer *b = editor_buffer();
    if (idx < 0 || idx >= b->numrows) return;
    if (rstart < 0 || rstart >= b->row[idx].rsize) return;
    if (rstart + rlen > b->row[idx].rsize) rlen = b->row[idx].rsize - rstart;
    memset(&b->row[idx].hl[rstart], HL_MATCH, (size_t)rlen);
}

int buffer_char_at(int cy, int cx) {
    Buffer *b = editor_buffer();
    if (cy < 0 || cy >= b->numrows) return -1;
    if (cx < 0 || cx >= b->row[cy].size) return -1;
    return (unsigned char)b->row[cy].chars[cx];
}

int buffer_cx_to_rx(int idx, int cx) {
    Buffer *b = editor_buffer();
    if (idx < 0 || idx >= b->numrows) return 0;
    erow *row = &b->row[idx];

    int rx = 0;
    for (int j = 0; j < cx && j < row->size; j++) {
        if (row->chars[j] == '\t') {
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);     // advance to just before the next tab stop
        }
        rx++;
    }
    return rx;
}

int buffer_rx_to_cx(int idx, int rx) {
    Buffer *b = editor_buffer();
    if (idx < 0 || idx >= b->numrows) return 0;
    erow *row = &b->row[idx];

    int cur_rx = 0;
    for (int cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') {
            cur_rx += (TAB_STOP - 1) - (cur_rx % TAB_STOP);
        }
        cur_rx++;
        if (cur_rx > rx) return cx;
    }
    return row->size;
}

/* ------------------------------------------------------------ mutations --- */

static buffer_hook_fn hook = NULL;
static void *hook_ctx = NULL;

void buffer_set_hook(buffer_hook_fn fn, void *ctx) {
    hook = fn;
    hook_ctx = ctx;
}

/* Fired after the mutation lands, so a hook that reads the buffer sees the new
 * state. Paired with undo_record: one reports the inverse, the other the shift.
 *
 * This is also where the language server learns the document changed. Putting it
 * here rather than at the keystroke level means undo, redo and completion
 * insertion all count as edits without any of them having to remember to say so. */
static void notify(undo_type op, int cy, int cx) {
    if (hook) hook(op, cy, cx, hook_ctx);
    lsp_touch_document(editor_buffer());
}

bpos buffer_insert_char(int cy, int cx, int c) {
    Buffer *b = editor_buffer();
    if (cy < 0) cy = 0;
    if (cy >= b->numrows) insert_row(b->numrows, "", 0); // typing into an empty buffer creates a line
    if (cy >= b->numrows) cy = b->numrows - 1;

    erow *row = &b->row[cy];
    if (cx < 0) cx = 0;
    if (cx > row->size) cx = row->size;

    undo_record(UNDO_INSERT_CHAR, cy, cx, (char)c);

    row->chars = Realloc(row->chars, (size_t)row->size + 2);  // +1 char, +1 terminator
    memmove(&row->chars[cx + 1], &row->chars[cx], (size_t)(row->size - cx) + 1);
    row->chars[cx] = (char)c;
    row->size++;
    update_row(cy);

    notify(UNDO_INSERT_CHAR, cy, cx);
    return (bpos){cy, cx + 1};
}

bpos buffer_delete_char(int cy, int cx) {
    Buffer *b = editor_buffer();
    if (cy < 0 || cy >= b->numrows) return (bpos){cy, cx};

    erow *row = &b->row[cy];
    if (cx < 0 || cx >= row->size) return (bpos){cy, cx};

    undo_record(UNDO_DELETE_CHAR, cy, cx, row->chars[cx]);  // capture the byte before it goes

    memmove(&row->chars[cx], &row->chars[cx + 1], (size_t)(row->size - cx));
    row->size--;                                        // allocation is left alone; it will grow again
    update_row(cy);

    notify(UNDO_DELETE_CHAR, cy, cx);
    return (bpos){cy, cx};
}

bpos buffer_split_line(int cy, int cx) {
    Buffer *b = editor_buffer();
    if (cy < 0 || cy >= b->numrows) return (bpos){cy, cx};

    erow *row = &b->row[cy];
    if (cx < 0) cx = 0;
    if (cx > row->size) cx = row->size;

    undo_record(UNDO_SPLIT_LINE, cy, cx, 0);

    insert_row(cy + 1, &row->chars[cx], (size_t)(row->size - cx));

    row = &b->row[cy];                                  // insert_row may have realloc'd the array
    row->size = cx;
    row->chars[cx] = '\0';
    update_row(cy);

    notify(UNDO_SPLIT_LINE, cy, cx);
    return (bpos){cy + 1, 0};
}

bpos buffer_join_lines(int cy) {
    Buffer *b = editor_buffer();
    if (cy < 0 || cy + 1 >= b->numrows) {
        int len = buffer_line_length(cy);               // -1 when cy is out of range
        return (bpos){cy, len < 0 ? 0 : len};           // never hand back a negative column
    }

    erow *row = &b->row[cy];
    erow *next = &b->row[cy + 1];

    int seam = row->size;                               // captured BEFORE appending: this is where the cursor belongs

    undo_record(UNDO_JOIN_LINES, cy, seam, 0);          // the seam is where a re-split must cut

    row->chars = Realloc(row->chars, (size_t)row->size + (size_t)next->size + 1);
    memcpy(&row->chars[row->size], next->chars, (size_t)next->size);
    row->size += next->size;
    row->chars[row->size] = '\0';
    update_row(cy);

    del_row(cy + 1);

    notify(UNDO_JOIN_LINES, cy, seam);                  // the seam is what a position on the
    return (bpos){cy, seam};                            // pulled-up line has to be offset by
}
