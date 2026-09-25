#ifndef SYNTAX_H
#define SYNTAX_H

/**
 * Highlight types. Each line carries an array of these, one byte per *rendered*
 * character, parallel to its render string and regenerated on the same trigger,
 * so the two can never drift apart.
 *
 * This array is the architecture that matters. A line-at-a-time scanner will
 * eventually lie about anything context-dependent, and the fix is to replace
 * the *producer* of this array with tree-sitter. The renderer, the color
 * mapping, and the search override all stay as they are.
 */
enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD,
    HL_TYPE,
    HL_STRING,
    HL_CHAR,
    HL_NUMBER,
    HL_FUNCTION,
    HL_MATCH,
    HL_CURSOR                   // an extra cursor or the selection, drawn in reverse video
};

#define HL_HIGHLIGHT_NUMBERS   (1 << 0)
#define HL_HIGHLIGHT_STRINGS   (1 << 1)
#define HL_HIGHLIGHT_FUNCTIONS (1 << 2)

/**
 * One language. Adding a language means adding an entry to the table in
 * syntax.c and nothing else; if it ever needs a code change, this struct is
 * wrong.
 */
struct editorSyntax {
    const char *filetype;
    const char **filematch;                 // extensions, NULL-terminated
    const char **keywords;
    const char **types;                     // colored differently from keywords
    const char *singleline_comment_start;
    const char *multiline_comment_start;    // NULL if the language has none
    const char *multiline_comment_end;
    int flags;
};

/**
 * Picks a syntax definition from the filename's extension. NULL filename or an
 * unknown extension means no highlighting.
 */
void syntax_select(const char *filename);

const char *syntax_filetype(void);

/**
 * Recomputes the highlight array for one row, then carries forward: line N's
 * ending comment state is line N+1's starting state, so a change cascades down
 * until the state stops changing. Opening a block comment at the top of a file
 * re-highlights everything below it; typing a letter mid-line stops immediately.
 */
void syntax_update_row(int idx);

void syntax_update_all(void);

int syntax_color(unsigned char hl);

#endif
