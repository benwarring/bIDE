#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>

#include "command.h"
#include "buffer.h"
#include "editor.h"
#include "files.h"
#include "input.h"
#include "list.h"
#include "lsp.h"
#include "render.h"
#include "search.h"
#include "undo.h"
#include "wrappers.h"

#define QUIT_TIMES 3

static int quit_requested = 0;
static int quit_times = QUIT_TIMES;

static void binding_label(const char *id, char *out, size_t n);

/* ------------------------------------------------------------- commands --- */

static void cmd_quit(void) {
    if (undo_any_dirty() && quit_times > 0) {
        char key[24];
        binding_label("editor.quit", key, sizeof(key));
        editor_set_status_message(
            "WARNING! Unsaved changes. Press %s %d more time%s to quit.",
            key, quit_times, quit_times == 1 ? "" : "s");
        quit_times--;
        return;
    }
    quit_requested = 1;
}

static void cmd_buffer_next(void) { buffer_cycle(1); }
static void cmd_buffer_prev(void) { buffer_cycle(-1); }

static int avail_many_buffers(void) {
    return buffer_count() > 1;
}

static void cmd_buffer_close(void) {
    int idx = buffer_active();

    if (buffer_is_dirty(idx)) {
        editor_set_status_message("Buffer has unsaved changes. Save first, or undo back.");
        return;
    }
    buffer_close(idx);
    editor_set_status_message("Closed. %d buffer%s open.",
                             buffer_count(), buffer_count() == 1 ? "" : "s");
}

static void cmd_buffer_list(void) {
    int n = buffer_count();

    char **items = Malloc(sizeof(char *) * (size_t)n);
    for (int i = 0; i < n; i++) {
        char line[256];
        snprintf(line, sizeof(line), "%s%s", buffer_name(i),
                 buffer_is_dirty(i) ? "  (modified)" : "");
        items[i] = strdup(line);
    }

    int pick = list_select("Buffers", items, n);
    if (pick >= 0) buffer_switch(pick);

    for (int i = 0; i < n; i++) free(items[i]);
    free(items);
}

static void cmd_file_open(void) {
    int n = 0;
    char **paths = files_list(&n);

    if (n == 0) {
        editor_set_status_message("No files found under the working directory");
        return;
    }

    int pick = list_select("Open", paths, n);
    if (pick < 0) return;

    buffer_open(paths[pick]);
    editor_set_status_message("Opened %s", buffer_name(buffer_active()));
}

static void cmd_file_refresh(void) {
    files_refresh();
    editor_set_status_message("File list will be rescanned on next open");
}

static void cmd_palette(void);
static void cmd_keys(void);

/* -------------------------------------------------------------- tables --- */

static const Command COMMANDS[] = {
    {"cursor.left",       "Move left",                  editor_move_left,       NULL},
    {"cursor.right",      "Move right",                 editor_move_right,      NULL},
    {"cursor.wordleft",   "Move word left",             editor_word_left,       NULL},
    {"cursor.wordright",  "Move word right",            editor_word_right,      NULL},
    {"cursor.up",         "Move up",                    editor_move_up,         NULL},
    {"cursor.down",       "Move down",                  editor_move_down,       NULL},
    {"cursor.pageup",     "Page up",                    editor_page_up,         NULL},
    {"cursor.pagedown",   "Page down",                  editor_page_down,       NULL},
    {"cursor.linestart",  "Go to start of line",        editor_line_start,      NULL},
    {"cursor.lineend",    "Go to end of line",          editor_line_end,        NULL},
    {"cursor.click",      "Move cursor to click",       editor_mouse_click,     NULL},
    {"cursor.wheelup",    "Scroll up",                  editor_wheel_up,        NULL},
    {"cursor.wheeldown",  "Scroll down",                editor_wheel_down,      NULL},

    {"edit.newline",      "Insert line break",          editor_insert_newline,  NULL},
    {"edit.backspace",    "Delete backward",            editor_backspace,       NULL},
    {"edit.delete",       "Delete forward",             editor_delete_forward,  NULL},
    {"edit.undo",         "Undo",                       editor_undo,            NULL},
    {"edit.redo",         "Redo",                       editor_redo,            NULL},

    {"file.save",         "Save file",                  editor_save,            NULL},
    {"file.open",         "Open file...",               cmd_file_open,          NULL},
    {"file.refresh",      "Rescan files",               cmd_file_refresh,       NULL},
    {"file.find",         "Find in file...",            editor_find,            NULL},

    {"buffer.next",       "Next buffer",                cmd_buffer_next,        avail_many_buffers},
    {"buffer.prev",       "Previous buffer",            cmd_buffer_prev,        avail_many_buffers},
    {"buffer.close",      "Close buffer",               cmd_buffer_close,       NULL},
    {"buffer.list",       "Switch buffer...",           cmd_buffer_list,        avail_many_buffers},

    {"multicursor.above", "Add cursor above",           editor_cursor_add_above, NULL},
    {"multicursor.below", "Add cursor below",           editor_cursor_add_below, NULL},
    {"multicursor.next",  "Add cursor at next match",   editor_cursor_add_next,  NULL},
    {"multicursor.clear", "Collapse to one cursor",     editor_cursor_collapse,
                                                        editor_has_extra_cursors},

    {"lsp.definition",    "Go to definition",           lsp_goto_definition,    lsp_available},
    {"lsp.back",          "Jump back",                  lsp_jump_back,          lsp_have_jumps},
    {"lsp.hover",         "Show hover documentation",   lsp_hover,              lsp_available},
    {"lsp.complete",      "Complete at cursor",         lsp_complete,           lsp_available},
    {"lsp.diag.next",     "Next diagnostic",            lsp_next_diagnostic,    lsp_available},
    {"lsp.diag.prev",     "Previous diagnostic",        lsp_prev_diagnostic,    lsp_available},
    {"lsp.log",           "Open LSP message log",       lsp_open_log,           NULL},
    {"lsp.restart",       "Restart language server",    lsp_restart,            NULL},

    {"editor.palette",    "Command palette...",         cmd_palette,            NULL},
    {"editor.keys",       "Configure keybindings...",   cmd_keys,               NULL},
    {"editor.quit",       "Quit",                       cmd_quit,               NULL},
};

