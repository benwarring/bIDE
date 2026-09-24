#ifndef COMMAND_H
#define COMMAND_H

/**
 * Dispatch is split into two layers:
 *
 *   the command table   what the editor can do   (stable id, display name, fn)
 *   the binding table   how keys reach them      (key -> command id)
 *
 * Key dispatch looks up the key, gets a command id, and invokes its function.
 * The palette lists the same table and invokes the chosen entry. One table, two
 * entry points — which is the only reason a palette is possible at all, since a
 * switch statement cannot be enumerated or invoked by name.
 *
 * Bindings are data now, not control flow. The table in command.c holds the
 * defaults; ~/.config/bide/keys.conf rebinds on top of them.
 */
typedef void (*command_fn)(void);

/** Optional predicate: whether the command currently applies. */
typedef int (*command_avail)(void);

typedef struct {
    const char *id;
    const char *name;
    command_fn fn;
    command_avail avail;        // NULL means always available
} Command;

/**
 * Runs the command bound to `key`. Returns 1 if one ran, 0 if the key is
 * unbound and should be treated as text.
 */
int command_dispatch(int key);

int command_quit_requested(void);

/** Any key that is not the quit key resets the quit confirmation countdown. */
void command_reset_quit(void);

/**
 * Builds the keymap from the defaults plus keys.conf. Must run before the first
 * dispatch; running it again reloads. A bad line is skipped and reported on the
 * status line. Returns how many lines were bad.
 */
int command_load_keymap(void);

/** The startup hint, naming whatever keys save, find, palette, keys and quit are on now. */
void command_show_help(void);

void command_free(void);

#endif
