// SPDX-License-Identifier: MIT
// The portable half of backtrace.h: the frame walker, and the log line built from a backtrace.
#include <stdio.h>

#include "wl/platform/backtrace.h"
#include "wl/platform/platform.h"

#define LOG_DEPTH 18u

uint32_t wl_backtrace_walk(WlBacktraceRead read, void *user, uint32_t sp, uint32_t lr, uint32_t skip,
                           uint32_t *out, uint32_t max) {
    uint32_t stored = 0;
    uint32_t seen = 0;

    if (read == NULL || out == NULL) {
        return 0;
    }
    if (lr != 0) {
        if (seen++ >= skip && stored < max) {
            out[stored++] = lr;
        }
    }
    // (a frame's back chain is the caller's frame; the caller's frame holds the return address
    // into the caller's caller, which is what the frame above reports)
    while (stored < max) {
        uint32_t above, ret;

        if ((sp & 3u) != 0 || !read(user, sp, &above)) {
            break;
        }
        if (above <= sp || (above & 3u) != 0 || above > 0xfffffff8u || !read(user, above + 4u, &ret)) {
            break;
        }
        if (ret == 0) {
            break; // (the first frame's return address is nobody's: the stack's end)
        }
        if (seen++ >= skip) {
            out[stored++] = ret;
        }
        sp = above;
    }
    return stored;
}

void wl_platform_log_backtrace(const char *tag, const char *what, uint32_t skip) {
    uint32_t callers[LOG_DEPTH];
    char line[LOG_DEPTH * 9 + 1];
    uint32_t count = wl_platform_backtrace(callers, LOG_DEPTH, skip + 1u); // (+1: this function's own frame)
    size_t length = 0;

    line[0] = '\0';
    for (uint32_t i = 0; i < count; i++) {
        int n = snprintf(line + length, sizeof line - length, " %08x", (unsigned)callers[i]);
        if (n < 0 || (size_t)n >= sizeof line - length) {
            break;
        }
        length += (size_t)n;
    }
    WL_LOG_E(tag, "%s from%s", what, count ? line : " (no backtrace on this platform)");
    wl_platform_log_flush();
}
