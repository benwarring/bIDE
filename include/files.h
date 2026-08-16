#ifndef FILES_H
#define FILES_H

/**
 * Every file under the working directory, walked once and cached. Returns a
 * borrowed array of `*count` paths; the caller must not free it.
 *
 * The walk is done lazily on first call and never repeated, because rescanning
 * a large repository on every keystroke is the obvious wrong answer and easy to
 * write by accident. files_refresh() drops the cache for an explicit rescan.
 */
char **files_list(int *count);

void files_refresh(void);

void files_free(void);

#endif
