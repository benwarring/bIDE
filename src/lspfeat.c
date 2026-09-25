#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "lsp.h"
#include "buffer.h"
#include "editor.h"
#include "input.h"
#include "json.h"
#include "list.h"
#include "log.h"
#include "popup.h"
#include "render.h"
#include "terminal.h"
#include "undo.h"
#include "wrappers.h"

#define REPLY_TIMEOUT_MS 2500   // how long a feature waits before giving up
#define MAX_COMPLETIONS 400
#define LINE_MAX_LEN 8192

/* ------------------------------------------------------- waiting for a reply --- */

/**
 * Some features have nothing to show until the server answers. Rather than
 * block the process, this keeps the normal loop running — polling the server,
 * redrawing, and letting Escape abandon the wait — until the reply lands or the
 * deadline passes.
 *
 * Typing is not accepted while waiting, which is the one concession: the
 * alternative is a popup that appears over text the user has since changed. The
 * wait is bounded and cancellable, so a server that never answers costs a
 * keystroke and a moment, not the session.
 */
static int wait_for_reply(const volatile int *done, int timeout_ms) {
    long deadline = MonotonicMs() + timeout_ms;

    while (!*done) {
        long left = deadline - MonotonicMs();
        if (left <= 0) return 0;

        struct pollfd fds[1 + LSP_MAX_FDS];
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        int n = 1 + lsp_pollfds(&fds[1], LSP_MAX_FDS);
        int wait = left < 50 ? (int)left : 50;

        if (Poll(fds, (nfds_t)n, wait) > 0) {
            lsp_dispatch(&fds[1], n - 1);

            if (fds[0].revents & POLLIN) {
                int c = editor_read_key();
                if (c == '\x1b') return 0;              // the user gave up on it
            }
        }
        lsp_tick();

        if (editor_take_redraw()) editor_refresh_screen();
    }
    return 1;
}

/* ------------------------------------------------------------- jump list --- */

/**
 * Where we were before following a definition. Without this, one jump into an
 * unfamiliar header strands you — a small feature that decides whether the
 * editor is usable for reading code.
 */
typedef struct {
    char *path;
    int cy, cx;
} Jump;

#define MAX_JUMPS 64
static Jump jumps[MAX_JUMPS];
static int njumps = 0;

void lsp_push_jump(void) {
    Buffer *b = editor_buffer();
    View *v = editor_view();
    if (b == NULL || b->filename == NULL) return;

    if (njumps == MAX_JUMPS) {                          // drop the oldest
        free(jumps[0].path);
        memmove(&jumps[0], &jumps[1], sizeof(Jump) * (MAX_JUMPS - 1));
        njumps--;
    }

    jumps[njumps].path = strdup(b->filename);
    jumps[njumps].cy = v->cy;
    jumps[njumps].cx = v->cx;
    njumps++;
}

int lsp_have_jumps(void) {
    return njumps > 0;
}

void lsp_jump_back(void) {
    if (njumps == 0) {
        editor_set_status_message("No jumps to return from");
        return;
    }

    Jump j = jumps[--njumps];
    buffer_open(j.path);

    View *v = editor_view();
    v->cy = j.cy;
    v->cx = j.cx;
    editor_clamp_cursor();

    free(j.path);
    editor_set_status_message("Back to %s:%d", buffer_name(buffer_active()), j.cy + 1);
}

void lsp_jump_to(const char *path, int line, int character) {
    if (path == NULL) return;

    buffer_open(path);

    Buffer *b = editor_buffer();
    View *v = editor_view();

    v->cy = line;
    v->cx = lsp_character_to_cx(lsp_server_for(b), b, line, character);
    editor_clamp_cursor();

    // land the target near the middle rather than glued to the top edge
    v->rowoff = v->cy - v->rows / 3;
    if (v->rowoff < 0) v->rowoff = 0;

    editor_request_redraw();
}

/* ------------------------------------------------------ go to definition --- */

static int def_done;
static char def_path[1024];
static int def_line, def_char;

/**
 * The response is a Location, an array of Locations, or an array of
 * LocationLinks, and which one you get depends on the server. All three have to
 * be handled; this inconsistency runs through the whole specification.
 */
