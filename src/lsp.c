#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <sys/wait.h>

#include "lsp.h"
#include "buffer.h"
#include "editor.h"
#include "json.h"
#include "log.h"
#include "render.h"
#include "wrappers.h"

#define CHANGE_DEBOUNCE_MS 200  // idle time before a didChange goes out
#define READ_CHUNK 16384
#define MAX_PENDING 256
#define LINE_MAX_LEN 8192

/* --------------------------------------------------------- server table --- */

/**
 * Which server to run for which files. Adding a language is adding a row —
 * there is deliberately no language-specific code anywhere else in the editor.
 */
typedef struct {
    const char *lang;                                   // the protocol's languageId
    const char *const *exts;
    const char *argv[6];
} ServerSpec;

static const char *const C_EXTS[]  = {".c", ".h", ".cc", ".cpp", ".hpp", ".cxx", NULL};
static const char *const PY_EXTS[] = {".py", NULL};

static const ServerSpec SERVERS[] = {
    {"c",      C_EXTS,  {"clangd", "--background-index", NULL}},
    {"python", PY_EXTS, {"pylsp", NULL}},
};

#define NSERVERS (int)(sizeof(SERVERS) / sizeof(SERVERS[0]))

/* Files that mark a project root, most specific first. A server given the wrong
 * root behaves badly in ways that look like our bug rather than its own. */
static const char *ROOT_MARKERS[] = {
    "compile_commands.json", ".git", "Cargo.toml", "go.mod", "pyproject.toml", NULL
};

/* ---------------------------------------------------------------- state --- */

typedef struct {
    int id;
    lsp_reply_fn fn;
    LspRequest ctx;
} Pending;

/** One diagnostic, already converted to our own byte-indexed positions. */
typedef struct {
    int line;
    int severity;
    char *message;
} Diag;

struct LspServer {
    const ServerSpec *spec;
    pid_t pid;
    int in_fd, out_fd, err_fd;
    char *root;

    char *inbuf;                                        // framing accumulator
    size_t inlen, incap;

    int next_id;
    Pending pending[MAX_PENDING];
    int npending;

    int initialized;
    int failed;

    /* Only the capabilities we act on are kept: the response is freed with its
     * message, and storing a whole JSON tree to read four fields from it later
     * is a lifetime problem in exchange for nothing. */
    int can_definition, can_hover, can_completion;
    char *trigger_chars;
    int utf16;                                          // server counts UTF-16 units, not bytes
};

/** Per-buffer server state. Opaque outside this file. */
struct LspDoc {
    LspServer *server;
    char *uri;
    int version;
    int opened;                                         // didOpen has gone out

    long dirty_at;                                      // monotonic ms of the last edit
    int pending_change;

    Diag *diags;
    int ndiags;
};

static LspServer *servers[LSP_MAX_SERVERS];
static int nservers = 0;

/* ------------------------------------------------------------- utilities --- */

static const char *path_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (dot == NULL || (slash && dot < slash)) return "";
    return dot;
}

static const ServerSpec *spec_for_path(const char *path) {
    if (path == NULL) return NULL;

    const char *ext = path_ext(path);
    if (ext[0] == '\0') return NULL;

    for (int i = 0; i < NSERVERS; i++) {
        for (int j = 0; SERVERS[i].exts[j]; j++) {
            if (strcmp(ext, SERVERS[i].exts[j]) == 0) return &SERVERS[i];
        }
    }
    return NULL;
}

/** Walks up from the file looking for a project marker; the file's own
 *  directory is the fallback. */
static char *find_root(const char *path) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);

    char *slash = strrchr(dir, '/');
    if (slash == NULL) return strdup(".");
    *slash = '\0';

    char fallback[PATH_MAX];
    snprintf(fallback, sizeof(fallback), "%s", dir);

    while (1) {
        for (int i = 0; ROOT_MARKERS[i]; i++) {
            char probe[PATH_MAX];
            int n = snprintf(probe, sizeof(probe), "%s/%s", dir, ROOT_MARKERS[i]);
            if (n > 0 && n < (int)sizeof(probe) && access(probe, F_OK) == 0) return strdup(dir);
        }

        slash = strrchr(dir, '/');
        if (slash == NULL || slash == dir) break;
        *slash = '\0';
    }
    return strdup(fallback);
}

/* ------------------------------------------------------------------ URIs --- */

static int uri_unreserved(int c) {
    return isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
}

char *lsp_path_to_uri(const char *path) {
    if (path == NULL) return NULL;

    char abs[PATH_MAX];
    if (realpath(path, abs) == NULL) snprintf(abs, sizeof(abs), "%s", path);

    char *uri = Malloc(strlen("file://") + strlen(abs) * 3 + 1);   // worst case every byte is %XX
    strcpy(uri, "file://");
    char *o = uri + 7;

    for (const unsigned char *p = (const unsigned char *)abs; *p; p++) {
        // a space or a non-ASCII byte left raw makes the server treat it as a
        // different document, silently
        if (uri_unreserved(*p)) *o++ = (char)*p;
        else o += sprintf(o, "%%%02X", *p);
    }
    *o = '\0';
    return uri;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *lsp_uri_to_path(const char *uri) {
    if (uri == NULL) return NULL;
    if (strncmp(uri, "file://", 7) == 0) uri += 7;

    char *out = Malloc(strlen(uri) + 1);
    char *o = out;

    while (*uri) {
        if (*uri == '%' && hexval(uri[1]) >= 0 && hexval(uri[2]) >= 0) {
            *o++ = (char)(hexval(uri[1]) * 16 + hexval(uri[2]));
            uri += 3;
        } else {
            *o++ = *uri++;
        }
    }
    *o = '\0';
    return out;
}

char *lsp_buffer_uri(const Buffer *b) {
    if (b == NULL || b->path == NULL) return NULL;
    return lsp_path_to_uri(b->path);
}

/* -------------------------------------------------- position conversion --- */

/** Bytes in the UTF-8 sequence starting with this lead byte. */
static int utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if (c < 0xC0) return 1;                             // stray continuation byte: treat as one
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    return 4;
}

