#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <poll.h>

#include "buffer.h"
#include "command.h"
#include "editor.h"
#include "input.h"
#include "lsp.h"
#include "pane.h"
#include "rain.h"
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
 * split: the call sites did not change, only what they resolve to. Panes are
 * the same trick again: focusing a pane, or drawing one, is nothing more than
 * changing what editor_view() returns.
 */
View *editor_view(void) {
    return E.view;
}

Buffer *editor_buffer(void) {
    View *v = editor_view();
    return v ? v->buf : NULL;
}

void editor_init(void) {
    E.bufs = NULL;
    E.nbufs = 0;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    update_window_size();
    install_winch_handler();
    pane_init();
}

static void cleanup(void) {
    lsp_stop_all();                                     // handshake, then reap, before anything is freed
    lsp_features_free();
    buffer_free();                                      // every buffer's rows and undo stacks
    pane_free();                                        // the split tree and every pane's view
    render_free();
    command_free();                                     // the list widget's scratch and the file cache
}

/**
 * The loop no longer assumes it is in control.
 *
 * Until now it blocked on a keystroke, because nothing could happen unless the
 * user made it happen. A language server replies whenever it likes — sometimes
 * in two milliseconds, sometimes four seconds into indexing — so the loop now
 * waits on stdin *and* every server pipe at once and handles whichever speaks
 * first.
 *
 * Two consequences worth naming. The timeout is computed rather than fixed, so
 * the process sleeps until there is genuinely something to do instead of
 * spinning. And rendering is now conditional on something having changed:
 * redrawing on every wake would flicker the cursor several times a second while
 * a server chatters.
 */
static void run(void) {
    while (1) {
        if (resize_pending) {
            resize_pending = 0;
            update_window_size();
            editor_request_redraw();
        }

        if (editor_take_redraw()) editor_refresh_screen();

        struct pollfd fds[1 + LSP_MAX_FDS];
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        int nfds = 1 + lsp_pollfds(&fds[1], LSP_MAX_FDS);

        /* Sleep exactly as long as the nearest thing that wants waking: a
         * debounced change to flush, or the status message expiring. -1 means
         * nothing is pending and only input can wake us. */
        int timeout = lsp_next_timeout_ms();
        int redraw_at = render_next_timeout_ms();
        if (timeout == -1 || (redraw_at != -1 && redraw_at < timeout)) {
            if (redraw_at != -1) timeout = redraw_at;
        }

        if (Poll(fds, (nfds_t)nfds, timeout) == -1) die("poll");

        lsp_dispatch(&fds[1], nfds - 1);
        lsp_tick();

        // the status message expiring changes the screen with no input at all
        if (render_next_timeout_ms() == -1 && E.statusmsg[0] != '\0') editor_request_redraw();

        if (fds[0].revents & POLLIN) {
            int c = editor_read_key();
            if (c != -1) {
                editor_request_redraw();
                if (!editor_process_keypress(c)) break;
            }
        }
    }

    // nothing to clear on the way out: leaving the alternate screen puts back the
    // main screen as it was left (with the rain, deliberately blank)
}

int main(int argc, char *argv[]) {
    int rain = 1;
    const char *file = NULL, *art = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-rain") == 0) rain = 0;
        else if (strcmp(argv[i], "--art") == 0 && i + 1 < argc) art = argv[++i];
        else if (file == NULL) file = argv[i];
    }

    enable_raw_mode();
    editor_init();
    lsp_init();

    buffer_open(file);
    command_load_keymap();

    // either way, the alternate screen is up with the first frame on it after this
    if (rain) rain_play(art);
    else enter_alt_screen();
    enable_key_reporting();                             // not before: the alternate screen keeps its own

    run();
    cleanup();
    return 0;
}
