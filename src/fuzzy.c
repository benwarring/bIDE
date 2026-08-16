#include <string.h>
#include <ctype.h>

#include "fuzzy.h"

#define SCORE_CONSECUTIVE  15
#define SCORE_BOUNDARY     10
#define SCORE_CAMEL         8
#define SCORE_BASENAME      5
#define SCORE_MATCH         1

static int is_boundary(char c) {
    return c == '/' || c == '_' || c == '-' || c == '.' || c == ' ';
}

int fuzzy_match(const char *pattern, const char *candidate, int *score) {
    int total = 0;

    // an empty query ranks everything equally, so the caller's own order shows
    // through: the palette opens on its table order rather than shuffled by length
    if (pattern == NULL || pattern[0] == '\0') {
        if (score) *score = 0;
        return 1;
    }

    const char *base = strrchr(candidate, '/');
    size_t base_at = base ? (size_t)(base - candidate) + 1 : 0;

    size_t pi = 0;
    int last_hit = -2;

    for (size_t ci = 0; candidate[ci]; ci++) {
        if (pattern[pi] == '\0') break;
        if (tolower((unsigned char)candidate[ci]) != tolower((unsigned char)pattern[pi])) continue;

        total += SCORE_MATCH;

        if ((int)ci == last_hit + 1) total += SCORE_CONSECUTIVE;
        if (ci == 0 || is_boundary(candidate[ci - 1])) total += SCORE_BOUNDARY;
        else if (isupper((unsigned char)candidate[ci]) &&
                 islower((unsigned char)candidate[ci - 1])) total += SCORE_CAMEL;
        if (ci >= base_at) total += SCORE_BASENAME;

        last_hit = (int)ci;
        pi++;
    }

    if (pattern[pi] != '\0') return 0;                  // ran out of candidate first

    total -= (int)strlen(candidate) / 4;                // shorter candidates win ties
    if (score) *score = total;
    return 1;
}