/**
 * LSP counts a position's `character` in UTF-16 code units unless the server
 * agrees to utf-8. That is the identity for ASCII, which is exactly why getting
 * it wrong stays invisible until a file has an accent in a comment and then
 * every position is quietly off.
 */
int lsp_cx_to_character(const LspServer *s, const Buffer *b, int cy, int cx) {
    if (s == NULL || !s->utf16) return cx;

    char line[LINE_MAX_LEN];
    int len = buffer_get_line_of(b, cy, line, sizeof(line));
    if (len < 0) return cx;
    if (cx > len) cx = len;

    int units = 0;
    for (int i = 0; i < cx; ) {
        int seq = utf8_seq_len((unsigned char)line[i]);
        units += (seq == 4) ? 2 : 1;                    // astral characters are a surrogate pair
        i += seq;
    }
    return units;
}

int lsp_character_to_cx(const LspServer *s, const Buffer *b, int cy, int character) {
    if (s == NULL || !s->utf16) return character;

    char line[LINE_MAX_LEN];
    int len = buffer_get_line_of(b, cy, line, sizeof(line));
    if (len < 0) return character;

    int units = 0, i = 0;
    while (i < len && units < character) {
        int seq = utf8_seq_len((unsigned char)line[i]);
        units += (seq == 4) ? 2 : 1;
        i += seq;
    }
    return i;
}

/* --------------------------------------------------------------- sending --- */

/**
 * Frames and writes one message. Content-Length is a *byte* count, not a
 * character count; the difference shows up as soon as a path or an identifier
 * contains non-ASCII.
 */
static int send_raw(LspServer *s, const char *json, size_t len) {
    if (s == NULL || s->failed || s->in_fd < 0) return 0;

    char header[64];
    int hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);

    log_message("-->", json, len);

    if (Writen(s->in_fd, header, (size_t)hlen) == -1 ||
        Writen(s->in_fd, json, len) == -1) {
        // SIGPIPE is ignored, so a dead server surfaces here as EPIPE instead of
        // killing the editor outright
        log_line("write to %s failed (%s) — server marked failed",
                 s->spec->argv[0], strerror(errno));
        s->failed = 1;
        return 0;
    }
    return 1;
}

void lsp_notify(LspServer *s, const char *method, const char *params) {
    if (s == NULL || s->failed) return;

    JsonW w;
    jw_init(&w);
    jw_obj(&w);
    jw_kstr(&w, "jsonrpc", "2.0");
    jw_kstr(&w, "method", method);                      // no id: a notification never gets a reply
    jw_key(&w, "params");
    jw_raw(&w, params ? params : "{}");
    jw_obj_end(&w);

    send_raw(s, w.b, w.len);
    jw_free(&w);
}

static void request_free(LspRequest *r) {
    free(r->uri);
    free(r->extra);
    r->uri = NULL;
    r->extra = NULL;
}

static void flush_pending_change(LspServer *s);

int lsp_request(LspServer *s, const char *method, const char *params,
                lsp_reply_fn on_reply, const LspRequest *ctx) {
    if (s == NULL || s->failed || !s->initialized) return 0;
    if (s->npending >= MAX_PENDING) return 0;

    // a request about the document's contents must not go out while the server
    // still holds an older copy: the answer would be confidently wrong
    flush_pending_change(s);

    int id = s->next_id++;

    Pending *p = &s->pending[s->npending++];
    p->id = id;
    p->fn = on_reply;
    memset(&p->ctx, 0, sizeof(p->ctx));
    if (ctx) {
        p->ctx = *ctx;
        p->ctx.uri = ctx->uri ? strdup(ctx->uri) : NULL;
        p->ctx.extra = ctx->extra ? strdup(ctx->extra) : NULL;
    }

    JsonW w;
    jw_init(&w);
    jw_obj(&w);
    jw_kstr(&w, "jsonrpc", "2.0");
    jw_kint(&w, "id", id);
    jw_kstr(&w, "method", method);
    jw_key(&w, "params");
    jw_raw(&w, params ? params : "{}");
    jw_obj_end(&w);

    int ok = send_raw(s, w.b, w.len);
    jw_free(&w);

    if (!ok) {
        request_free(&p->ctx);
        s->npending--;
    }
    return ok;
}

/* -------------------------------------------------------- request context --- */

void lsp_fill_request(LspRequest *req, const Buffer *b, int cy, int cx) {
    memset(req, 0, sizeof(*req));
    req->uri = lsp_buffer_uri(b);
    req->version = (b && b->lsp) ? b->lsp->version : 0;
    req->cy = cy;
    req->cx = cx;
}

/**
 * Whether an answer still applies. The user may have moved, edited, switched
 * buffers or closed the file since the request went out; acting on an answer
 * about a world that no longer exists is the characteristic bug of this
 * milestone.
 */