#define NCOMMANDS (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0]))

typedef struct {
    int key;
    const char *id;
} Binding;

/* The defaults. keys.conf rebinds on top of a copy of this table. The Ctrl
 * choices avoid the ones VS Code's integrated terminal tends to intercept. */
static const Binding BINDINGS[] = {
    {ARROW_LEFT,        "cursor.left"},
    {ARROW_RIGHT,       "cursor.right"},
    {CTRL_ARROW_LEFT,   "cursor.wordleft"},
    {CTRL_ARROW_RIGHT,  "cursor.wordright"},
    {ARROW_UP,          "cursor.up"},
    {ARROW_DOWN,        "cursor.down"},
    {PAGE_UP,           "cursor.pageup"},
    {PAGE_DOWN,         "cursor.pagedown"},
    {HOME_KEY,          "cursor.linestart"},
    {END_KEY,           "cursor.lineend"},
    {MOUSE_CLICK,       "cursor.click"},
    {WHEEL_UP,          "cursor.wheelup"},
    {WHEEL_DOWN,        "cursor.wheeldown"},

    {'\r',              "edit.newline"},
    {BACKSPACE,         "edit.backspace"},
    {CTRL_KEY('h'),     "edit.backspace"},
    {DEL_KEY,           "edit.delete"},
    {CTRL_KEY('z'),     "edit.undo"},
    {CTRL_KEY('y'),     "edit.redo"},

    {CTRL_KEY('s'),     "file.save"},
    {CTRL_KEY('o'),     "file.open"},
    {CTRL_KEY('r'),     "file.refresh"},
    {CTRL_KEY('f'),     "file.find"},

    {CTRL_KEY('n'),     "buffer.next"},
    {CTRL_KEY('p'),     "buffer.prev"},
    {CTRL_KEY('w'),     "buffer.close"},
    {CTRL_KEY('b'),     "buffer.list"},

    {CTRL_ARROW_UP,     "multicursor.above"},
    {CTRL_ARROW_DOWN,   "multicursor.below"},
    {CTRL_KEY('d'),     "multicursor.next"},
    {'\x1b',            "multicursor.clear"},

    {CTRL_KEY('g'),     "lsp.definition"},
    {CTRL_KEY('t'),     "lsp.back"},
    {CTRL_KEY('l'),     "lsp.hover"},
    {CTRL_KEY(' '),     "lsp.complete"},                // Ctrl-Space arrives as NUL
    {CTRL_KEY('e'),     "lsp.diag.next"},
    {CTRL_KEY('a'),     "lsp.diag.prev"},

    {CTRL_KEY('k'),     "editor.palette"},
    {CTRL_SHIFT_KEY('k'), "editor.keys"},               // only in terminals that can tell it from Ctrl-K
    {CTRL_KEY('x'),     "editor.quit"},
};

#define NBINDINGS (int)(sizeof(BINDINGS) / sizeof(BINDINGS[0]))

