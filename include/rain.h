#ifndef RAIN_H
#define RAIN_H

/**
 * The startup effect. Matrix rain falls over whatever the shell left on the main
 * screen, and each column's tail uncovers the editor's first frame. Once every
 * column has cleared, one write swaps to the alternate screen showing that same
 * frame, so the handoff is invisible.
 *
 * ASCII art from `art_path` (NULL: the built-in assets/bIDE.txt) sits centered
 * above the rain, then dissolves into the editor underneath. Missing or too big
 * art is silently left out.
 *
 * Always leaves the alternate screen up with the frame drawn, whether the effect
 * ran, was skipped with a keypress, or could not run (not a terminal).
 */
void rain_play(const char *art_path);

#endif