int lsp_request_still_valid(const LspRequest *req) {
    Buffer *b = editor_buffer();
    View *v = editor_view();
    if (b == NULL || b->lsp == NULL || req->uri == NULL) return 0;

    if (strcmp(b->lsp->uri, req->uri) != 0) return 0;   // switched buffers
    if (b->lsp->version != req->version) return 0;      // edited since
    if (v->cy != req->cy || v->cx != req->cx) return 0; // moved since
    return 1;
}

char *lsp_position_params(LspServer *s, const Buffer *b, int cy, int cx) {
    char *uri = lsp_buffer_uri(b);
    if (uri == NULL) return NULL;

    JsonW w;
    jw_init(&w);
    jw_obj(&w);
      jw_key(&w, "textDocument");
      jw_obj(&w); jw_kstr(&w, "uri", uri); jw_obj_end(&w);
      jw_key(&w, "position");
      jw_obj(&w);
        jw_kint(&w, "line", cy);
        jw_kint(&w, "character", lsp_cx_to_character(s, b, cy, cx));
      jw_obj_end(&w);
    jw_obj_end(&w);

    free(uri);
    return w.b;                                         // caller frees
}

/* ---------------------------------------------------------- diagnostics --- */

static void diags_clear(LspDoc *d) {
    for (int i = 0; i < d->ndiags; i++) free(d->diags[i].message);
    free(d->diags);
    d->diags = NULL;
    d->ndiags = 0;
}

static LspDoc *doc_for_uri(const char *uri) {
    for (int i = 0; i < E.nbufs; i++) {
        LspDoc *d = E.bufs[i]->lsp;
        if (d && d->uri && strcmp(d->uri, uri) == 0) return d;
    }
    return NULL;
}

/**
 * A publication replaces every diagnostic for that document — it is a full
 * replacement, not an append.
 *
 * Diagnostics for a version we have already moved past are dropped rather than
 * shown at drifting positions. That is the cleaner of the two choices: brief
 * gaps while typing, never a marker pointing at the wrong line.
 */
static void store_diagnostics(const char *uri, int version, JsonValue *arr) {
    LspDoc *d = doc_for_uri(uri);
    if (d == NULL) return;                              // for a document we do not have open

    if (version >= 0 && version != d->version) {
        log_line("dropping diagnostics for v%d (document is at v%d)", version, d->version);
        return;
    }

    diags_clear(d);

    int n = json_len(arr);
    if (n == 0) return;

    d->diags = Malloc(sizeof(Diag) * (size_t)n);
    for (int i = 0; i < n; i++) {
        JsonValue *item = json_at(arr, i);
        JsonValue *start = json_get(json_get(item, "range"), "start");

        d->diags[d->ndiags].line = json_get_int(start, "line", 0);
        d->diags[d->ndiags].severity = json_get_int(item, "severity", LSP_ERROR);
        d->diags[d->ndiags].message = strdup(json_get_str(item, "message", ""));
        d->ndiags++;
    }
    log_line("%d diagnostics for %s", d->ndiags, uri);
}

int lsp_line_severity(const Buffer *b, int line) {
    if (b == NULL || b->lsp == NULL) return 0;

    int worst = 0;
    for (int i = 0; i < b->lsp->ndiags; i++) {
        if (b->lsp->diags[i].line != line) continue;
        // severity numbers run 1 (error) to 4 (hint), so the lowest is the worst
        if (worst == 0 || b->lsp->diags[i].severity < worst) worst = b->lsp->diags[i].severity;
    }
    return worst;
}

const char *lsp_line_message(const Buffer *b, int line) {
    if (b == NULL || b->lsp == NULL) return NULL;

    const char *best = NULL;
    int worst = 0;
    for (int i = 0; i < b->lsp->ndiags; i++) {
        if (b->lsp->diags[i].line != line) continue;
        if (worst == 0 || b->lsp->diags[i].severity < worst) {
            worst = b->lsp->diags[i].severity;
            best = b->lsp->diags[i].message;
        }
    }
    return best;
}

const char *lsp_status(const Buffer *b) {
    static char out[64];
    if (b == NULL || b->lsp == NULL || b->lsp->server == NULL) return NULL;

    LspServer *s = b->lsp->server;
    if (s->failed) {
        snprintf(out, sizeof(out), "%s: down", s->spec->argv[0]);
        return out;
    }
    if (!s->initialized) {
        snprintf(out, sizeof(out), "%s: starting", s->spec->argv[0]);
        return out;
    }

    int errs = 0, warns = 0;
    for (int i = 0; i < b->lsp->ndiags; i++) {
        if (b->lsp->diags[i].severity == LSP_ERROR) errs++;
        else if (b->lsp->diags[i].severity == LSP_WARNING) warns++;
    }

    if (errs || warns) snprintf(out, sizeof(out), "%d✖ %d⚠", errs, warns);
    else snprintf(out, sizeof(out), "%s", s->spec->argv[0]);
    return out;
}

