#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "command.h"
#include "buffer.h"
#include "editor.h"
#include "files.h"
#include "input.h"
#include "list.h"
#include "render.h"
#include "search.h"
#include "undo.h"
#include "wrappers.h"

#define QUIT_TIMES 3

static int quit_requested = 0;
static int quit_times = QUIT_TIMES;

/* ------------------------------------------------------------- commands --- */

static void cmd_quit(void) {
    if (undo_any_dirty() && quit_times > 0) {
        editor_set_status_message(
            "WARNING! Unsaved changes. Press Ctrl-X %d more time%s to quit.",
            quit_times, quit_times == 1 ? "" : "s");
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

/* -------------------------------------------------------------- tables --- */

static const Command COMMANDS[] = {
    {"cursor.left",       "Move left",                  editor_move_left,       NULL},
    {"cursor.right",      "Move right",                 editor_move_right,      NULL},
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

    {"editor.palette",    "Command palette...",         cmd_palette,            NULL},
    {"editor.quit",       "Quit",                       cmd_quit,               NULL},
};

#define NCOMMANDS (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0]))

typedef struct {
    int key;
    const char *id;
} Binding;

/* Rebinding is editing this table. The Ctrl choices avoid the ones VS Code's
 * integrated terminal tends to intercept. */
static const Binding BINDINGS[] = {
    {ARROW_LEFT,        "cursor.left"},
    {ARROW_RIGHT,       "cursor.right"},
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

    {CTRL_KEY('k'),     "editor.palette"},
    {CTRL_KEY('x'),     "editor.quit"},
};

#define NBINDINGS (int)(sizeof(BINDINGS) / sizeof(BINDINGS[0]))

/* ------------------------------------------------------------- lookups --- */

static const Command *find_command(const char *id) {
    for (int i = 0; i < NCOMMANDS; i++) {
        if (strcmp(COMMANDS[i].id, id) == 0) return &COMMANDS[i];
    }
    return NULL;
}

static void key_label(int key, char *out, size_t n) {
    switch (key) {
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
    else snprintf(out, n, " ");
}

/** First key bound to a command, for display in the palette. */
static void binding_label(const char *id, char *out, size_t n) {
    for (int i = 0; i < NBINDINGS; i++) {
        if (strcmp(BINDINGS[i].id, id) == 0) {
            key_label(BINDINGS[i].key, out, n);
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
    for (int i = 0; i < NBINDINGS; i++) {
        if (BINDINGS[i].key != key) continue;

        const Command *c = find_command(BINDINGS[i].id);
        if (c == NULL) return 0;
        if (c->avail && !c->avail()) return 1;          // bound but not applicable: swallow it

        // a quit attempt from ten minutes ago must not combine with one now
        if (strcmp(c->id, "editor.quit") != 0) command_reset_quit();

        c->fn();
        return 1;
    }
    return 0;
}

void command_free(void) {
    list_free();
    files_free();
}
