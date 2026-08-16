#ifndef SEARCH_H
#define SEARCH_H

/**
 * Incremental search. Opens the prompt with a callback that jumps to a match on
 * every keystroke, so search-as-you-type falls out for free. Arrows step
 * between matches, Enter accepts, Escape restores the exact position the search
 * started from.
 */
void editor_find(void);

#endif