static int extract_location(JsonValue *loc, char **path, int *line, int *character) {
    if (loc == NULL) return 0;

    const char *uri = json_get_str(loc, "uri", NULL);
    JsonValue *range = json_get(loc, "range");

    if (uri == NULL) {                                  // a LocationLink instead
        uri = json_get_str(loc, "targetUri", NULL);
        range = json_get(loc, "targetSelectionRange");
        if (range == NULL) range = json_get(loc, "targetRange");
    }
    if (uri == NULL || range == NULL) return 0;

    JsonValue *start = json_get(range, "start");
    *path = lsp_uri_to_path(uri);
    *line = json_get_int(start, "line", 0);
    *character = json_get_int(start, "character", 0);
    return 1;
}

static void on_definition(LspServer *s, JsonValue *result, JsonValue *error,
                          const LspRequest *req) {
    (void)s; (void)req;
    def_done = 1;
    def_path[0] = '\0';

    if (error) {
        log_line("definition error: %s", json_get_str(error, "message", "?"));
        return;
    }

    JsonValue *loc = result;
    if (json_type(result) == JSON_ARRAY) {
        if (json_len(result) == 0) return;
        loc = json_at(result, 0);                       // the first is the one to follow
    }

    char *path = NULL;
    if (!extract_location(loc, &path, &def_line, &def_char)) return;

    snprintf(def_path, sizeof(def_path), "%s", path);
    free(path);
}

void lsp_goto_definition(void) {
    Buffer *b = editor_buffer();
    View *v = editor_view();
    LspServer *s = lsp_server_for(b);

    if (!lsp_supports(s, LSP_FEAT_DEFINITION)) {
        editor_set_status_message("No definition support for this file");
        return;
    }

    char *params = lsp_position_params(s, b, v->cy, v->cx);
    if (params == NULL) return;

    LspRequest req;
    lsp_fill_request(&req, b, v->cy, v->cx);

    def_done = 0;
    int sent = lsp_request(s, "textDocument/definition", params, on_definition, &req);
    free(params);
    free(req.uri);
    if (!sent) return;

    editor_set_status_message("Looking up definition...");
    if (!wait_for_reply(&def_done, REPLY_TIMEOUT_MS)) {
        editor_set_status_message("Definition timed out");
        return;
    }

    if (def_path[0] == '\0') {
        editor_set_status_message("No definition found");
        return;
    }

    lsp_push_jump();                                    // so jump-back can return here
    lsp_jump_to(def_path, def_line, def_char);
    editor_set_status_message("%s:%d", buffer_name(buffer_active()), def_line + 1);
}

/* ------------------------------------------------------------------ hover --- */

static int hover_done;
static char *hover_text;

/** Hover contents arrive as markdown, plain text, or either of two deprecated
 *  MarkedString shapes. Everything reduces to one string here. */
static void collect_hover(JsonValue *v, char **out) {
    if (v == NULL) return;

    if (json_type(v) == JSON_STRING) {
        const char *s = json_as_str(v, "");
        size_t have = *out ? strlen(*out) : 0;
        *out = Realloc(*out, have + strlen(s) + 2);
        if (have == 0) (*out)[0] = '\0';
        strcat(*out, s);
        strcat(*out, "\n");
        return;
    }

    if (json_type(v) == JSON_ARRAY) {
        for (int i = 0; i < json_len(v); i++) collect_hover(json_at(v, i), out);
        return;
    }

    if (json_type(v) == JSON_OBJECT) {
        JsonValue *value = json_get(v, "value");        // MarkupContent or MarkedString
        collect_hover(value ? value : json_get(v, "contents"), out);
    }
}

static void on_hover(LspServer *s, JsonValue *result, JsonValue *error,
                     const LspRequest *req) {
    (void)s; (void)req;
    hover_done = 1;

    free(hover_text);
    hover_text = NULL;
    if (error || result == NULL) return;

    collect_hover(json_get(result, "contents"), &hover_text);
}

/**
 * Markdown is stripped rather than rendered. A real renderer is a rabbit hole
 * with very little payoff in a terminal; fences and headings are the only
 * syntax that actually gets in the way of reading.
 */
