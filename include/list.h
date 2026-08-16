#ifndef LIST_H
#define LIST_H

/**
 * Modal selectable list: a query being typed, candidates filtered and ranked by
 * it, a highlighted selection moved with the arrows, Enter to accept, Escape to
 * cancel.
 *
 * Deliberately generic — it takes an array of display strings and returns the
 * chosen index into that array, or -1 for cancel. It knows nothing about
 * commands or filenames, which is what lets the palette, the fuzzy file open and
 * the buffer switcher all use it unmodified.
 */
int list_select(const char *title, char **items, int nitems);

void list_free(void);

#endif
