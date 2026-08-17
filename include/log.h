#ifndef LOG_H
#define LOG_H

#include <stddef.h>

/**
 * A timestamped transcript of every LSP message in both directions.
 *
 * This exists because LSP failures are almost entirely silent. A server that
 * disagrees with you about a document's contents does not report an error — it
 * returns positions that are subtly wrong, and completions for the wrong
 * context. There is no way to see that from inside the editor, and no way to
 * miss it in a transcript.
 *
 * The server's stderr is captured to the same file rather than discarded, since
 * that is where servers explain themselves; clangd in particular says exactly
 * why it could not resolve an include.
 */

/** Opens the log, truncating any previous session. Failure is silent: no log is
 *  a degraded session, not a broken one. */
void log_open(const char *path);

void log_close(void);

/** Path the log was opened at, or NULL. Used by the open-the-log command. */
const char *log_path(void);

/** One protocol message. `dir` is a short tag: "-->" sent, "<--" received. */
void log_message(const char *dir, const char *json, size_t len);

/** Free-form line, for lifecycle events and our own notes. */
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** A chunk of a server's stderr, split into lines and tagged. */
void log_stderr(const char *name, const char *data, size_t len);

#endif
