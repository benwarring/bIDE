#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "search.h"
#include "buffer.h"
#include "editor.h"
#include "input.h"
#include "render.h"
#include "syntax.h"
#include "wrappers.h"

static int last_match = -1;     // row of the previous match, -1 for "start fresh"
static int direction = 1;

// the matched row's highlights, saved so the match coloring can be lifted again
static int saved_hl_line = -1;
static unsigned char *saved_hl = NULL;
static int saved_hl_len = 0;

// scratch for materializing one render string per search step
static char *scratch = NULL;
static size_t scratch_cap = 0;

static void scratch_reserve(size_t need) {
    if (need <= scratch_cap) return;
    scratch = Realloc(scratch, need);
    scratch_cap = need;
}

static void restore_hl(void) {
    if (saved_hl == NULL) return;

    buffer_set_hl(saved_hl_line, saved_hl, saved_hl_len);
    free(saved_hl);
    saved_hl = NULL;
    saved_hl_line = -1;
    saved_hl_len = 0;
}

/**
 * Smart case: case-insensitive unless the query contains an uppercase letter,
 * in which case the user clearly meant it.
 */
static int query_is_case_sensitive(const char *q) {
    for (const char *p = q; *p; p++) {
        if (isupper((unsigned char)*p)) return 1;
    }
    return 0;
}

static char *find_in(const char *haystack, const char *needle, int sensitive) {
    if (sensitive) return strstr(haystack, needle);

    size_t nlen = strlen(needle);
    if (nlen == 0) return (char *)haystack;

    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return (char *)p;
    }
    return NULL;
}

static void find_callback(char *query, int key) {
    restore_hl();                                       // lift the previous match's coloring

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;                                // the query changed: search from the top
        direction = 1;
    }

    int numrows = buffer_num_lines();
    if (numrows == 0 || query[0] == '\0') return;

    if (last_match == -1) direction = 1;
    int current = last_match;
    int sensitive = query_is_case_sensitive(query);

    for (int i = 0; i < numrows; i++) {
        current += direction;
        if (current == -1) current = numrows - 1;       // wrap at both ends
        else if (current == numrows) current = 0;

        int rlen = buffer_line_rlength(current);
        scratch_reserve((size_t)rlen + 1);
        buffer_get_rline(current, scratch, scratch_cap);

        // searching the render string means tab expansion is already handled and
        // the hit is in rx, which converts straight back to cx
        char *match = find_in(scratch, query, sensitive);
        if (match == NULL) continue;

        int rx = (int)(match - scratch);
        last_match = current;

        View *v = editor_view();
        v->cy = current;
        v->cx = buffer_rx_to_cx(current, rx);

        saved_hl_line = current;
        saved_hl_len = rlen;
        saved_hl = Malloc((size_t)rlen + 1);
        buffer_get_hl(current, saved_hl, (size_t)rlen + 1);
        buffer_highlight_match(current, rx, (int)strlen(query));
        return;
    }
}

void editor_find(void) {
    View *v = editor_view();

    // a search jump is navigation, not a multi-cursor edit, so it collapses
    editor_cursor_collapse();

    // stash everything, because a cancelled search must not move the user
    int saved_cx = v->cx;
    int saved_cy = v->cy;
    int saved_coloff = v->coloff;
    int saved_rowoff = v->rowoff;

    char *query = editor_prompt("Search: %s (arrows = next/prev, ESC = cancel)", find_callback);

    if (query) {
        free(query);
    } else {
        v->cx = saved_cx;
        v->cy = saved_cy;
        v->coloff = saved_coloff;
        v->rowoff = saved_rowoff;
    }

    restore_hl();
    last_match = -1;
    direction = 1;

    free(scratch);
    scratch = NULL;
    scratch_cap = 0;
}