static int split_lines(char *text, char **lines, int max) {
    int n = 0;
    int in_fence = 0;
    char *p = text;

    while (p && *p && n < max) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';

        char *line = p;
        if (strncmp(line, "```", 3) == 0) {              // a fence marker itself is noise
            in_fence = !in_fence;
            p = nl ? nl + 1 : NULL;
            continue;
        }
        if (!in_fence) {
            while (*line == '#' || (*line == ' ' && line[1] == '#')) line++;   // heading marks
            if (*line == ' ') line++;
        }

        // blank lines at the top of the box waste the little space there is
        if (!(n == 0 && *line == '\0')) lines[n++] = line;
        p = nl ? nl + 1 : NULL;
    }

    while (n > 0 && lines[n - 1][0] == '\0') n--;        // and trailing ones
    return n;
}

void lsp_hover(void) {
    Buffer *b = editor_buffer();
    View *v = editor_view();
    LspServer *s = lsp_server_for(b);

    if (!lsp_supports(s, LSP_FEAT_HOVER)) {
        editor_set_status_message("No hover support for this file");
        return;
    }

    char *params = lsp_position_params(s, b, v->cy, v->cx);
    if (params == NULL) return;

    LspRequest req;
    lsp_fill_request(&req, b, v->cy, v->cx);

    hover_done = 0;
    int sent = lsp_request(s, "textDocument/hover", params, on_hover, &req);
    free(params);
    free(req.uri);
    if (!sent) return;

    if (!wait_for_reply(&hover_done, REPLY_TIMEOUT_MS)) {
        editor_set_status_message("Hover timed out");
        return;
    }
    if (hover_text == NULL || hover_text[0] == '\0') {
        editor_set_status_message("Nothing to show here");
        return;
    }

    char *lines[128];
    int n = split_lines(hover_text, lines, 128);
    if (n == 0) {
        editor_set_status_message("Nothing to show here");
        return;
    }

    list_message("Hover", lines, n);
    free(hover_text);
    hover_text = NULL;
}

/* ------------------------------------------------------------ completion --- */

typedef struct {
    char *label;                                        // what is shown
    char *insert;                                       // what gets typed in
    char *sort;                                         // the server's own ranking key
    int has_edit;                                       // insert replaces a range
    int e_line, e_start, e_end;                         // that range, server-counted
} Completion;

static Completion *comps;
static int ncomps;

static char **comp_labels;          // borrowed by the popup for as long as it is open
static Buffer *comp_buffer;         // which buffer the open popup belongs to
static int comp_anchor_cy;          // where the word being completed starts
static int comp_anchor_cx;

/* Bumped on every request, so a reply for a request the user has already
 * superseded can be recognized and dropped. */
static int comp_serial;
static int comp_want;

static void comps_clear(void) {
    popup_close();                  // it borrows comp_labels; it must not outlive them

    for (int i = 0; i < ncomps; i++) {
        free(comps[i].label);
        free(comps[i].insert);
        free(comps[i].sort);
    }
    free(comps);
    comps = NULL;
    ncomps = 0;

    free(comp_labels);
    comp_labels = NULL;

    /* comp_buffer and the anchor deliberately survive: they describe the request
     * that is still in flight, not the list being thrown away, and the reply
     * handler clears the old list before it checks them. */
}

/**
 * A buffer is going away while a request about it may still be unanswered.
 * Forgetting it here is what keeps the reply handler from comparing against a
 * pointer that has been freed.
 */
void lsp_completion_forget(const Buffer *b) {
    if (b != comp_buffer) return;

    comp_buffer = NULL;
    comps_clear();
}

static int by_sort_text(const void *a, const void *b) {
    const Completion *x = a, *y = b;
    // servers rank relevance in sortText; ignoring it buries the good items
    int r = strcmp(x->sort, y->sort);
    return r ? r : strcmp(x->label, y->label);
}

