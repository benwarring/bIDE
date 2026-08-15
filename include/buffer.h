#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>

void buffer_open(const char *filename);

void buffer_free(void);

int buffer_num_lines(void);

int buffer_line_length(int idx);

/**
 * Copies line `idx` into the caller's buffer, NUL-terminated and truncated to
 * fit. Returns the number of bytes written, or -1 if `idx` is out of range.
 *
 * Deliberately materializes rather than handing back a pointer into internal
 * storage: a piece table could not honor a borrowed pointer, so establishing
 * the copying signature now keeps the later swap a swap and not a rewrite.
 */
int buffer_get_line(int idx, char *dst, size_t dstsize);

#endif
