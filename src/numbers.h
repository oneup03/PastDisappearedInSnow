#pragma once
#include <stdlib.h>
#include <string.h>

/* Numbers from the data files, read with strtod and strtof rather than sscanf: MinGW's sscanf
 * can read a decimal one bit off (3352.018687, say), and then a loop rendered on Windows hashes
 * and sounds a hair different from one rendered anywhere else. */

/* Up to `most` numbers from `at`, separated by spaces. Returns how many, and where it stopped. */
static inline int ReadNumbers(const char *at, double *out, int most, const char **end) {
    int count = 0;
    while (count < most) {
        char *next;
        double value = strtod(at, &next);
        if (next == at)
            break;
        out[count++] = value;
        at = next;
    }
    if (end)
        *end = at;
    return count;
}

static inline int ReadFloats(const char *at, float *out, int most, const char **end) {
    int count = 0;
    while (count < most) {
        char *next;
        float value = strtof(at, &next);
        if (next == at)
            break;
        out[count++] = value;
        at = next;
    }
    if (end)
        *end = at;
    return count;
}

/* What follows `prefix` at the start of `line`, or NULL. */
static inline const char *After(const char *line, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(line, prefix, n) == 0 ? line + n : NULL;
}