/** Line of the next diagnostic strictly after `from`, wrapping. -1 if none. */
int lsp_diag_next_line(const Buffer *b, int from, int dir) {
    if (b == NULL || b->lsp == NULL || b->lsp->ndiags == 0) return -1;

    int best = -1;
    for (int i = 0; i < b->lsp->ndiags; i++) {
        int line = b->lsp->diags[i].line;
        if (dir > 0 && line > from) {
            if (best == -1 || line < best) best = line;
        } else if (dir < 0 && line < from) {
            if (best == -1 || line > best) best = line;
        }
    }
    if (best != -1) return best;

    // wrap: the extreme in the direction of travel
    best = b->lsp->diags[0].line;
    for (int i = 1; i < b->lsp->ndiags; i++) {
        int line = b->lsp->diags[i].line;
        if (dir > 0 ? line < best : line > best) best = line;
    }
    return best;
}

/* ---------------------------------------------------- incoming dispatch --- */

/**
 * A request *from* the server. Most deserve only a stub, but they must all be
 * answered: an unanswered request leaves some servers waiting indefinitely.
 */
static void handle_server_request(LspServer *s, JsonValue *msg, const char *method) {
    JsonValue *idv = json_get(msg, "id");

    JsonW w;
    jw_init(&w);
    jw_obj(&w);
    jw_kstr(&w, "jsonrpc", "2.0");
    jw_key(&w, "id");
    if (json_type(idv) == JSON_STRING) jw_str(&w, json_as_str(idv, ""));
    else jw_int(&w, json_as_int(idv, 0));

    if (strcmp(method, "workspace/configuration") == 0) {
        int n = json_len(json_get(json_get(msg, "params"), "items"));
        jw_key(&w, "result");
        jw_arr(&w);
        for (int i = 0; i < (n > 0 ? n : 1); i++) jw_null(&w);   // no configuration to offer
        jw_arr_end(&w);
    } else if (strcmp(method, "client/registerCapability") == 0 ||
               strcmp(method, "client/unregisterCapability") == 0) {
        jw_key(&w, "result");
        jw_null(&w);
    } else if (strcmp(method, "window/workDoneProgress/create") == 0) {
        jw_key(&w, "result");
        jw_null(&w);
    } else {
        jw_key(&w, "error");
        jw_obj(&w);
        jw_kint(&w, "code", -32601);                    // MethodNotFound
        jw_kstr(&w, "message", "not implemented by bIDE");
        jw_obj_end(&w);
    }

    jw_obj_end(&w);
    send_raw(s, w.b, w.len);
    jw_free(&w);
}

static void handle_message(LspServer *s, const char *json, size_t len) {
    log_message("<--", json, len);

    JsonValue *msg = json_parse(json, len);
    if (msg == NULL) {
        log_line("unparseable message from %s", s->spec->argv[0]);
        return;
    }

    const char *method = json_get_str(msg, "method", NULL);

    if (method && json_has(msg, "id")) {                // request from the server
        handle_server_request(s, msg, method);
        json_free(msg);
        return;
    }

    if (method) {                                       // notification from the server
        if (strcmp(method, "textDocument/publishDiagnostics") == 0) {
            JsonValue *p = json_get(msg, "params");
            store_diagnostics(json_get_str(p, "uri", ""),
                              json_get_int(p, "version", -1),
                              json_get(p, "diagnostics"));
            editor_request_redraw();
        } else if (strcmp(method, "window/showMessage") == 0) {
            const char *text = json_get_str(json_get(msg, "params"), "message", "");
            editor_set_status_message("%s: %s", s->spec->argv[0], text);
        } else if (strcmp(method, "window/logMessage") == 0) {
            log_line("%s: %s", s->spec->argv[0],
                     json_get_str(json_get(msg, "params"), "message", ""));
        }
        json_free(msg);
        return;
    }

    // otherwise a response: match it to what it answers
    int id = json_get_int(msg, "id", -1);
    for (int i = 0; i < s->npending; i++) {
        if (s->pending[i].id != id) continue;

        Pending p = s->pending[i];
        memmove(&s->pending[i], &s->pending[i + 1],
                sizeof(Pending) * (size_t)(s->npending - i - 1));
        s->npending--;

        if (p.fn) p.fn(s, json_get(msg, "result"), json_get(msg, "error"), &p.ctx);
        request_free(&p.ctx);
        json_free(msg);
        return;
    }

    log_line("response id=%d matched no pending request", id);
    json_free(msg);
}

/* ------------------------------------------------------------- receiving --- */

/**
 * Consumes every complete message the accumulator holds.
 *
 * A message may arrive split across several reads and several may arrive in
 * one; assuming a read is a message works in every test and fails the first
 * time the server says something long.
 */
static void consume_messages(LspServer *s) {
    while (1) {
        char *head_end = NULL;
        for (size_t i = 0; i + 4 <= s->inlen; i++) {
            if (memcmp(&s->inbuf[i], "\r\n\r\n", 4) == 0) {
                head_end = &s->inbuf[i];
                break;
            }
        }
        if (head_end == NULL) return;                   // headers incomplete

        size_t head_len = (size_t)(head_end - s->inbuf);
        size_t body_at = head_len + 4;

        // unknown headers are skipped rather than treated as errors: Content-Type
        // is optional but does appear
        long content_len = -1;
        for (size_t i = 0; i < head_len; ) {
            size_t j = i;
            while (j < head_len && s->inbuf[j] != '\r') j++;

            if (j - i > 15 && strncasecmp(&s->inbuf[i], "Content-Length:", 15) == 0) {
                content_len = strtol(&s->inbuf[i + 15], NULL, 10);
            }
            i = j + 2;
        }

        if (content_len < 0) {                          // unusable frame: resync past it
            log_line("frame with no Content-Length; skipping %zu bytes", body_at);
            memmove(s->inbuf, s->inbuf + body_at, s->inlen - body_at);
            s->inlen -= body_at;
            continue;
        }

        if (s->inlen < body_at + (size_t)content_len) return;   // body incomplete

        handle_message(s, s->inbuf + body_at, (size_t)content_len);

        size_t total = body_at + (size_t)content_len;
        memmove(s->inbuf, s->inbuf + total, s->inlen - total);
        s->inlen -= total;
        // and round again: another whole message may already be buffered
    }
}