static void on_completion(LspServer *s, JsonValue *result, JsonValue *error,
                          const LspRequest *req) {
    (void)s;

    // the user has asked again since; this answer is about a world that is gone
    if (req->extra && atoi(req->extra) != comp_want) {
        log_line("discarding completion response for superseded request");
        return;
    }

    comps_clear();
    if (error || result == NULL) return;

    JsonValue *items = (json_type(result) == JSON_ARRAY) ? result : json_get(result, "items");
    int n = json_len(items);
    if (n == 0) return;
    if (n > MAX_COMPLETIONS) n = MAX_COMPLETIONS;       // a broad prefix can return thousands

    comps = Malloc(sizeof(Completion) * (size_t)n);
    memset(comps, 0, sizeof(Completion) * (size_t)n);

    for (int i = 0; i < n; i++) {
        JsonValue *it = json_at(items, i);
        const char *label = json_get_str(it, "label", "");

        comps[ncomps].label = strdup(label);
        comps[ncomps].sort = strdup(json_get_str(it, "sortText", label));

        /* Prefer textEdit: it is a precise range replacement, and handles the
         * cases where what gets inserted does not simply extend what was typed. */
        JsonValue *edit = json_get(it, "textEdit");
        if (edit) {
            JsonValue *range = json_get(edit, "range");
            if (range == NULL) range = json_get(edit, "replace");

            JsonValue *st = json_get(range, "start");
            JsonValue *en = json_get(range, "end");

            comps[ncomps].has_edit = 1;
            comps[ncomps].e_line = json_get_int(st, "line", 0);
            comps[ncomps].e_start = json_get_int(st, "character", 0);
            comps[ncomps].e_end = json_get_int(en, "character", 0);
            comps[ncomps].insert = strdup(json_get_str(edit, "newText", label));
        } else {
            comps[ncomps].insert = strdup(json_get_str(it, "insertText", label));
        }
        ncomps++;
    }

    qsort(comps, (size_t)ncomps, sizeof(Completion), by_sort_text);

    /* The answer describes the document as it was when the request went out, and
     * the user has been typing the whole time. It is still useful as long as they
     * are on the same word — that is the point of filtering client-side — but not
     * if they have left it. */
    Buffer *b = editor_buffer();
    View *v = editor_view();

    if (b != comp_buffer || v->cy != comp_anchor_cy || v->cx < comp_anchor_cx) {
        log_line("completion arrived after the cursor left the word; dropping");
        comps_clear();
        return;
    }

    comp_labels = Malloc(sizeof(char *) * (size_t)ncomps);
    for (int i = 0; i < ncomps; i++) comp_labels[i] = comps[i].label;

    popup_open(comp_labels, ncomps, comp_anchor_cy, comp_anchor_cx);
    lsp_completion_sync();                              // narrow to what is already typed
}

/** Replaces [start,end) on `line` with `text`, as one undoable action. */
static void apply_completion(const Completion *c) {
    Buffer *b = editor_buffer();
    View *v = editor_view();
    LspServer *s = lsp_server_for(b);

    int line = c->has_edit ? c->e_line : comp_anchor_cy;
    int from = c->has_edit ? lsp_character_to_cx(s, b, line, c->e_start) : comp_anchor_cx;
    int to   = c->has_edit ? lsp_character_to_cx(s, b, line, c->e_end) : v->cx;

    /* The server measured that range before the last few keystrokes landed, so on
     * the line the cursor is actually on it can stop short of what has been typed
     * since. Replacing as far as the cursor instead leaves no stray suffix. */
    if (line == v->cy && v->cx > to) to = v->cx;

    int len = buffer_line_length(line);
    if (len < 0) return;
    if (from < 0) from = 0;
    if (to > len) to = len;
    if (to < from) to = from;

    undo_begin_group();                                 // one keystroke, one undo

    for (int i = to - 1; i >= from; i--) buffer_delete_char(line, i);

    bpos p = {line, from};
    for (const char *t = c->insert; *t; t++) {
        if (*t == '\n') p = buffer_split_line(p.cy, p.cx);
        else p = buffer_insert_char(p.cy, p.cx, *t);
    }

    undo_end_group();

    v->cy = p.cy;
    v->cx = p.cx;
    editor_clamp_cursor();
}

void lsp_complete(void) {
    Buffer *b = editor_buffer();
    View *v = editor_view();
    LspServer *s = lsp_server_for(b);

    if (!lsp_supports(s, LSP_FEAT_COMPLETION)) {
        editor_set_status_message("No completion support for this file");
        return;
    }

    /* Where the word being completed begins. The popup hangs from it, the filter
     * is measured from it, and a plain insertText replaces from it. */
    char line[LINE_MAX_LEN];
    if (buffer_get_line(v->cy, line, sizeof(line)) < 0) return;

    int start = v->cx;
    while (start > 0 && (isalnum((unsigned char)line[start - 1]) || line[start - 1] == '_')) start--;

    char *params = lsp_position_params(s, b, v->cy, v->cx);
    if (params == NULL) return;

    LspRequest req;
    lsp_fill_request(&req, b, v->cy, v->cx);

    char serial[16];
    comp_want = ++comp_serial;
    snprintf(serial, sizeof(serial), "%d", comp_want);
    req.extra = serial;

    int sent = lsp_request(s, "textDocument/completion", params, on_completion, &req);
    free(params);
    free(req.uri);
    if (!sent) return;

    /* And that is the whole of it — no waiting. The popup appears if and when the
     * answer arrives; until then the user keeps typing into a buffer that has not
     * noticed it asked a question. A completion request that blocked would undo
     * the entire point of the non-blocking loop. */
    comp_buffer = b;
    comp_anchor_cy = v->cy;
    comp_anchor_cx = start;
}

