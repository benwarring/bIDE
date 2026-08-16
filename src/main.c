#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buffer.h"
#include "command.h"
#include "editor.h"
#include "input.h"
#include "render.h"
#include "terminal.h"
#include "undo.h"
#include "wrappers.h"

EditorConfig E;

/**
 * The active view, and the buffer it looks at.
 *
 * Every module reaches per-file and per-cursor state through these two rather
 * than through globals, which is what let the single-buffer code survive the
 * split: the call sites did not change, only what they resolve to. It is also
 * the seam split panes will need — a second view onto the same buffer only has
 * to change what editor_view() returns.
 */
View *editor_view(void) {
    if (E.active < 0 || E.active >= E.nviews) return NULL;
    return E.views[E.active];
}

Buffer *editor_buffer(void) {
    View *v = editor_view();
    return v ? v->buf : NULL;
}

void editor_init(void) {
    E.bufs = NULL;
    E.nbufs = 0;
    E.views = NULL;
    E.nviews = 0;
    E.active = 0;
    E.gutter = 0;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    update_window_size();
    install_winch_handler();
}

static void cleanup(void) {
    buffer_free();                                      // every buffer's rows, undo stacks and views
    render_free();
    command_free();                                     // the list widget's scratch and the file cache
}

static void run(void) {
    while (1) {
        if (resize_pending) {
            resize_pending = 0;
            update_window_size();
        }
        editor_refresh_screen();

        int c;
        while ((c = editor_read_key()) == -1 && !resize_pending)
            ;
        if (c == -1) continue;                          // woken by a resize, redraw

        if (!editor_process_keypress(c)) break;
    }

    Write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
}

int main(int argc, char *argv[]) {
    enable_raw_mode();
    editor_init();

    buffer_open(argc >= 2 ? argv[1] : NULL);
    editor_set_status_message(
        "HELP: Ctrl-S save | Ctrl-F find | Ctrl-Z undo | Ctrl-Y redo | Ctrl-X quit");

    run();
    cleanup();
    return 0;
}
