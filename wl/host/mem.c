// SPDX-License-Identifier: MIT
// Portable string duplication (strdup is POSIX, not C11).
#include <stdlib.h>
#include <string.h>

#include "wl/host/base.h"

char *wl_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}