/* Keys the config file can neither take away nor hand out: movement and the
 * editing keys every editor agrees on. Tab, Enter and Esc are here because
 * Ctrl-I, Ctrl-M and Ctrl-[ are the same bytes, and Ctrl-H is Backspace. */
static const int LOCKED_KEYS[] = {
    ARROW_LEFT, ARROW_RIGHT, ARROW_UP, ARROW_DOWN,
    CTRL_ARROW_LEFT, CTRL_ARROW_RIGHT,
    HOME_KEY, END_KEY, PAGE_UP, PAGE_DOWN,
    MOUSE_CLICK, WHEEL_UP, WHEEL_DOWN,
    '\r', '\t', '\x1b', BACKSPACE, CTRL_KEY('h'), DEL_KEY,
    CTRL_KEY('z'), CTRL_KEY('y'),                       // undo, redo
    CTRL_KEY('c'), CTRL_KEY('v'),                       // reserved for copy and paste
};

#define NLOCKED (int)(sizeof(LOCKED_KEYS) / sizeof(LOCKED_KEYS[0]))

// the live keymap: BINDINGS plus whatever keys.conf changed
static Binding *keymap = NULL;
static int nkeymap = 0;

/* ------------------------------------------------------------- lookups --- */

static const Command *find_command(const char *id) {
    for (int i = 0; i < NCOMMANDS; i++) {
        if (strcmp(COMMANDS[i].id, id) == 0) return &COMMANDS[i];
    }
    return NULL;
}

static void key_label(int key, char *out, size_t n) {
    switch (key) {
        case 0:               snprintf(out, n, "Ctrl-Space"); return;
        case '\r':            snprintf(out, n, "Enter");     return;
        case '\x1b':          snprintf(out, n, "Esc");       return;
        case BACKSPACE:       snprintf(out, n, "Backspace"); return;
        case DEL_KEY:         snprintf(out, n, "Del");       return;
        case ARROW_LEFT:      snprintf(out, n, "Left");      return;
        case ARROW_RIGHT:     snprintf(out, n, "Right");     return;
        case ARROW_UP:        snprintf(out, n, "Up");        return;
        case ARROW_DOWN:      snprintf(out, n, "Down");      return;
        case CTRL_ARROW_UP:   snprintf(out, n, "Ctrl-Up");   return;
        case CTRL_ARROW_DOWN: snprintf(out, n, "Ctrl-Down"); return;
        case CTRL_ARROW_LEFT: snprintf(out, n, "Ctrl-Left"); return;
        case CTRL_ARROW_RIGHT: snprintf(out, n, "Ctrl-Right"); return;
        case PAGE_UP:         snprintf(out, n, "PgUp");      return;
        case PAGE_DOWN:       snprintf(out, n, "PgDn");      return;
        case HOME_KEY:        snprintf(out, n, "Home");      return;
        case END_KEY:         snprintf(out, n, "End");       return;
        case MOUSE_CLICK:     snprintf(out, n, "Click");     return;
        case WHEEL_UP:        snprintf(out, n, "WheelUp");   return;
        case WHEEL_DOWN:      snprintf(out, n, "WheelDn");   return;
        default: break;
    }

    if (key >= 1 && key <= 26) snprintf(out, n, "Ctrl-%c", 'A' + key - 1);
    else if (key >= CTRL_SHIFT_A && key < CTRL_SHIFT_A + 26)
        snprintf(out, n, "Ctrl-Shift-%c", 'A' + key - CTRL_SHIFT_A);
    else snprintf(out, n, " ");
}

/** First key bound to a command, for display in the palette. */
static void binding_label(const char *id, char *out, size_t n) {
    for (int i = 0; i < nkeymap; i++) {
        if (strcmp(keymap[i].id, id) == 0) {
            key_label(keymap[i].key, out, n);
            return;
        }
    }
    out[0] = '\0';
}

/* ------------------------------------------------------------- palette --- */

/**
 * The palette is small precisely because the command table and the list widget
 * already exist. If this were long, one of those two was built wrong.
 */
static void cmd_palette(void) {
    char **items = Malloc(sizeof(char *) * (size_t)NCOMMANDS);
    int *map = Malloc(sizeof(int) * (size_t)NCOMMANDS);
    int n = 0;

    for (int i = 0; i < NCOMMANDS; i++) {
        if (COMMANDS[i].avail && !COMMANDS[i].avail()) continue;   // hide what doesn't apply

        char key[24];
        binding_label(COMMANDS[i].id, key, sizeof(key));

        char line[256];
        // showing the binding turns the palette into the keymap documentation
        snprintf(line, sizeof(line), "%-28s %s", COMMANDS[i].name, key);
        items[n] = strdup(line);
        map[n] = i;
        n++;
    }

    int pick = list_select("Command", items, n);

    for (int i = 0; i < n; i++) free(items[i]);
    free(items);

    if (pick >= 0) {
        const Command *c = &COMMANDS[map[pick]];
        free(map);
        c->fn();
        return;
    }
    free(map);
}

