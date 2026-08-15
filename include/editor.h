#ifndef EDITOR_H
#define EDITOR_H

#include <time.h>

typedef struct erow {
    int size;
    char *chars;
} erow;

typedef struct {
    int cx, cy;             // cursor position in the file: line index, byte index within line
    int rx;                 // cursor's rendered column (tabs expanded); equals cx until tabs exist
    int rowoff, coloff;     // which file line/column is at the top-left of the screen
    int screenrows, screencols; // text area size (status bar + message line already excluded)
    int numrows;
    erow *row;
    char *filename;
    int crlf;                // 1 if the loaded file used CRLF line endings, for M2 save
    char statusmsg[80];
    time_t statusmsg_time;
} EditorConfig;

extern EditorConfig E;

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

void editor_init(void);

#endif