/* --------------------------------------- the popup, driven by key dispatch --- */

int lsp_completion_active(void) {
    return popup_active();
}

/**
 * Re-narrows the open popup to whatever has been typed since it opened, or shuts
 * it if the cursor has left the word it belongs to. Called after each key has
 * been handled normally — so the keystroke reaches the buffer first and the list
 * catches up, rather than the list intercepting it.
 */
void lsp_completion_sync(void) {
    if (!popup_active()) return;

    Buffer *b = editor_buffer();
    View *v = editor_view();

    if (b != comp_buffer || v->cy != popup_anchor_cy() || v->cx < popup_anchor_cx()) {
        popup_close();
        return;
    }

    char line[LINE_MAX_LEN];
    int len = buffer_get_line(v->cy, line, sizeof(line));

    int start = popup_anchor_cx();
    int plen = v->cx - start;

    if (len < 0 || start + plen > len || plen >= (int)sizeof(line)) {
        popup_close();
        return;
    }

    char prefix[128];
    if (plen > (int)sizeof(prefix) - 1) {               // typed well past any candidate
        popup_close();
        return;
    }
    memcpy(prefix, &line[start], (size_t)plen);
    prefix[plen] = '\0';

    popup_filter(prefix);
}

/** Offers a key to the popup. Returns 1 if the popup dealt with it. */
int lsp_completion_key(int c) {
    if (!popup_active()) return 0;

    int r = popup_key(c);
    if (r == POPUP_CONSUMED) return 1;
    if (r != POPUP_ACCEPTED) return 0;                  // not ours: let it through

    int pick = popup_selection();

    // the popup borrows comp_labels, so it goes down before the edit rather than
    // being left pointing at strings comps_clear is about to free
    popup_close();
    if (pick >= 0 && pick < ncomps) apply_completion(&comps[pick]);

    comps_clear();
    return 1;
}

/**
 * The server declares which characters should bring completion up — "." and ">"
 * for C, "::" for Rust — and honoring that list rather than a hardcoded one is
 * what keeps this file free of language-specific knowledge.
 */
void lsp_completion_maybe_trigger(int c) {
    Buffer *b = editor_buffer();
    LspServer *s = lsp_server_for(b);
    if (s == NULL) return;

    const char *triggers = lsp_trigger_chars(s);
    if (triggers[0] == '\0' || c > 127) return;
    if (strchr(triggers, c) == NULL) return;

    lsp_complete();
}

/* ---------------------------------------------------------- diagnostics --- */

static void goto_diagnostic(int dir) {
    Buffer *b = editor_buffer();
    View *v = editor_view();

    int line = lsp_diag_next_line(b, v->cy, dir);
    if (line < 0) {
        editor_set_status_message("No diagnostics");
        return;
    }

    v->cy = line;
    v->cx = 0;
    editor_clamp_cursor();

    const char *msg = lsp_line_message(b, line);
    editor_set_status_message("%d: %s", line + 1, msg ? msg : "");
}

void lsp_next_diagnostic(void) { goto_diagnostic(1); }
void lsp_prev_diagnostic(void) { goto_diagnostic(-1); }

/* ------------------------------------------------------------------- log --- */

/* --------------------------------------------------------------- teardown --- */

/** Everything the feature layer holds onto between calls. */
void lsp_features_free(void) {
    comps_clear();
    popup_free();

    for (int i = 0; i < njumps; i++) free(jumps[i].path);
    njumps = 0;

    free(hover_text);
    hover_text = NULL;
}

/* ------------------------------------------------------------------- log --- */

void lsp_open_log(void) {
    const char *p = log_path();
    if (p == NULL) {
        editor_set_status_message("No log file open");
        return;
    }

    buffer_open(p);
    editor_set_status_message("LSP log — reopen after new traffic to refresh");
}
