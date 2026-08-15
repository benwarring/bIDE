#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "input.h"
#include "buffer.h"
#include "editor.h"
#include "wrappers.h"

#define CTRL_KEY(k) ((k) & 0x1F)

int editor_read_key(void) {
    unsigned char c;

    if (Read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c != '\x1b') return c;

    // an escape byte with nothing behind it is a real Escape keypress;
    // the VTIME timeout is what makes that distinguishable from a sequence
    unsigned char seq[3];
    if (Read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (Read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (Read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
            if (seq[2] == '~') {
                switch (seq[1]) {
                    case '1': return HOME_KEY;
                    case '3': return DEL_KEY;
                    case '4': return END_KEY;
                    case '5': return PAGE_UP;
                    case '6': return PAGE_DOWN;
                    case '7': return HOME_KEY;
                    case '8': return END_KEY;
                }
            }
        } else {
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
        }
    }

    return '\x1b';
}

static void clamp_cx(void) {
    int len = buffer_line_length(E.cy);
    if (len < 0) len = 0;
    if (E.cx > len) E.cx = len;
}

static void move_left(void) {
    if (E.cx > 0) {
        E.cx--;
    } else if (E.cy > 0) {
        E.cy--;
        E.cx = buffer_line_length(E.cy);
    }
}

static void move_right(void) {
    int len = buffer_line_length(E.cy);
    if (len >= 0 && E.cx < len) {
        E.cx++;
    } else if (E.cy < buffer_num_lines() - 1) {
        E.cy++;
        E.cx = 0;
    }
}

static void move_cursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            move_left();
            break;
        case ARROW_RIGHT:
            move_right();
            break;
        case ARROW_UP:
            if (E.cy > 0) E.cy--;
            clamp_cx();
            break;
        case ARROW_DOWN:
            if (E.cy < buffer_num_lines() - 1) E.cy++;
            clamp_cx();
            break;
    }
}

int editor_process_keypress(int c) {
    switch (c) {
        case CTRL_KEY('x'):
            return 0;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            E.cx = buffer_line_length(E.cy);
            break;

        case PAGE_UP:
            E.cy -= E.screenrows;
            if (E.cy < 0) E.cy = 0;
            clamp_cx();
            break;

        case PAGE_DOWN:
            E.cy += E.screenrows;
            if (E.cy > buffer_num_lines() - 1) E.cy = buffer_num_lines() - 1;
            clamp_cx();
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            move_cursor(c);
            break;
    }

    return 1;
}
