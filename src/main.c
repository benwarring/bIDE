#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buffer.h"
#include "editor.h"
#include "input.h"
#include "render.h"
#include "terminal.h"
#include "wrappers.h"

EditorConfig E;

void editor_init(void) {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.filename = NULL;
    E.crlf = 0;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    update_window_size();
    install_winch_handler();
}

static void cleanup(void) {
    buffer_free();
    render_free();
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
    editor_set_status_message("HELP: Ctrl-X = quit");

    run();
    cleanup();
    return 0;
}