static void server_read(LspServer *s) {
    while (1) {
        if (s->inlen + READ_CHUNK + 1 > s->incap) {
            s->incap = (s->inlen + READ_CHUNK + 1) * 2;
            s->inbuf = Realloc(s->inbuf, s->incap);
        }

        ssize_t n = read(s->out_fd, s->inbuf + s->inlen, READ_CHUNK);
        if (n > 0) {
            s->inlen += (size_t)n;
            continue;                                   // drain until it would block
        }
        if (n == 0) {
            log_line("%s closed its output; server is gone", s->spec->argv[0]);
            s->failed = 1;
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;

        log_line("read from %s failed: %s", s->spec->argv[0], strerror(errno));
        s->failed = 1;
        return;
    }

    consume_messages(s);
}

static void server_read_stderr(LspServer *s) {
    char buf[READ_CHUNK];

    while (1) {
        ssize_t n = read(s->err_fd, buf, sizeof(buf));
        if (n > 0) {
            log_stderr(s->spec->argv[0], buf, (size_t)n);   // where servers explain themselves
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

/* -------------------------------------------------------------- launch --- */

static LspServer *server_start(const ServerSpec *spec, const char *root) {
    if (nservers >= LSP_MAX_SERVERS) return NULL;

    int to_child[2], from_child[2], err_pipe[2];
    if (Pipe(to_child) == -1) return NULL;
    if (Pipe(from_child) == -1) {
        Close(to_child[0]); Close(to_child[1]);
        return NULL;
    }
    if (Pipe(err_pipe) == -1) {
        Close(to_child[0]); Close(to_child[1]);
        Close(from_child[0]); Close(from_child[1]);
        return NULL;
    }

    pid_t pid = Fork();
    if (pid == -1) {
        Close(to_child[0]); Close(to_child[1]);
        Close(from_child[0]); Close(from_child[1]);
        Close(err_pipe[0]); Close(err_pipe[1]);
        return NULL;
    }

    if (pid == 0) {
        Dup2(to_child[0], STDIN_FILENO);
        Dup2(from_child[1], STDOUT_FILENO);
        Dup2(err_pipe[1], STDERR_FILENO);

        /* The child closes its copies too. Leaving the write end of its own
         * stdout pipe open anywhere means that pipe never reports EOF, so a
         * reader waits forever for a process that already exited — the classic
         * pipe bug, whose symptom is a hang with nothing to work from. */
        Close(to_child[0]); Close(to_child[1]);
        Close(from_child[0]); Close(from_child[1]);
        Close(err_pipe[0]); Close(err_pipe[1]);

        Execvp(spec->argv[0], (char *const *)(void *)spec->argv);
        _exit(127);                                     // exec failed: the parent will see EOF
    }

    Close(to_child[0]);                                 // parent drops the ends it does not use
    Close(from_child[1]);
    Close(err_pipe[1]);

    LspServer *s = Malloc(sizeof(LspServer));
    memset(s, 0, sizeof(*s));
    s->spec = spec;
    s->pid = pid;
    s->in_fd = to_child[1];
    s->out_fd = from_child[0];
    s->err_fd = err_pipe[0];
    s->root = strdup(root);
    s->next_id = 1;
    s->utf16 = 1;                                       // the protocol default until told otherwise

    // with poll deciding when to read, non-blocking guarantees a read never
    // stalls the editor part-way through a message
    SetNonblock(s->out_fd);
    SetNonblock(s->err_fd);

    servers[nservers++] = s;
    log_line("started %s (pid %d) root=%s", spec->argv[0], (int)pid, root);
    return s;
}

/* ------------------------------------------------------------ lifecycle --- */

static void send_did_open(LspServer *s, LspDoc *d, const Buffer *b);

static void on_initialize_reply(LspServer *s, JsonValue *result, JsonValue *error,
                                const LspRequest *req) {
    (void)req;

    if (error != NULL || result == NULL) {
        log_line("initialize failed: %s", json_get_str(error, "message", "no result"));
        s->failed = 1;
        editor_set_status_message("%s failed to initialize", s->spec->argv[0]);
        return;
    }

    JsonValue *caps = json_get(result, "capabilities");

    /* Honor what the server actually offers rather than assuming. LSP 3.17 lets
     * the client ask for utf-8 positions; most modern servers agree, and the
     * ones that do not make us convert. */
    const char *enc = json_get_str(result, "positionEncoding",
                                   json_get_str(caps, "positionEncoding", "utf-16"));
    s->utf16 = (strcmp(enc, "utf-8") != 0);

    s->can_definition = json_has(caps, "definitionProvider") &&
                        json_get_bool(caps, "definitionProvider", 1);
    s->can_hover      = json_has(caps, "hoverProvider") &&
                        json_get_bool(caps, "hoverProvider", 1);
    s->can_completion = json_has(caps, "completionProvider");

    JsonValue *trig = json_get(json_get(caps, "completionProvider"), "triggerCharacters");
    if (json_len(trig) > 0) {
        char buf[64];
        size_t n = 0;
        for (int i = 0; i < json_len(trig) && n < sizeof(buf) - 1; i++) {
            for (const char *p = json_as_str(json_at(trig, i), ""); *p && n < sizeof(buf) - 1; p++) {
                buf[n++] = *p;
            }
        }
        buf[n] = '\0';
        s->trigger_chars = strdup(buf);
    }

    s->initialized = 1;
    lsp_notify(s, "initialized", "{}");

    log_line("%s ready: encoding=%s definition=%d hover=%d completion=%d triggers=[%s]",
             s->spec->argv[0], enc, s->can_definition, s->can_hover, s->can_completion,
             s->trigger_chars ? s->trigger_chars : "");

    // documents opened before the handshake finished can be announced now
    for (int i = 0; i < E.nbufs; i++) {
        LspDoc *d = E.bufs[i]->lsp;
        if (d && d->server == s && !d->opened) send_did_open(s, d, E.bufs[i]);
    }

    editor_set_status_message("%s ready", s->spec->argv[0]);
}

static void send_initialize(LspServer *s) {
    char *root_uri = lsp_path_to_uri(s->root);

    JsonW p;
    jw_init(&p);
    jw_obj(&p);
      jw_kint(&p, "processId", (long)getpid());
      jw_kstr(&p, "rootUri", root_uri);
      jw_key(&p, "clientInfo");
      jw_obj(&p); jw_kstr(&p, "name", "bIDE"); jw_kstr(&p, "version", "0.1.0"); jw_obj_end(&p);

      /* Declare only what is actually implemented: advertising a capability we
       * do not have means the server sends messages we would mishandle. */
      jw_key(&p, "capabilities");
      jw_obj(&p);
        jw_key(&p, "general");
        jw_obj(&p);
          jw_key(&p, "positionEncodings");
          jw_arr(&p); jw_str(&p, "utf-8"); jw_str(&p, "utf-16"); jw_arr_end(&p);
        jw_obj_end(&p);

        jw_key(&p, "textDocument");
        jw_obj(&p);
          jw_key(&p, "synchronization");
          jw_obj(&p);
            jw_kbool(&p, "dynamicRegistration", 0);
            jw_kbool(&p, "willSave", 0);
            jw_kbool(&p, "didSave", 0);
          jw_obj_end(&p);

          jw_key(&p, "publishDiagnostics");
          jw_obj(&p); jw_kbool(&p, "relatedInformation", 0); jw_obj_end(&p);

          jw_key(&p, "definition");
          jw_obj(&p); jw_kbool(&p, "linkSupport", 1); jw_obj_end(&p);

          jw_key(&p, "hover");
          jw_obj(&p);
            jw_key(&p, "contentFormat");
            jw_arr(&p); jw_str(&p, "plaintext"); jw_str(&p, "markdown"); jw_arr_end(&p);
          jw_obj_end(&p);

          jw_key(&p, "completion");
          jw_obj(&p);
            jw_key(&p, "completionItem");
            jw_obj(&p);
              jw_kbool(&p, "snippetSupport", 0);        // snippets are deferred, so say so
            jw_obj_end(&p);
          jw_obj_end(&p);
        jw_obj_end(&p);
      jw_obj_end(&p);
    jw_obj_end(&p);

    /* Sent by hand rather than through lsp_request, which refuses to send
     * anything before the handshake it is itself part of. */
    int id = s->next_id++;
    Pending *pend = &s->pending[s->npending++];
    memset(pend, 0, sizeof(*pend));
    pend->id = id;
    pend->fn = on_initialize_reply;

    JsonW msg;
    jw_init(&msg);
    jw_obj(&msg);
    jw_kstr(&msg, "jsonrpc", "2.0");
    jw_kint(&msg, "id", id);
    jw_kstr(&msg, "method", "initialize");
    jw_key(&msg, "params");
    jw_raw(&msg, p.b);
    jw_obj_end(&msg);

    send_raw(s, msg.b, msg.len);

    jw_free(&msg);
    jw_free(&p);
    free(root_uri);
}

static LspServer *find_server(const ServerSpec *spec, const char *root) {
    for (int i = 0; i < nservers; i++) {
        if (servers[i]->spec == spec && !servers[i]->failed &&
            strcmp(servers[i]->root, root) == 0) {
            return servers[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------- document sync --- */

static void send_did_open(LspServer *s, LspDoc *d, const Buffer *b) {
    size_t len;
    char *text = buffer_text_of(b, &len);

    JsonW w;
    jw_init(&w);
    jw_obj(&w);
      jw_key(&w, "textDocument");
      jw_obj(&w);
        jw_kstr(&w, "uri", d->uri);
        jw_kstr(&w, "languageId", s->spec->lang);
        jw_kint(&w, "version", d->version);
        jw_kstr(&w, "text", text);
      jw_obj_end(&w);
    jw_obj_end(&w);

    lsp_notify(s, "textDocument/didOpen", w.b);
    jw_free(&w);
    free(text);
    d->opened = 1;
}

/**
 * Full document sync rather than incremental ranges. Incremental is faster and
 * is the wrong trade here: a range that is ever slightly wrong diverges the
 * server's copy silently and permanently, and every feature misbehaves
 * afterwards with no visible cause.
 */
static void send_did_change(LspServer *s, LspDoc *d, const Buffer *b) {
    size_t len;
    char *text = buffer_text_of(b, &len);

    d->version++;

    JsonW w;
    jw_init(&w);
    jw_obj(&w);
      jw_key(&w, "textDocument");
      jw_obj(&w);
        jw_kstr(&w, "uri", d->uri);
        jw_kint(&w, "version", d->version);
      jw_obj_end(&w);
      jw_key(&w, "contentChanges");
      jw_arr(&w);
        jw_obj(&w); jw_kstr(&w, "text", text); jw_obj_end(&w);   // the whole document
      jw_arr_end(&w);
    jw_obj_end(&w);

    lsp_notify(s, "textDocument/didChange", w.b);
    jw_free(&w);
    free(text);

    d->pending_change = 0;
    d->dirty_at = 0;
}

static void flush_pending_change(LspServer *s) {
    for (int i = 0; i < E.nbufs; i++) {
        Buffer *b = E.bufs[i];
        if (b->lsp == NULL || b->lsp->server != s) continue;
        if (!b->lsp->pending_change || !b->lsp->opened) continue;
        send_did_change(s, b->lsp, b);
    }
}

void lsp_open_document(Buffer *b) {
    if (b == NULL || b->path == NULL || b->lsp != NULL) return;

    const ServerSpec *spec = spec_for_path(b->path);
    if (spec == NULL) return;                           // no server for this language

    char *root = find_root(b->path);
    LspServer *s = find_server(spec, root);
    if (s == NULL) {
        s = server_start(spec, root);
        if (s == NULL) {
            log_line("could not start %s", spec->argv[0]);
            free(root);
            return;
        }
        send_initialize(s);
    }
    free(root);

    LspDoc *d = Malloc(sizeof(LspDoc));
    memset(d, 0, sizeof(*d));
    d->server = s;
    d->uri = lsp_buffer_uri(b);
    d->version = 1;
    b->lsp = d;

    if (s->initialized) send_did_open(s, d, b);         // otherwise the handshake sends it
}

void lsp_touch_document(Buffer *b) {
    if (b == NULL || b->lsp == NULL) return;
    b->lsp->pending_change = 1;
    b->lsp->dirty_at = MonotonicMs();
}

void lsp_close_document(Buffer *b) {
    if (b == NULL || b->lsp == NULL) return;

    LspDoc *d = b->lsp;
    if (d->opened && d->server && !d->server->failed) {
        JsonW w;
        jw_init(&w);
        jw_obj(&w);
          jw_key(&w, "textDocument");
          jw_obj(&w); jw_kstr(&w, "uri", d->uri); jw_obj_end(&w);
        jw_obj_end(&w);
        lsp_notify(d->server, "textDocument/didClose", w.b);
        jw_free(&w);
    }
}

void lsp_free_document(Buffer *b) {
    if (b == NULL) return;
    lsp_completion_forget(b);                           // before the pointer becomes stale
    if (b->lsp == NULL) return;

    diags_clear(b->lsp);
    free(b->lsp->uri);
    free(b->lsp);
    b->lsp = NULL;
}

/* ------------------------------------------------------- the event loop --- */

int lsp_pollfds(struct pollfd *fds, int max) {
    int n = 0;
    for (int i = 0; i < nservers && n + 2 <= max; i++) {
        if (servers[i]->failed) continue;

        fds[n].fd = servers[i]->out_fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;

        fds[n].fd = servers[i]->err_fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    return n;
}

void lsp_dispatch(struct pollfd *fds, int n) {
    for (int i = 0; i < n; i++) {
        if (fds[i].revents == 0) continue;

        for (int j = 0; j < nservers; j++) {
            LspServer *s = servers[j];
            if (s->failed) continue;

            if (fds[i].fd == s->err_fd) {
                server_read_stderr(s);
                break;
            }
            if (fds[i].fd == s->out_fd) {
                server_read(s);
                editor_request_redraw();
                break;
            }
        }
    }
}

/**
 * Whether this document has a debounced change still waiting to go out.
 *
 * The timeout calculation and the flush have to agree on this *exactly*. If the
 * timeout reports a change due now for a document the flush then declines to
 * send, poll is handed a zero timeout it will be handed again on the next pass,
 * and the loop spins at full CPU for the rest of the session — no crash, no
 * hang, nothing on screen, just a busy core. A server that has died is precisely
 * that case, so its state is part of the question rather than a separate check
 * one of the two callers can forget.
 */
static int change_due(const Buffer *b, long *dirty_at) {
    LspDoc *d = b->lsp;
    if (d == NULL || d->server == NULL || d->server->failed) return 0;
    if (!d->opened || !d->pending_change || d->dirty_at == 0) return 0;

    *dirty_at = d->dirty_at;
    return 1;
}

int lsp_next_timeout_ms(void) {
    long now = MonotonicMs();
    int best = -1;

    for (int i = 0; i < E.nbufs; i++) {
        long dirty_at;
        if (!change_due(E.bufs[i], &dirty_at)) continue;

        long due = dirty_at + CHANGE_DEBOUNCE_MS - now;
        if (due < 0) due = 0;
        if (best == -1 || due < best) best = (int)due;
    }
    return best;
}

void lsp_tick(void) {
    long now = MonotonicMs();

    for (int i = 0; i < E.nbufs; i++) {
        Buffer *b = E.bufs[i];

        long dirty_at;
        if (!change_due(b, &dirty_at)) continue;

        if (now - dirty_at >= CHANGE_DEBOUNCE_MS) {
            send_did_change(b->lsp->server, b->lsp, b);
        }
    }

    // reap whatever exited, so a server that crashes does not become a zombie
    for (int i = 0; i < nservers; i++) {
        LspServer *s = servers[i];
        if (s->pid <= 0) continue;

        int status;
        if (Waitpid(s->pid, &status, WNOHANG) == s->pid) {
            log_line("%s exited (status %d)", s->spec->argv[0],
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            s->pid = -1;
            if (!s->failed) {
                s->failed = 1;
                editor_set_status_message("%s exited; language features are off",
                                          s->spec->argv[0]);
            }
            editor_request_redraw();
        }
    }
}

/* -------------------------------------------------------------- shutdown --- */

static void server_free(LspServer *s) {
    for (int i = 0; i < s->npending; i++) request_free(&s->pending[i].ctx);

    if (s->in_fd >= 0) Close(s->in_fd);
    if (s->out_fd >= 0) Close(s->out_fd);
    if (s->err_fd >= 0) Close(s->err_fd);

    free(s->trigger_chars);
    free(s->root);
    free(s->inbuf);
    free(s);
}

/**
 * shutdown request, then the exit notification, then close and reap. Skipping
 * the handshake leaves some servers' caches corrupted — but every wait here is
 * bounded, because servers do sometimes hang and the editor still has to exit.
 */
static void server_stop(LspServer *s) {
    if (!s->failed && s->initialized) {
        JsonW w;
        jw_init(&w);
        jw_obj(&w);
        jw_kstr(&w, "jsonrpc", "2.0");
        jw_kint(&w, "id", s->next_id++);
        jw_kstr(&w, "method", "shutdown");
        jw_key(&w, "params"); jw_null(&w);
        jw_obj_end(&w);
        send_raw(s, w.b, w.len);
        jw_free(&w);

        long deadline = MonotonicMs() + 300;
        while (MonotonicMs() < deadline) {
            struct pollfd p = {s->out_fd, POLLIN, 0};
            if (Poll(&p, 1, 50) > 0 && (p.revents & POLLIN)) break;
        }

        lsp_notify(s, "exit", "null");
    }

    if (s->in_fd >= 0) {                                // EOF on its stdin tells it to go
        Close(s->in_fd);
        s->in_fd = -1;
    }

    if (s->pid > 0) {
        int status;
        long deadline = MonotonicMs() + 500;
        int done = 0;

        while (MonotonicMs() < deadline) {
            if (Waitpid(s->pid, &status, WNOHANG) == s->pid) { done = 1; break; }
            struct pollfd p = {s->out_fd, POLLIN, 0};
            Poll(&p, 1, 20);                            // a sleep that also lets output drain
        }

        if (!done) {
            log_line("%s ignored shutdown; SIGTERM", s->spec->argv[0]);
            Kill(s->pid, SIGTERM);

            deadline = MonotonicMs() + 300;
            while (MonotonicMs() < deadline) {
                if (Waitpid(s->pid, &status, WNOHANG) == s->pid) { done = 1; break; }
                struct pollfd p = {s->out_fd, POLLIN, 0};
                Poll(&p, 1, 20);
            }
            if (!done) {
                log_line("%s ignored SIGTERM; SIGKILL", s->spec->argv[0]);
                Kill(s->pid, SIGKILL);
                Waitpid(s->pid, &status, 0);
            }
        }
        s->pid = -1;
    }
}

void lsp_stop_all(void) {
    // every document's state points at a server about to be freed
    for (int i = 0; i < E.nbufs; i++) lsp_free_document(E.bufs[i]);

    for (int i = 0; i < nservers; i++) {
        server_stop(servers[i]);
        server_free(servers[i]);
        servers[i] = NULL;
    }
    nservers = 0;

    log_line("all servers stopped");
    log_close();
}

void lsp_init(void) {
    /* Writing to a server that has already died raises SIGPIPE, whose default
     * action terminates the editor with no message whatsoever. Ignored, so that
     * the write returns EPIPE and send_raw can report it instead. */
    IgnoreSignal(SIGPIPE);

    log_open("lsp.log");
    log_line("bIDE LSP client starting (pid %d)", (int)getpid());
}

/* --------------------------------------------------------------- queries --- */

LspServer *lsp_server_for(const Buffer *b) {
    if (b == NULL || b->lsp == NULL) return NULL;
    LspServer *s = b->lsp->server;
    return (s && !s->failed && s->initialized) ? s : NULL;
}

int lsp_supports(const LspServer *s, LspFeature f) {
    if (s == NULL || s->failed || !s->initialized) return 0;
    switch (f) {
        case LSP_FEAT_DEFINITION: return s->can_definition;
        case LSP_FEAT_HOVER:      return s->can_hover;
        case LSP_FEAT_COMPLETION: return s->can_completion;
    }
    return 0;
}

const char *lsp_trigger_chars(const LspServer *s) {
    return (s && s->trigger_chars) ? s->trigger_chars : "";
}

int lsp_available(void) {
    return lsp_server_for(editor_buffer()) != NULL;
}

/** Drops every server and reopens the active document against a fresh one. */
void lsp_restart(void) {
    lsp_stop_all();
    log_open("lsp.log");
    log_line("restarted by user");

    for (int i = 0; i < E.nbufs; i++) lsp_open_document(E.bufs[i]);
    editor_set_status_message("Language server restarting");
}
