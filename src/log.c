#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#include "log.h"

#define LOG_BODY_MAX 4000       // a completion response can be megabytes; the head is what debugs it

static FILE *fp = NULL;
static char *path_copy = NULL;
static struct timeval start;

/** Milliseconds since the log opened. Absolute times say when; deltas say what
 *  was slow, which is the question actually being asked of an async transcript. */
static long elapsed_ms(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
}

void log_open(const char *path) {
    log_close();

    // not Fopen: the wrapper dies on failure, and losing the log must not lose
    // the editor with it
    fp = fopen(path, "w");
    if (fp == NULL) return;

    setvbuf(fp, NULL, _IOLBF, 0);                        // line buffered: a crash keeps what came before it
    path_copy = strdup(path);
    gettimeofday(&start, NULL);

    time_t t = time(NULL);
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(fp, "=== bIDE LSP log — session started %s ===\n", stamp);
}

void log_close(void) {
    if (fp) fclose(fp);
    fp = NULL;
    free(path_copy);
    path_copy = NULL;
}

const char *log_path(void) {
    return path_copy;
}

void log_message(const char *dir, const char *json, size_t len) {
    if (fp == NULL) return;

    fprintf(fp, "\n[%8ld ms] %s %zu bytes\n", elapsed_ms(), dir, len);

    size_t show = len < LOG_BODY_MAX ? len : LOG_BODY_MAX;
    fwrite(json, 1, show, fp);
    if (show < len) fprintf(fp, "\n    ... %zu bytes truncated", len - show);
    fputc('\n', fp);
}

void log_line(const char *fmt, ...) {
    if (fp == NULL) return;

    fprintf(fp, "[%8ld ms] ", elapsed_ms());

    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
}

/**
 * Server stderr arrives in arbitrary chunks, so it is tagged per line rather
 * than per read — otherwise a message split across two reads is unreadable.
 */
void log_stderr(const char *name, const char *data, size_t len) {
    if (fp == NULL) return;

    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && data[j] != '\n') j++;

        if (j > i) fprintf(fp, "[%8ld ms] %s(stderr): %.*s\n", elapsed_ms(), name, (int)(j - i), &data[i]);
        i = j + 1;
    }
}