/* ------------------------------------------------------------ dispatch --- */

void command_reset_quit(void) {
    quit_times = QUIT_TIMES;
}

int command_quit_requested(void) {
    return quit_requested;
}

int command_dispatch(int key) {
    for (int i = 0; i < nkeymap; i++) {
        if (keymap[i].key != key) continue;

        const Command *c = find_command(keymap[i].id);
        if (c == NULL) return 0;
        if (c->avail && !c->avail()) return 1;          // bound but not applicable: swallow it

        // a quit attempt from ten minutes ago must not combine with one now
        if (strcmp(c->id, "editor.quit") != 0) command_reset_quit();

        c->fn();
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------- keymap --- */

/**
 * Parsing a key name is key_label run backwards: try every key there is and
 * keep the one whose label matches. The palette and the config file can then
 * never disagree about what a key is called.
 */
static int parse_key(const char *name) {
    char label[24];
    for (int k = 0; k < CTRL_SHIFT_A + 26; k++) {
        if (k > BACKSPACE && k < ARROW_LEFT) continue;
        key_label(k, label, sizeof(label));
        if (strcasecmp(label, name) == 0) return k;
    }
    return -1;
}

static int is_locked(int key) {
    for (int i = 0; i < NLOCKED; i++) {
        if (LOCKED_KEYS[i] == key) return 1;
    }
    return 0;
}

/** Drops unlocked bindings: every one for command `id`, or if `id` is NULL, the one on `key`. */
static void unbind(int key, const char *id) {
    int out = 0;
    for (int i = 0; i < nkeymap; i++) {
        int drop = !is_locked(keymap[i].key) &&
                   (id ? strcmp(keymap[i].id, id) == 0 : keymap[i].key == key);
        if (!drop) keymap[out++] = keymap[i];
    }
    nkeymap = out;
}

/** Where keys.conf lives. Returns 0 if there is nowhere to put it. */
static int keys_path(char *out, size_t n) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/bide/keys.conf", xdg);
    else if (home) snprintf(out, n, "%s/.config/bide/keys.conf", home);
    else return 0;
    return 1;
}

int command_load_keymap(void) {
    free(keymap);                                       // a reload starts over from the defaults
    keymap = Malloc(sizeof(BINDINGS));
    memcpy(keymap, BINDINGS, sizeof(BINDINGS));
    nkeymap = NBINDINGS;

    char path[1024];
    if (!keys_path(path, sizeof(path))) return 0;

    FILE *fp = fopen(path, "r");                        // no file is the normal case, not an error
    if (fp == NULL) return 0;

    // the first mention of a command replaces its default keys; later lines add more
    char rebound[NCOMMANDS];
    memset(rebound, 0, sizeof(rebound));

    char line[256];
    int lineno = 0, errors = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char id[64], name[64], extra;
        int got = sscanf(line, "%63s %63s %c", id, name, &extra);
        if (got <= 0) continue;                         // blank or comment

        const Command *c = find_command(id);
        int key = (got == 2) ? parse_key(name) : -1;

        // a bad line is skipped, not fatal; the last one is what stays on screen
        if (got != 2 || c == NULL || key == -1 || is_locked(key)) errors++;

        if (got != 2) {
            editor_set_status_message("keys.conf:%d: expected '<command> <key>'", lineno);
        } else if (c == NULL) {
            editor_set_status_message("keys.conf:%d: no command '%s'", lineno, id);
        } else if (key == -1) {
            editor_set_status_message("keys.conf:%d: unknown key '%s'", lineno, name);
        } else if (is_locked(key)) {
            editor_set_status_message("keys.conf:%d: %s can't be rebound", lineno, name);
        } else {
            if (!rebound[c - COMMANDS]) {
                rebound[c - COMMANDS] = 1;
                unbind(0, c->id);
            }
            unbind(key, NULL);                          // the key now belongs to this command alone

            keymap = Realloc(keymap, sizeof(Binding) * (size_t)(nkeymap + 1));
            keymap[nkeymap].key = key;
            keymap[nkeymap].id = c->id;                 // the table's string, which never goes away
            nkeymap++;
        }
    }
    fclose(fp);
    return errors;
}

