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

/* Ask for modified keys as escape sequences, so Ctrl-Shift-K is not just Ctrl-K.
 * ">1u" pushes the kitty keyboard protocol's disambiguate mode, "<u" pops it;
 * ">4;1m" is xterm's modifyOtherKeys. A terminal that knows neither ignores
 * both and keeps sending plain control bytes. */
#define KEYS_ON  "\x1b[>1u\x1b[>4;1m"
#define KEYS_OFF "\x1b[>4m\x1b[<u"

/* Set once the original settings have actually been captured, so a die() during
 * startup does not restore a struct that was never filled in, and cleared once
 * they are back so the atexit handler is a no-op after die() has already run. */
static int termios_saved = 0;

// colors off and the cursor back, in case we left mid-frame or mid-rain
#define RESET "\x1b[m\x1b[?25h"

// runs from atexit and from fatal signal handlers, so it uses tcsetattr and write
// directly: both are async-signal-safe, and the wrappers die() on failure, which
// calls exit() — undefined behavior from an exit handler
static void restore_terminal(void) {
    if (!termios_saved) return;
    termios_saved = 0;

    ssize_t n = write(STDOUT_FILENO, KEYS_OFF MOUSE_OFF SCREEN_OFF RESET,
                      sizeof(KEYS_OFF MOUSE_OFF SCREEN_OFF RESET) - 1);
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

/* A crash or a kill skips atexit, and would leave the shell in raw mode with no
 * echo and no cursor. SA_RESETHAND puts the default action back, so re-raising
 * still dies the way it would have (core dump and all). */
static void on_fatal_signal(int sig) {
    restore_terminal();
    raise(sig);
}

static void install_fatal_handlers(void) {
    static const int SIGNALS[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT,
                                  SIGTERM, SIGHUP, SIGINT, SIGQUIT};
    struct sigaction sa;
    sa.sa_handler = on_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    for (size_t i = 0; i < sizeof(SIGNALS) / sizeof(SIGNALS[0]); i++) {
        Sigaction(SIGNALS[i], &sa, NULL);
    }
}

void enable_raw_mode(void) {
    Tcgetattr(STDIN_FILENO, &orig_termios);             // get current terminal settings
    termios_saved = 1;
    atexit(restore_terminal);                           // register restore_terminal to be called at exit
    install_fatal_handlers();

    struct termios raw = orig_termios;

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    Tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    // the alternate screen waits: the startup effect runs on the main one first
    Write(STDOUT_FILENO, MOUSE_ON KEYS_ON, sizeof(MOUSE_ON KEYS_ON) - 1);
}

void enter_alt_screen(void) {
    Write(STDOUT_FILENO, SCREEN_ON, sizeof(SCREEN_ON) - 1);
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
