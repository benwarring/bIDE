#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "syntax.h"
#include "editor.h"
#include "wrappers.h"

/* ------------------------------------------------------ the syntax table --- */

static const char *C_extensions[] = {".c", ".h", ".cpp", ".hpp", ".cc", NULL};
static const char *C_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "case", "default", "do",
    "goto", "sizeof", "const", "extern", "volatile", "register", "inline",
    "#include", "#define", "#ifndef", "#ifdef", "#endif", "#if", "#else",
    "#elif", "#undef", "#pragma", NULL
};
static const char *C_types[] = {
    "int", "long", "double", "float", "char", "unsigned", "signed", "void",
    "short", "size_t", "ssize_t", "bool", "FILE", NULL
};

static const char *PY_extensions[] = {".py", NULL};
static const char *PY_keywords[] = {
    "def", "class", "if", "elif", "else", "while", "for", "in", "return",
    "import", "from", "as", "try", "except", "finally", "raise", "with",
    "pass", "break", "continue", "lambda", "yield", "global", "nonlocal",
    "assert", "del", "and", "or", "not", "is", NULL
};
static const char *PY_types[] = {
    "int", "float", "str", "bytes", "bool", "list", "dict", "set", "tuple",
    "None", "True", "False", "self", NULL
};

/* Adding a language is adding an entry here. Python has no block comment, so
 * its multiline markers are NULL and the scanner simply skips that step. */
static const struct editorSyntax HLDB[] = {
    {
        "c", C_extensions, C_keywords, C_types,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_FUNCTIONS
    },
    {
        "python", PY_extensions, PY_keywords, PY_types,
        "#", NULL, NULL,
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_FUNCTIONS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* The active syntax lives on the buffer, so two open files in different
 * languages highlight independently. */
static const struct editorSyntax *cur_syntax(void) {
    Buffer *b = editor_buffer();
    return b ? b->syntax : NULL;
}

void syntax_select(const char *filename) {
    Buffer *b = editor_buffer();
    if (b == NULL) return;

    b->syntax = NULL;
    if (filename == NULL) return;

    const char *ext = strrchr(filename, '.');

    for (size_t j = 0; j < HLDB_ENTRIES; j++) {
        const struct editorSyntax *s = &HLDB[j];
        for (int i = 0; s->filematch[i]; i++) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(filename, s->filematch[i]))) {
                b->syntax = s;
                return;
            }
        }
    }
}

const char *syntax_filetype(void) {
    const struct editorSyntax *s = cur_syntax();
    return s ? s->filetype : "no ft";
}

int syntax_color(unsigned char hl) {
    switch (hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 90;       // grey, so comments recede
        case HL_KEYWORD:   return 35;       // magenta
        case HL_TYPE:      return 32;       // green
        case HL_STRING:    return 33;       // yellow
        case HL_CHAR:      return 33;
        case HL_NUMBER:    return 31;       // red
        case HL_FUNCTION:  return 36;       // cyan
        case HL_MATCH:     return 34;       // blue
        case HL_CURSOR:    return 7;        // reverse video, for extra cursors
        default:           return 39;       // terminal default
    }
}

/* ------------------------------------------------------------- scanner --- */

/**
 * A keyword only counts when bounded by separators. Without this the "int"
 * inside "printf" lights up, which looks obviously broken.
 */
static int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[]{};:&|!?#", c) != NULL;
}

/**
 * Rescans one row. Returns 1 if its ending comment state changed, which is the
 * signal to continue down the file.
 */
static int update_one(int idx) {
    Buffer *b = editor_buffer();
    const struct editorSyntax *SYNTAX = b->syntax;
    erow *row = &b->row[idx];

    row->hl = Realloc(row->hl, (size_t)row->rsize + 1);   // +1: never a zero-size allocation
    memset(row->hl, HL_NORMAL, (size_t)row->rsize);

    int prev_open = row->hl_open_comment;

    if (SYNTAX == NULL) {
        row->hl_open_comment = 0;
        return prev_open != 0;
    }

    const char *scs = SYNTAX->singleline_comment_start;
    const char *mcs = SYNTAX->multiline_comment_start;
    const char *mce = SYNTAX->multiline_comment_end;
    int scs_len = scs ? (int)strlen(scs) : 0;
    int mcs_len = mcs ? (int)strlen(mcs) : 0;
    int mce_len = mce ? (int)strlen(mce) : 0;

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (idx > 0 && b->row[idx - 1].hl_open_comment);  // carried forward

    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment &&
            !strncmp(&row->render[i], scs, (size_t)scs_len)) {
            memset(&row->hl[i], HL_COMMENT, (size_t)(row->rsize - i));
            break;                                      // rest of the line is comment
        }

        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, (size_t)mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, (size_t)mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                }
                i++;
                continue;
            } else if (!strncmp(&row->render[i], mcs, (size_t)mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, (size_t)mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (SYNTAX->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = (in_string == '\'') ? HL_CHAR : HL_STRING;

                // a backslash escapes the next byte, quotes included: without
                // this, "he said \"hi\"" ends early and the rest miscolors
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i + 1] = row->hl[i];
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else if (c == '"' || c == '\'') {
                in_string = c;
                row->hl[i] = (c == '\'') ? HL_CHAR : HL_STRING;
                i++;
                continue;
            }
        }

        if (SYNTAX->flags & HL_HIGHLIGHT_NUMBERS) {
            // a digit straight after a letter belongs to an identifier
            if ((isdigit((unsigned char)c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep) {
            int matched = 0;
            for (int list = 0; list < 2 && !matched; list++) {
                const char **kw = (list == 0) ? SYNTAX->keywords : SYNTAX->types;
                if (kw == NULL) continue;

                for (int j = 0; kw[j]; j++) {
                    int klen = (int)strlen(kw[j]);
                    if (i + klen > row->rsize) continue;
                    if (strncmp(&row->render[i], kw[j], (size_t)klen)) continue;

                    int after = (i + klen == row->rsize) ? '\0' : row->render[i + klen];
                    if (!is_separator(after)) continue;

                    memset(&row->hl[i], list == 0 ? HL_KEYWORD : HL_TYPE, (size_t)klen);
                    i += klen;
                    matched = 1;
                    break;
                }
            }
            if (matched) {
                prev_sep = 0;
                continue;
            }
        }

        // an identifier immediately followed by '(' is a call. This runs after
        // the keyword pass, so `if (`, `while (` and `sizeof(` are already
        // consumed as keywords and never reach here.
        if ((SYNTAX->flags & HL_HIGHLIGHT_FUNCTIONS) && prev_sep &&
            (isalpha((unsigned char)c) || c == '_')) {
            int j = i;
            while (j < row->rsize &&
                   (isalnum((unsigned char)row->render[j]) || row->render[j] == '_')) {
                j++;
            }
            if (j < row->rsize && row->render[j] == '(') {
                memset(&row->hl[i], HL_FUNCTION, (size_t)(j - i));
                i = j;
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator((unsigned char)c);
        i++;
    }

    row->hl_open_comment = in_comment;
    return row->hl_open_comment != prev_open;
}

void syntax_update_row(int idx) {
    Buffer *b = editor_buffer();
    if (b == NULL) return;

    // iterative rather than recursive: opening a block comment at the top of a
    // large file would otherwise recurse once per line
    while (idx >= 0 && idx < b->numrows) {
        if (!update_one(idx)) break;                    // state unchanged: nothing below can differ
        idx++;
    }
}

void syntax_update_all(void) {
    Buffer *b = editor_buffer();
    if (b == NULL) return;

    for (int i = 0; i < b->numrows; i++) {
        update_one(i);
    }
}
