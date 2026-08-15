#ifndef TERMINAL_H
#define TERMINAL_H
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>

extern volatile sig_atomic_t resize_pending;

void enable_raw_mode(void);

int die(const char *s);

int get_window_size(int *rows, int *cols);

void install_winch_handler(void);

void update_window_size(void);

#endif
