#ifndef TERMINAL_H
#define TERMINAL_H
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>

extern volatile sig_atomic_t resize_pending;

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

/** Raw mode, mouse and key reporting. Stays on the main screen. */
void enable_raw_mode(void);

/** For when there is no startup effect to do it as part of its last frame. */
void enter_alt_screen(void);

int die(const char *s);

int get_window_size(int *rows, int *cols);

void install_winch_handler(void);

void update_window_size(void);

#endif
