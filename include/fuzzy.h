#ifndef FUZZY_H
#define FUZZY_H

/**
 * Subsequence matching: every character of `pattern` appears in `candidate`, in
 * order, not necessarily adjacent. Case-insensitive.
 *
 * Returns 1 on a match and writes a score to *score (higher is better), or 0.
 * An empty pattern matches everything.
 *
 * Ranking is most of what makes a fuzzy finder feel good, so the score rewards
 * consecutive runs, matches at word boundaries, matches in the filename rather
 * than the directory path, and shorter candidates on ties. This is the family of
 * heuristics fzf and Sublime use; diminishing returns arrive quickly.
 */
int fuzzy_match(const char *pattern, const char *candidate, int *score);

#endif
