#ifndef POPUP_H
#define POPUP_H

/**
 * A list anchored at the cursor that does not take the keyboard.
 *
 * The difference from `list_select` is the entire point of it. That one runs its
 * own loop and owns every key, which is right for the palette and wrong for
 * completion: there the keystrokes have to keep reaching the buffer while the
 * list narrows underneath them. A modal list would put the user's typing into a
 * filter box instead of their file, and make Enter mean "insert this item"
 * rather than "newline" — surprising in the middle of a line of code.
 *
 * So this widget has no loop. The key dispatch offers it each key, it takes the
 * handful it wants, and everything else carries on into the editor as if it were
 * not there. Filtering is client-side over the items it was given: the server is
 * asked once, and typing narrows what came back rather than asking again.
 */

enum {
    POPUP_IGNORED = 0,          // not one of ours — handle it normally
    POPUP_CONSUMED,             // navigation, swallowed
    POPUP_ACCEPTED              // the user picked the highlighted item
};

/**
 * `items` is borrowed, not copied — the caller keeps ownership and must not free
 * it before closing. The anchor is the buffer position the completed word starts
 * at, which is where the box hangs from and what the caller measures its prefix
 * against.
 */
void popup_open(char **items, int nitems, int anchor_cy, int anchor_cx);

int popup_active(void);

/** Narrows to the items matching `prefix`. Closes itself if none do. */
void popup_filter(const char *prefix);

int popup_key(int c);

/** Index into the array handed to `popup_open`, or -1. */
int popup_selection(void);

int popup_anchor_cy(void);
int popup_anchor_cx(void);

void popup_close(void);

void popup_free(void);

#endif