/** mkdir -p for everything above `path`. Failures surface when the file is opened. */
static void make_parents(const char *path) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *s = dir + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        mkdir(dir, 0755);                               // EEXIST is the usual answer
        *s = '/';
    }
}

/**
 * Rewrites keys.conf with `id` bound to `label` (or back to its defaults if
 * `label` is NULL). Every line for `id`, and every line giving `key` to someone
 * else, is dropped; everything else, comments included, is kept as written. Goes
 * through a temporary file so a failed write cannot eat the old config.
 */
static int write_binding(const char *path, const char *id, int key, const char *label) {
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    make_parents(path);
    FILE *out = fopen(tmp, "w");
    if (out == NULL) return 0;
    FILE *in = fopen(path, "r");                        // absent the first time

    char line[256];
    while (in && fgets(line, sizeof(line), in)) {
        char copy[256], lid[64], lname[64];
        memcpy(copy, line, sizeof(line));
        char *hash = strchr(copy, '#');
        if (hash) *hash = '\0';

        if (sscanf(copy, "%63s %63s", lid, lname) == 2 &&
            (strcmp(lid, id) == 0 || (label && parse_key(lname) == key))) continue;
        fputs(line, out);
    }
    if (label) fprintf(out, "%-16s %s\n", id, label);

    int ok = !ferror(out);
    if (in) fclose(in);
    if (fclose(out) != 0) ok = 0;
    if (ok && rename(tmp, path) == 0) return 1;

    remove(tmp);
    return 0;
}

/**
 * Rebinding from inside the editor edits keys.conf and reloads it, rather than
 * patching the live table, so what you set is exactly what the next start sees.
 */
static void cmd_keys(void) {
    char path[1024];
    if (!keys_path(path, sizeof(path))) {
        editor_set_status_message("No HOME or XDG_CONFIG_HOME: nowhere to save keys.conf");
        return;
    }

    char **items = Malloc(sizeof(char *) * (size_t)NCOMMANDS);
    for (int i = 0; i < NCOMMANDS; i++) {
        char key[24];
        binding_label(COMMANDS[i].id, key, sizeof(key));

        char line[256];
        snprintf(line, sizeof(line), "%-28s %s", COMMANDS[i].name, key);
        items[i] = strdup(line);
    }

    int pick = list_select("Rebind", items, NCOMMANDS);
    for (int i = 0; i < NCOMMANDS; i++) free(items[i]);
    free(items);
    if (pick < 0) return;

    const Command *c = &COMMANDS[pick];
    editor_set_status_message("Press a key for \"%s\"  (Backspace: default, Esc: cancel)", c->name);

    int key;
    do {
        editor_refresh_screen();
        key = editor_read_key();
    } while (key == -1);                                // read timeout, not a keypress

    char label[24];
    key_label(key, label, sizeof(label));
    int reset = (key == BACKSPACE || key == CTRL_KEY('h'));

    if (key == '\x1b') {
        editor_set_status_message("Rebind cancelled");
        return;
    }
    if (!reset && (is_locked(key) || label[0] == ' ')) { // a locked key, or plain text
        editor_set_status_message("%s can't be rebound", label[0] == ' ' ? "That key" : label);
        return;
    }
    if (!write_binding(path, c->id, key, reset ? NULL : label)) {
        editor_set_status_message("Can't write %s: %s", path, strerror(errno));
        return;
    }

    command_load_keymap();
    if (reset) editor_set_status_message("\"%s\" is back to its default", c->name);
    else editor_set_status_message("\"%s\" is now %s", c->name, label);
}

void command_show_help(void) {
    static const char *const HELP[][2] = {
        {"file.save", "save"}, {"file.find", "find"}, {"editor.palette", "palette"},
        {"editor.keys", "keys"}, {"editor.quit", "quit"},
    };

    char msg[128] = "HELP:";
    size_t len = strlen(msg);
    const char *sep = " ";

    for (size_t i = 0; i < sizeof(HELP) / sizeof(HELP[0]) && len < sizeof(msg); i++) {
        char key[24];
        binding_label(HELP[i][0], key, sizeof(key));
        if (key[0] == '\0') continue;                   // unbound: nothing to advertise

        len += (size_t)snprintf(msg + len, sizeof(msg) - len, "%s%s %s", sep, key, HELP[i][1]);
        sep = " | ";
    }
    editor_set_status_message("%s", msg);
}

void command_free(void) {
    free(keymap);
    keymap = NULL;
    nkeymap = 0;
    list_free();
    files_free();
}
