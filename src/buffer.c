#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buffer.h"
#include "editor.h"
#include "terminal.h"
#include "wrappers.h"

static void append_row(const char *s, size_t len) {
    E.row = Realloc(E.row, sizeof(erow) * (size_t)(E.numrows + 1));

    erow *row = &E.row[E.numrows];
    row->size = (int)len;
    row->chars = Malloc(len + 1);
    memcpy(row->chars, s, len);
    row->chars[len] = '\0';

    E.numrows++;
}

void buffer_open(const char *filename) {
    free(E.filename);
    E.filename = filename ? strdup(filename) : NULL;

    if (filename == NULL) {
        append_row("", 0);                              // never leave a zero-line buffer
        return;
    }

    FILE *fp = Fopen(filename, "r");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            if (line[linelen - 1] == '\r') E.crlf = 1;  // remembered so M2 can save it back
            linelen--;
        }
        append_row(line, (size_t)linelen);
    }

    free(line);
    Fclose(fp);

    if (E.numrows == 0) append_row("", 0);
}

void buffer_free(void) {
    for (int i = 0; i < E.numrows; i++) {
        free(E.row[i].chars);
    }
    free(E.row);
    E.row = NULL;
    E.numrows = 0;

    free(E.filename);
    E.filename = NULL;
}

int buffer_num_lines(void) {
    return E.numrows;
}

int buffer_line_length(int idx) {
    if (idx < 0 || idx >= E.numrows) return -1;
    return E.row[idx].size;
}

int buffer_get_line(int idx, char *dst, size_t dstsize) {
    if (idx < 0 || idx >= E.numrows || dstsize == 0) return -1;

    size_t len = (size_t)E.row[idx].size;
    if (len > dstsize - 1) len = dstsize - 1;

    memcpy(dst, E.row[idx].chars, len);
    dst[len] = '\0';
    return (int)len;
}
