#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "terminal.h"
#include "wrappers.h"
#include "editor.h"

static struct termios orig_termios;

volatile sig_atomic_t resize_pending = 0;

/* 1000 = report button presses, 1006 = SGR coordinates, which unlike the
 * original encoding are plain decimal and so work past column 223. */
#define MOUSE_ON  "\x1b[?1000h\x1b[?1006h"
#define MOUSE_OFF "\x1b[?1006l\x1b[?1000l"

/* 1049 is the alternate screen buffer: a second, scrollback-less screen that
 * the terminal swaps in whole. Entering saves the real screen and hands us a
 * blank one; leaving restores the real screen exactly as it was. Every frame we
 * draw lands there and nothing reaches the scrollback, which is what stops a
 * session leaving a wall of editor frames behind in the terminal history.
 *
 * This is how vim and less behave, and it is a single pair of sequences rather
 * than any clearing of our own — clearing with \x1b[2J would still scroll the
 * old contents away permanently. */
#define SCREEN_ON  "\x1b[?1049h"
#define SCREEN_OFF "\x1b[?1049l"

/* Set once the original settings have actually been captured, so a die() during
 * startup does not restore a struct that was never filled in, and cleared once
 * they are back so the atexit handler is a no-op after die() has already run. */
static int termios_saved = 0;

// runs from atexit, so it uses tcsetattr and write directly: the wrappers die()
// on failure, and calling exit() from an exit handler is undefined behavior
static void restore_terminal(void) {
    if (!termios_saved) return;
    termios_saved = 0;

    ssize_t n = write(STDOUT_FILENO, MOUSE_OFF SCREEN_OFF, sizeof(MOUSE_OFF SCREEN_OFF) - 1);
    (void)n;                                            // failing here changes nothing we can act on
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);  // restore original terminal settings
}

int die(const char *s) {
    static int dying = 0;
    if (!dying) {
        dying = 1;                                      // Write() calls die() on failure; don't recurse

        /* Leave the alternate screen *before* reporting, or the message is
         * printed onto a screen that is about to be discarded and the user sees
         * the editor vanish with no explanation. Restoring the termios first
         * also puts OPOST back, without which the newline has no carriage
         * return and the text walks off to the right. */
        restore_terminal();
    }
    perror(s);
    exit(1);
}

void enable_raw_mode(void) {
    Tcgetattr(STDIN_FILENO, &orig_termios);             // get current terminal settings
    termios_saved = 1;
    atexit(restore_terminal);                           // register restore_terminal to be called at exit

    struct termios raw = orig_termios;

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    Tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    Write(STDOUT_FILENO, SCREEN_ON MOUSE_ON, sizeof(SCREEN_ON MOUSE_ON) - 1);
}

/**
 * Fallback path for get_window_size: ask the terminal where the cursor is
 * with a Device Status Report and parse the \x1b[<rows>;<cols>R reply
 */
static int get_cursor_position(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    Write(STDOUT_FILENO, "\x1b[6n", 4);

    while (i < sizeof(buf) - 1) {
        if (Read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int get_window_size(int *rows, int *cols) {
    struct winsize ws;

    if (Ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // park the cursor at the far bottom-right; both commands stop at the edge
        Write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12);
        return get_cursor_position(rows, cols);
    }

    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
}

static void handle_winch(int sig) {
    (void)sig;
    resize_pending = 1;
}

void install_winch_handler(void) {
    struct sigaction sa;
    sa.sa_handler = handle_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    Sigaction(SIGWINCH, &sa, NULL);
}

void update_window_size(void) {
    if (get_window_size(&E.screenrows, &E.screencols) == -1) die("get_window_size");
    E.screenrows -= 2;                                  // status bar + message line
    if (E.screenrows < 1) E.screenrows = 1;
}
