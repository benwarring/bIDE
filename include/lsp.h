#ifndef LSP_H
#define LSP_H

#include <poll.h>
#include <stddef.h>

#include "editor.h"
#include "json.h"

/**
 * The language server client.
 *
 * One server process per language, shared by every buffer of that type. The
 * editor must remain a working text editor with no server at all: every entry
 * point here is a no-op when nothing is running, and a server that dies takes
 * its features down and nothing else.
 *
 * The asymmetry worth keeping in mind while reading this: we send requests and
 * the answers come back an unknown time later, out of order, after the user has
 * kept typing. Nothing here may assume the world still looks the way it did
 * when a request went out — which is what the stored request context is for.
 */

#define LSP_MAX_SERVERS 4
#define LSP_MAX_FDS (LSP_MAX_SERVERS * 2)               // stdout and stderr per server

/* Diagnostic severities, as the protocol numbers them. */
enum {
    LSP_ERROR = 1,
    LSP_WARNING = 2,
    LSP_INFO = 3,
    LSP_HINT = 4
};

void lsp_init(void);

/** Shuts every server down through the proper handshake, then frees. */
void lsp_stop_all(void);

/* ------------------------------------------------------- document sync --- */

/**
 * Starts a server for this buffer's language if one is not already running, and
 * tells it the document exists. Safe to call for a buffer with no known server.
 */
void lsp_open_document(Buffer *b);

void lsp_close_document(Buffer *b);

/** Marks the document changed. The actual didChange is debounced. */
void lsp_touch_document(Buffer *b);

/** Frees the per-buffer server state. Called when a buffer is closed. */
void lsp_free_document(Buffer *b);

/* -------------------------------------------------------- the event loop --- */

/** Adds every server descriptor to `fds`. Returns how many were added. */
int lsp_pollfds(struct pollfd *fds, int max);

/** Handles whatever those descriptors reported ready. */
void lsp_dispatch(struct pollfd *fds, int n);

/** Milliseconds until the next timed action (a debounce flush), or -1. */
int lsp_next_timeout_ms(void);

/** Time-driven work: flush debounced changes, reap exited servers. */
void lsp_tick(void);

/* -------------------------------------------------------------- queries --- */

/** Highest severity on `line` of buffer `b`, or 0 if it is clean. */
int lsp_line_severity(const Buffer *b, int line);

/** Message to show for `line`, or NULL. */
const char *lsp_line_message(const Buffer *b, int line);

/** Short status string for the status bar, or NULL. */
const char *lsp_status(const Buffer *b);

/** Line of the nearest diagnostic in direction `dir`, wrapping. -1 if none. */
int lsp_diag_next_line(const Buffer *b, int from, int dir);

/* ------------------------------------------------------------- features --- */

void lsp_goto_definition(void);
void lsp_jump_back(void);
void lsp_hover(void);
void lsp_complete(void);
void lsp_next_diagnostic(void);
void lsp_prev_diagnostic(void);
void lsp_open_log(void);
void lsp_restart(void);

/* ------------------------------------------------- the completion popup --- */

/**
 * Completion is the one feature that does not wait for its answer. It is also
 * the one the user is still typing during, so its popup is offered each key and
 * takes only what it needs — see popup.h for why it cannot be a modal list.
 */

int lsp_completion_active(void);

/** Offers a key to the popup. Returns 1 if the popup dealt with it. */
int lsp_completion_key(int c);

/** Re-narrows or dismisses the popup after a key has been handled normally. */
void lsp_completion_sync(void);

/** Requests completion if `c` is a character the server asked to be woken on. */
void lsp_completion_maybe_trigger(int c);

/** Drops any completion state tied to `b`, which is about to be freed. */
void lsp_completion_forget(const Buffer *b);

/** Frees what the feature layer holds: popup scratch, jump list, hover text. */
void lsp_features_free(void);

/* Availability predicates for the command table. */
int lsp_available(void);
int lsp_have_jumps(void);

/* --------------------------------------------- shared by the feature code --- */

typedef struct LspServer LspServer;

/**
 * What a request was about, kept so its answer can be checked for relevance
 * when it arrives. The user may have moved, edited, switched buffers or closed
 * the file in the meantime, and acting on a stale answer is the characteristic
 * async bug of this milestone.
 */
typedef struct {
    char *uri;
    int version;                // document version when the request went out
    int cy, cx;                 // cursor position at that moment
    char *extra;                // feature-specific: the completion prefix
} LspRequest;

typedef void (*lsp_reply_fn)(LspServer *s, JsonValue *result, JsonValue *error,
                             const LspRequest *req);

/** The server handling the active buffer, or NULL. */
LspServer *lsp_server_for(const Buffer *b);

/** Flushes any pending change, then sends a request. Returns 0 if it could not
 *  be sent. `params` is serialized JSON for the params object. */
int lsp_request(LspServer *s, const char *method, const char *params,
                lsp_reply_fn on_reply, const LspRequest *ctx);

void lsp_notify(LspServer *s, const char *method, const char *params);

/**
 * Features the server advertised at startup. Calling an unsupported method gets
 * an error from some servers and silence from others, so every feature checks
 * before asking.
 */
typedef enum {
    LSP_FEAT_DEFINITION,
    LSP_FEAT_HOVER,
    LSP_FEAT_COMPLETION
} LspFeature;

int lsp_supports(const LspServer *s, LspFeature f);

/** Characters the server wants completion requested on ("." and ">" for C), or "". */
const char *lsp_trigger_chars(const LspServer *s);

/** Serialized `{"textDocument":{...},"position":{...}}` for the cursor. Caller frees. */
char *lsp_position_params(LspServer *s, const Buffer *b, int cy, int cx);

/** Fills in a request context describing where the cursor is now. */
void lsp_fill_request(LspRequest *req, const Buffer *b, int cy, int cx);

/** Whether a reply still applies: same document, same version, cursor unmoved. */
int lsp_request_still_valid(const LspRequest *req);

/** file:// URI for a path, percent-encoded. Caller frees. */
char *lsp_path_to_uri(const char *path);

/** Absolute path from a file:// URI, percent-decoded. Caller frees. */
char *lsp_uri_to_path(const char *uri);

/**
 * Byte index within a line, converted to the character offset the server counts
 * in, and back. These are the identity for ASCII, which is exactly why getting
 * them wrong stays invisible until someone puts an accent in a comment.
 */
int lsp_cx_to_character(const LspServer *s, const Buffer *b, int cy, int cx);
int lsp_character_to_cx(const LspServer *s, const Buffer *b, int cy, int character);

/** The document URI for a buffer, or NULL if it has no path. Caller frees. */
char *lsp_buffer_uri(const Buffer *b);

/** Pushes the current position onto the jump list, before jumping away. */
void lsp_push_jump(void);

/** Opens `path` and puts the cursor at (line, character), server-counted. */
void lsp_jump_to(const char *path, int line, int character);

#endif
